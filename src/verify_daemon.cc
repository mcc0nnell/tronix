// SPDX-License-Identifier: Apache-2.0
// Persistent, operator-pinned Celix verifier. No model execution authority.
#include "tronix_answer_verify.h"
#include <celix_bundle_context.h>
#include <celix_constants.h>
#include <celix_framework.h>
#include <celix_framework_factory.h>
#include <celix_properties.h>
#include <msgpack.hpp>
#include <openssl/evp.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>

namespace {
using Digest=std::array<uint8_t,32>;
using P=msgpack::packer<msgpack::sbuffer>;
std::atomic<bool> stop{false};
void signal_stop(int){stop.store(true);}
struct Error:std::runtime_error{using std::runtime_error::runtime_error;};
void key(P& pk,const char* v){pk.pack(std::string(v));}
bool valid_id(const std::string& s){
  if(s.empty()||s.size()>64)return false;
  for(unsigned char c:s)if(!(c>='a'&&c<='z')&&!(c>='A'&&c<='Z')&&
                            !(c>='0'&&c<='9')&&c!='_'&&c!='-')return false;
  return true;
}
bool valid_sha(const std::string& s){
  if(s.size()!=64)return false;
  for(char c:s)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
  return true;
}
Digest sha(const uint8_t* v,size_t n){
  Digest d{};unsigned int len=0;
  if(!EVP_Digest(v,n,d.data(),&len,EVP_sha256(),nullptr)||len!=32)throw Error("SHA-256 failure");
  return d;
}
std::string hex(const Digest& d){
  static constexpr char n[]="0123456789abcdef";
  std::string s;s.reserve(64);
  for(uint8_t b:d){s+=n[b>>4];s+=n[b&15];}return s;
}
Digest unhex(const std::string& s){
  Digest d{};
  for(size_t i=0;i<32;i++){
    auto val=[](char c){return c<='9'?c-'0':c-'a'+10;};
    d[i]=static_cast<uint8_t>(val(s[2*i])*16+val(s[2*i+1]));
  }
  return d;
}
std::vector<uint8_t> read_regular(const std::string& name,size_t limit,bool private_file){
  namespace fs=std::filesystem;fs::path p(name);
  if(!p.is_absolute()||p.lexically_normal()!=p)throw Error("noncanonical path");
  fs::path path("/");
  for(const auto& part:p.relative_path()){
    if(part.empty()||part=="."||part=="..")throw Error("unsafe path segment");
    path/=part;struct stat s{};
    if(lstat(path.c_str(),&s)!=0||S_ISLNK(s.st_mode))throw Error("symlink/missing path");
  }
  int fd=open(name.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
  if(fd<0)throw Error("file open denied");
  struct Guard{int fd;~Guard(){close(fd);}} g{fd};
  struct stat s{};
  if(fstat(fd,&s)!=0||!S_ISREG(s.st_mode)||s.st_size<0||
     static_cast<uint64_t>(s.st_size)>limit)throw Error("file type/size denied");
  if(private_file&&(s.st_uid!=getuid()||(s.st_mode&077)!=0||s.st_nlink!=1))
    throw Error("oracle must be owner-private single-link");
  std::vector<uint8_t> out;out.reserve(s.st_size);
  uint8_t buf[4096];
  for(;;){
    ssize_t n=read(fd,buf,sizeof(buf));if(n<0&&errno==EINTR)continue;
    if(n<0)throw Error("file read error");
    if(n==0)break;
    if(out.size()+static_cast<size_t>(n)>limit)throw Error("file exceeded byte limit");
    out.insert(out.end(),buf,buf+n);
  }
  struct stat after{};
  if(fstat(fd,&after)!=0||after.st_size!=s.st_size||
     after.st_mtim.tv_sec!=s.st_mtim.tv_sec||
     after.st_mtim.tv_nsec!=s.st_mtim.tv_nsec||
     out.size()!=static_cast<size_t>(s.st_size))throw Error("file changed");
  return out;
}
std::map<std::string,std::string> read_oracles(const std::vector<uint8_t>& bytes){
  if(bytes.empty()||bytes.back()!='\n')throw Error("oracle manifest must end in newline");
  std::map<std::string,std::string> out;
  const std::string all(bytes.begin(),bytes.end());size_t start=0;
  while(start<all.size()){
    size_t end=all.find('\n',start);
    if(end==std::string::npos)throw Error("malformed oracle");
    auto line=all.substr(start,end-start);
    auto sep=line.find(' ');
    if(sep==std::string::npos||line.find(' ',sep+1)!=std::string::npos)
      throw Error("malformed oracle row");
    auto id=line.substr(0,sep),expected=line.substr(sep+1);
    if(!valid_id(id)||!valid_sha(expected)||!out.emplace(id,expected).second||
       out.size()>128)throw Error("duplicate/noncanonical oracle");
    start=end+1;
  }
  if(out.empty())throw Error("no oracles");
  return out;
}
std::map<std::string,std::string> arguments(int n,char** v){
  if(n!=9)throw Error("expected --socket --oracle --bundle --bundle-sha256");
  std::map<std::string,std::string> a;
  for(int i=1;i<n;i+=2){
    std::string k=v[i];
    if(k!="--socket"&&k!="--oracle"&&k!="--bundle"&&k!="--bundle-sha256")
      throw Error("unexpected argument");
    if(!a.emplace(k,v[i+1]).second)throw Error("duplicate argument");
  }
  if(a.size()!=4||!valid_sha(a.at("--bundle-sha256")))throw Error("bad bundle pin");
  return a;
}
bool exact(int fd,void* buf,size_t n,bool send_mode){
  char* p=static_cast<char*>(buf);
  while(n){
    ssize_t k=send_mode?send(fd,p,n,MSG_NOSIGNAL):recv(fd,p,n,0);
    if(k<0&&errno==EINTR)continue;
    if(k<=0)return false;p+=k;n-=k;
  }return true;
}
struct Query{std::string action,task;std::vector<uint8_t> answer;};
Query parse(const std::vector<char>& bytes){
  size_t consumed=0;
  auto oh=msgpack::unpack(bytes.data(),bytes.size(),consumed);
  if(consumed!=bytes.size())throw Error("trailing messagepack data");
  const auto& obj=oh.get();if(obj.type!=msgpack::type::MAP)throw Error("map required");
  Query q;std::set<std::string> keys;
  for(uint32_t i=0;i<obj.via.map.size;i++){
    auto& e=obj.via.map.ptr[i];
    if(e.key.type!=msgpack::type::STR)throw Error("bad key");
    const auto k=e.key.as<std::string>();
    if(!keys.insert(k).second)throw Error("duplicate key");
    if(k=="v"){
      if(e.val.type!=msgpack::type::POSITIVE_INTEGER||e.val.as<uint64_t>()!=2)
        throw Error("bad version");
    }else if(k=="action"||k=="taskId"){
      if(e.val.type!=msgpack::type::STR)throw Error("bad string");
      if(k=="action")q.action=e.val.as<std::string>();else q.task=e.val.as<std::string>();
    }else if(k=="answer"){
      if(e.val.type!=msgpack::type::BIN||
         e.val.via.bin.size>TRONIX_ANSWER_MAX_BYTES)throw Error("bad binary");
      const uint8_t* p=reinterpret_cast<const uint8_t*>(e.val.via.bin.ptr);
      if(e.val.via.bin.size>0)q.answer.assign(p,p+e.val.via.bin.size);
      else q.answer.clear();
    }else throw Error("unknown field");
  }
  if(q.action=="verify"){
    if(keys!=std::set<std::string>{"v","action","taskId","answer"}||
       !valid_id(q.task))throw Error("bad verify shape");
  }else if(q.action=="stats"){
    if(keys!=std::set<std::string>{"v","action"})throw Error("bad stats shape");
  }else throw Error("action denied");
  return q;
}
struct Call{
  const uint8_t* data;size_t len;Digest expected,actual{};
  bool invoked=false;int result=TRONIX_ANSWER_UNVERIFIABLE;
};
void invoke(void* handle,void* svc){
  auto& call=*static_cast<Call*>(handle);
  auto* s=static_cast<tronix_answer_verify_service_t*>(svc);
  if(!s||!s->verify)return;
  call.invoked=true;
  call.result=s->verify(s->handle,call.data,call.len,
                        call.expected.data(),call.actual.data());
}
struct State{
  celix_bundle_context_t* ctx;
  std::map<std::string,std::string> expected;
  std::string manifest_hash;
  uint64_t count=0,ok=0,rejected=0,uncertain=0,execution_ns=0;
};
std::string reply(State& state,const Query& q){
  msgpack::sbuffer buffer;P pk(buffer);
  if(q.action=="stats"){
    pk.pack_map(9);
    key(pk,"v");pk.pack(2);key(pk,"ok");pk.pack(true);
    key(pk,"protocol");pk.pack(std::string("tronix.typed-verification/v2"));
    key(pk,"oracleManifestSha256");pk.pack(state.manifest_hash);
    key(pk,"requests");pk.pack(state.count);
    key(pk,"verified");pk.pack(state.ok);
    key(pk,"rejected");pk.pack(state.rejected);
    key(pk,"reviewRequired");pk.pack(state.uncertain);
    key(pk,"totalVerifyNs");pk.pack(state.execution_ns);
  }else{
    const auto start=std::chrono::steady_clock::now();
    auto found_oracle=state.expected.find(q.task);
    int status=TRONIX_ANSWER_UNVERIFIABLE;
    const auto actual=hex(sha(q.answer.data(),q.answer.size()));
    std::string wanted;
    if(found_oracle!=state.expected.end()){
      wanted=found_oracle->second;
      const uint8_t* payload=q.answer.empty()?reinterpret_cast<const uint8_t*>(""):q.answer.data();
      Call call{payload,q.answer.size(),unhex(wanted)};
      celix_service_use_options_t opts{};
      opts.filter.serviceName=TRONIX_ANSWER_VERIFY_SERVICE;
      opts.filter.versionRange="[1.0.0,2.0.0)";
      opts.callbackHandle=&call;opts.use=invoke;
      bool found=celix_bundleContext_useServiceWithOptions(state.ctx,&opts);
      if(found&&call.invoked&&hex(call.actual)==actual)status=call.result;
    }
    const auto elapsed=static_cast<uint64_t>(std::chrono::duration_cast<
      std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());
    state.count++;state.execution_ns+=elapsed;
    if(status==TRONIX_ANSWER_VERIFIED)state.ok++;
    else if(status==TRONIX_ANSWER_REJECTED)state.rejected++;
    else state.uncertain++;
    const std::string decision=status==TRONIX_ANSWER_VERIFIED?"VERIFIED":
                               status==TRONIX_ANSWER_REJECTED?"REJECTED":"UNVERIFIABLE";
    pk.pack_map(15);
    key(pk,"v");pk.pack(2);key(pk,"ok");pk.pack(true);
    key(pk,"protocol");pk.pack(std::string("tronix.typed-verification/v2"));
    key(pk,"taskId");pk.pack(q.task);key(pk,"status");pk.pack(decision);
    key(pk,"route");pk.pack(status==TRONIX_ANSWER_UNVERIFIABLE?
                        std::string("atlas-review-required"):std::string("none"));
    key(pk,"expectedSha256");pk.pack(wanted);
    key(pk,"actualSha256");pk.pack(actual);
    key(pk,"oracleManifestSha256");pk.pack(state.manifest_hash);
    key(pk,"answerBytes");pk.pack(static_cast<uint32_t>(q.answer.size()));
    key(pk,"verifyNs");pk.pack(elapsed);
    key(pk,"humanReview");pk.pack(std::string("pending"));
    key(pk,"executionAuthorized");pk.pack(false);
    key(pk,"objectiveTruthEstablished");pk.pack(false);
    key(pk,"published");pk.pack(false);
  }
  return std::string(buffer.data(),buffer.size());
}
std::string invalid(){
  msgpack::sbuffer b;P pk(b);pk.pack_map(4);
  key(pk,"v");pk.pack(2);key(pk,"ok");pk.pack(false);
  key(pk,"error");pk.pack(std::string("INVALID_REQUEST"));
  key(pk,"executionAuthorized");pk.pack(false);
  return std::string(b.data(),b.size());
}
void client(int fd,State& state){
  timeval tv{2,0};
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
  setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
  struct ucred peer{};socklen_t len=sizeof(peer);
  if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&len)!=0||peer.uid!=getuid())return;
  uint32_t be=0;
  if(!exact(fd,&be,4,false))return;
  uint32_t n=ntohl(be);
  if(n==0||n>TRONIX_ANSWER_MAX_BYTES+1024u)return;
  std::vector<char> data(n);
  if(!exact(fd,data.data(),data.size(),false))return;
  std::string result;
  try{result=reply(state,parse(data));}catch(const std::exception&){result=invalid();}
  if(result.size()>2048)return;
  be=htonl(static_cast<uint32_t>(result.size()));
  if(exact(fd,&be,4,true))exact(fd,result.data(),result.size(),true);
}
} // namespace

int main(int argc,char** argv){
  try{
    auto a=arguments(argc,argv);
    const auto manifest=read_regular(a.at("--oracle"),8192,true);
    auto oracles=read_oracles(manifest);
    const std::string manifest_hash=hex(sha(manifest.data(),manifest.size()));
    const auto zip=read_regular(a.at("--bundle"),4u*1024u*1024u,false);
    if(hex(sha(zip.data(),zip.size()))!=a.at("--bundle-sha256"))
      throw Error("untrusted verifier bundle");
    const auto socket=a.at("--socket");
    const std::filesystem::path sp(socket);
    if(!sp.is_absolute()||socket.size()>=sizeof(sockaddr_un::sun_path)||
       !std::filesystem::exists(sp.parent_path()))throw Error("bad socket path");
    std::filesystem::path ancestor("/");
    for(const auto& part:sp.parent_path().relative_path()){
      if(part.empty()||part=="."||part=="..")throw Error("unsafe socket path");
      ancestor/=part;struct stat s{};
      if(lstat(ancestor.c_str(),&s)!=0||S_ISLNK(s.st_mode))
        throw Error("symlinked socket parent");
    }
    struct stat parent{};
    if(lstat(sp.parent_path().c_str(),&parent)!=0||!S_ISDIR(parent.st_mode)||
       parent.st_uid!=getuid()||(parent.st_mode&077)!=0)
      throw Error("socket directory must be owner-private 0700");
    struct stat prior{};
    if(lstat(socket.c_str(),&prior)==0||errno!=ENOENT)throw Error("socket path already exists");
    char temp[]="/tmp/tronix-typed-XXXXXX";
    char* p=mkdtemp(temp);if(!p)throw Error("no cache");
    std::string scratch=p;
    struct Cleanup{std::string path;~Cleanup(){std::error_code ec;
      std::filesystem::remove_all(path,ec);}} cleanup{scratch};
    std::string bundle=scratch+"/verifier.zip";
    int fd=open(bundle.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
    if(fd<0)throw Error("stage create failed");
    size_t pos=0;
    while(pos<zip.size()){
      ssize_t n=write(fd,zip.data()+pos,zip.size()-pos);
      if(n<0&&errno==EINTR)continue;
      if(n<=0){close(fd);throw Error("stage write failed");}pos+=n;
    }
    if(fsync(fd)!=0){close(fd);throw Error("stage sync failed");}
    if(close(fd)!=0)throw Error("stage close failed");
    const auto staged=read_regular(bundle,4u*1024u*1024u,false);
    if(hex(sha(staged.data(),staged.size()))!=a.at("--bundle-sha256"))
      throw Error("stage pin mismatch");
    std::string cache=scratch+"/framework";
    if(!std::filesystem::create_directory(cache))throw Error("cache create failed");
    auto* props=celix_properties_create();
    celix_properties_set(props,CELIX_FRAMEWORK_CACHE_DIR,cache.c_str());
    celix_properties_set(props,"CELIX_LOGGING_DEFAULT_ACTIVE_LOG_LEVEL","warning");
    celix_properties_setBool(props,CELIX_FRAMEWORK_CLEAN_CACHE_DIR_ON_CREATE,true);
    auto* framework=celix_frameworkFactory_createFramework(props);
    if(!framework)throw Error("Celix unavailable");
    struct Holder{celix_framework_t* p;~Holder(){celix_frameworkFactory_destroyFramework(p);}} hold{framework};
    auto* ctx=celix_framework_getFrameworkContext(framework);
    if(celix_bundleContext_installBundle(ctx,bundle.c_str(),true)<0)
      throw Error("bundle install refused");
    bool available=false;celix_service_use_options_t opts{};
    opts.filter.serviceName=TRONIX_ANSWER_VERIFY_SERVICE;
    opts.filter.versionRange="[1.0.0,2.0.0)";
    opts.callbackHandle=&available;
    opts.use=[](void* h,void* svc){
      auto* s=static_cast<tronix_answer_verify_service_t*>(svc);
      *static_cast<bool*>(h)=s&&s->verify;};
    if(!celix_bundleContext_useServiceWithOptions(ctx,&opts)||!available)
      throw Error("verifier service missing");
    int listener=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(listener<0)throw Error("socket failed");
    bool bound=false;
    struct SocketGuard{int fd;std::string path;bool* bound;
      ~SocketGuard(){close(fd);if(*bound)unlink(path.c_str());}} guard{listener,socket,&bound};
    sockaddr_un addr{};addr.sun_family=AF_UNIX;std::strcpy(addr.sun_path,socket.c_str());
    mode_t um=umask(0077);
    int rc=bind(listener,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
    umask(um);if(rc!=0)throw Error("bind failed");
    bound=true;
    if(chmod(socket.c_str(),0600)!=0||listen(listener,32)!=0)
      throw Error("listen failed");
    State state{ctx,std::move(oracles),manifest_hash};
    std::signal(SIGINT,signal_stop);std::signal(SIGTERM,signal_stop);
    std::printf("TRONIX_VERIFY_READY %s\n",socket.c_str());std::fflush(stdout);
    while(!stop.load()){
      pollfd pfd{listener,POLLIN,0};int n=poll(&pfd,1,100);
      if(n==0||(n<0&&errno==EINTR))continue;
      if(n<0||(pfd.revents&(POLLERR|POLLHUP|POLLNVAL)))break;
      if(!(pfd.revents&POLLIN))continue;
      int c=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);
      if(c<0){if(errno==EINTR)continue;break;}
      client(c,state);close(c);
    }
    return 0;
  }catch(const std::exception& e){
    std::fprintf(stderr,"TRONIX_VERIFY_FAIL_CLOSED: %s\n",e.what());
    return 3;
  }
}
