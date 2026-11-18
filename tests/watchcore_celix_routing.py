#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real pinned Celix cooldown/routing lifecycle, independent Python parity.

External provider probing and source acquisition are normalized fixtures.
The actual Atlas Watcher Python decision functions remain unmodified.
Neither this suite nor the service reads or alters live cooldown state.
"""
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import types
from types import SimpleNamespace
from unittest.mock import patch


def tool(host, bundle, operation, payload, *, pin=None):
    env = {
        "PATH": "/usr/bin:/bin",
        "WATCHCORE_CELIX_BUNDLE": str(bundle),
        "WATCHCORE_CELIX_BUNDLE_SHA256": (
            pin if pin is not None else hashlib.sha256(bundle.read_bytes()).hexdigest()
        ),
    }
    return subprocess.run(
        [str(host), operation],
        input=json.dumps(payload, separators=(",", ":")) if not isinstance(payload, str) else payload,
        text=True, capture_output=True, check=False, timeout=5, env=env,
    )


def run(host, bundle, operation, payload):
    result=tool(host,bundle,operation,payload)
    assert result.returncode == 0, (operation,payload,result.stderr)
    return json.loads(result.stdout)


def state(vendor,until,set_at=900,consecutive=1):
    return {"vendor":vendor,"until":until,"set_at":set_at,"consecutive":consecutive}


def cooldown_case(*, op="active",vendor="alpha",now=1000,states=None,
                  reset=None,strong=False,policy=None):
    return {
        "op":op,"vendor":vendor,"now":now,
        "states":[] if states is None else states,
        "limit":{"reset":reset,"strong":strong},
        "policy": policy or {
            "base_seconds":3600,"max_seconds":21600,
            "strong_reset_max_seconds":691200,
        },
    }


def routing_case(*,op="pick",now=1000,agents=None,states=None,exclude=None,
                 seen=None,require_unseen=False,roles=None):
    return {
        "op":op,"now":now,
        "agents":[] if agents is None else agents,
        "states":[] if states is None else states,
        "exclude":[] if exclude is None else exclude,
        "seen":[] if seen is None else seen,
        "require_unseen":require_unseen,
        "roles":[] if roles is None else roles,
    }


def agent(name,vendor,available=True):
    return {"name":name,"vendor":vendor,"available":available}


def public_tests(host,bundle,wrong):
    checks=[
        (cooldown_case(),{"has_state":False,"until":None,"set_at":None,"consecutive":0}),
        (cooldown_case(states=[state("alpha",2000)]),
         {"has_state":True,"until":2000,"set_at":900,"consecutive":1}),
        (cooldown_case(states=[state("*",3000)]),
         {"has_state":True,"until":3000,"set_at":900,"consecutive":1}),
        (cooldown_case(states=[state("alpha",900),state("*",3000)]),
         {"has_state":True,"until":3000,"set_at":900,"consecutive":1}),
        (cooldown_case(states=[state("alpha",2000),state("*",3000)]),
         {"has_state":True,"until":2000,"set_at":900,"consecutive":1}),
        (cooldown_case(op="record"),{"has_state":True,"until":4600,"set_at":1000,"consecutive":1}),
        (cooldown_case(op="record",now=1100,states=[state("alpha",2000,900)]),
         {"has_state":True,"until":8300,"set_at":1100,"consecutive":2}),
        (cooldown_case(op="record",now=1100,reset=800000,strong=True),
         {"has_state":True,"until":692300,"set_at":1100,"consecutive":1}),
        (cooldown_case(op="record",now=1100,reset=800000,strong=False),
         {"has_state":True,"until":22700,"set_at":1100,"consecutive":1}),
        (cooldown_case(op="record",now=1100,reset=1200,strong=True),
         {"has_state":True,"until":1200,"set_at":1100,"consecutive":1}),
    ]
    for request,expected in checks:
        assert run(host,bundle,"cooldown",request)==expected,(request,expected)
    print(f"PASS {len(checks)} real-Celix cooldown known-answer cases")
    agents=[agent("a","alpha"),agent("b","beta")]
    cases=[
        (routing_case(agents=agents),0,"none",None,False,False),
        (routing_case(agents=agents,states=[state("alpha",2000)]),
         1,"none",None,False,False),
        (routing_case(agents=agents,seen=["alpha"],exclude=["b"]),0,"none",None,False,False),
        (routing_case(agents=agents,seen=["alpha"],exclude=["a"]),1,"none",None,False,False),
        (routing_case(agents=agents,seen=["alpha"],require_unseen=True),
         1,"none",None,False,False),
        (routing_case(agents=agents,seen=["alpha","beta"],require_unseen=True),
         -1,"missing",None,False,False),
        (routing_case(agents=[agent("a","alpha",False)]),
         -1,"missing",None,False,False),
        (routing_case(agents=[agent("a","alpha")],states=[state("alpha",2000)]),
         -1,"cooldown",None,False,False),
        (routing_case(agents=agents,op="roles",roles=[{"offset":0,"length":2}],
                      states=[state("alpha",2000)]),
         -1,"none",2000,False,False),
        (routing_case(agents=agents,op="roles",
                      roles=[{"offset":0,"length":1},{"offset":1,"length":1}],
                      states=[state("alpha",2000)]),
         -1,"none",2000,False,True),
        (routing_case(agents=agents,op="roles",
                      roles=[{"offset":0,"length":1},{"offset":1,"length":1}],
                      states=[state("alpha",2000),state("beta",2400)]),
         -1,"none",2400,True,True),
        (routing_case(agents=agents,op="roles",
                      roles=[{"offset":0,"length":1},{"offset":1,"length":1}]),
         -1,"none",None,False,False),
    ]
    for request,index,reason,ready,stop,skip in cases:
        expected={"selected_index":index,"reason":reason,"ready_at":ready,
                  "stop_pass":stop,"skip_item":skip}
        assert run(host,bundle,"routing",request)==expected,(request,expected)
    print(f"PASS {len(cases)} real-Celix routing known-answer cases")

    cases_bad=[
        ("cooldown", "{"),
        ("cooldown", "null"),
        ("cooldown", cooldown_case(states=[state("alpha",2000),state("alpha",3000)])),
        ("cooldown", cooldown_case(vendor="alpha beta")),
        ("cooldown", cooldown_case(states=[state("alpha",2000,consecutive=-1)])),
        ("cooldown", cooldown_case(states=[state("alpha",2000,consecutive=2**64-1)])),
        ("cooldown", cooldown_case(states=[state("alpha",2000,consecutive=1.5)])),
        ("cooldown", cooldown_case(now="1000")),
        ("cooldown", cooldown_case(policy={
            "base_seconds":-1,"max_seconds":120,
            "strong_reset_max_seconds":300,
        })),
        ("cooldown", cooldown_case(strong="yes")),
        ("cooldown", cooldown_case(op="unknown")),
        ("cooldown", cooldown_case(states=[{"vendor":"alpha","until":1000}])),
        ("cooldown", cooldown_case(states=[state("alpha",1000)]*129)),
        ("routing", "{"),
        ("routing", "[]"),
        ("routing", routing_case(agents=[agent("a","beta",available=1)])),
        ("routing", routing_case(agents=[agent("a","beta",available=False)],
                                 roles=[{"offset":1,"length":1}],op="roles")),
        ("routing", routing_case(agents=[agent("a","beta",available=False)],
                                 roles=[{"offset":0,"length":2}],op="roles")),
        ("routing", routing_case(agents=[agent("a","beta")],roles=[{"offset":0,"length":1}])),
        ("routing", routing_case(agents=[agent("a","beta")],op="roles",exclude=["a"])),
        ("routing", routing_case(agents=[agent("a","beta")],op="roles",seen=["beta"])),
        ("routing", routing_case(op="pick",require_unseen=5)),
        ("routing", routing_case(states=[state("alpha",2000),state("alpha",3000)])),
        ("routing", routing_case(agents=[agent("bad name","beta")])),
        ("routing", routing_case(agents=[agent("a","beta")],
                                 roles=[{"offset":-1,"length":2}],op="roles")),
        ("routing", routing_case(agents=[agent("a","beta")],
                                 roles=[{"offset":0,"length":-1}],op="roles")),
        ("cooldown", " " * 65537),
        ("routing", " " * 65537),
        ("routing", '{"op":"pick","op":"roles"}'),
    ]
    for operation,payload in cases_bad:
        result=tool(host,bundle,operation,payload)
        assert result.returncode and not result.stdout.strip(), (
            operation,payload,result.stdout,result.stderr,
        )
    for operation,payload in [("cooldown",cooldown_case()),
                              ("routing",routing_case())]:
        wrong_pin=tool(host,bundle,operation,payload,pin="0"*64)
        assert wrong_pin.returncode and not wrong_pin.stdout.strip()
        wrong_service=tool(host,wrong,operation,payload)
        assert wrong_service.returncode and not wrong_service.stdout.strip()
    with tempfile.TemporaryDirectory(prefix="routing-fail-closed-") as td:
        link=Path(td) / "symlink.zip"
        link.symlink_to(bundle)
        result=tool(host,link,"routing",routing_case())
        assert result.returncode and not result.stdout.strip()
    print(f"PASS {len(cases_bad)+5} bounded-schema/pin/symlink/wrong-service cases fail closed")


def python_parity(host,bundle,source):
    root=source/"integrations/atlas-watcher/src"
    sys.path.insert(0,str(root))
    config=types.ModuleType("atlas_watcher.config")
    config.settings=SimpleNamespace()
    config.AGENT_COMMAND_FIELDS={}
    config.AGENT_VENDORS={}
    log=types.ModuleType("atlas_watcher.log")
    log.log=lambda *_args:None
    paths=types.ModuleType("atlas_watcher.paths")
    paths.cooldown_file=lambda:Path("/unused-cooldown-fixture")
    for name,module in [
        ("atlas_watcher.config",config),
        ("atlas_watcher.log",log),
        ("atlas_watcher.paths",paths),
    ]:
        sys.modules[name]=module
    from atlas_watcher import agents,cooldown
    rng=random.Random(0xCE11A21)
    vendors=["alpha","beta","gamma","delta"]
    count=0

    def states_for(now):
        states=[]
        for vendor in vendors+["*"]:
            if rng.random()<0.5:
                states.append(state(vendor,now+rng.randint(-3000,8000),
                                    now-rng.randint(0,1000),rng.randrange(1,6)))
        return states

    def loaded(rows):
        return {
            r["vendor"]: {
                "until_ts":r["until"],"set_at":r["set_at"],
                "consecutive":r["consecutive"],"reason":"normalized",
            }
            for r in rows
        }

    for i in range(240):
        now=1000000+rng.randint(0,80000)
        rows=states_for(now)
        vendor=rng.choice(vendors)
        op=rng.choice(["active","record"])
        reset=now+rng.randint(-1500,600000) if rng.random()<0.5 else None
        policy={
            "base_seconds":rng.choice([0,30,900,3600]),
            "max_seconds":rng.choice([0,200,3600,21600]),
            "strong_reset_max_seconds":rng.choice([3600,21600,691200]),
        }
        strong=bool(rng.getrandbits(1))
        request=cooldown_case(op=op,vendor=vendor,now=now,states=rows,
                              reset=reset,strong=strong,policy=policy)
        data=loaded(rows)
        settings=SimpleNamespace(
            agent_limit_backoff_secs=policy["base_seconds"],
            agent_limit_backoff_max_secs=policy["max_seconds"],
            agent_limit_reset_max_secs=policy["strong_reset_max_seconds"],
        )
        with patch.object(cooldown,"_load",lambda:dict(data)), \
             patch.object(cooldown,"_save",lambda *_args:None), \
             patch.object(cooldown.time,"time",lambda:now), \
             patch.object(cooldown,"settings",settings):
            if op=="active":
                expected=cooldown.active_for(vendor)
            else:
                expected=cooldown.record(vendor,cooldown.LimitInfo(
                    reason="normalized",reset_ts=reset,strong=strong))
        output={
            "has_state":expected is not None,
            "until":expected["until_ts"] if expected else None,
            "set_at":expected["set_at"] if expected else None,
            "consecutive":expected["consecutive"] if expected else 0,
        }
        assert run(host,bundle,"cooldown",request)==output,(i,request,output)
        count+=1
    print(f"PASS {count} Celix cooldown decisions match actual Python cooldown functions")

    for i in range(300):
        now=1000000+rng.randint(0,100000)
        states=states_for(now)
        candidate_count=rng.randrange(9)
        candidate_list=[
            agent(f"a{j}",rng.choice(vendors),rng.random()>0.26)
            for j in range(candidate_count)
        ]
        names=[a["name"] for a in candidate_list]
        vendor_of={a["name"]:a["vendor"] for a in candidate_list}
        available={a["name"]:a["available"] for a in candidate_list}
        seen=set(rng.sample(vendors,rng.randrange(len(vendors)+1)))
        exclude=set(rng.sample(names,rng.randrange(len(names)+1)))
        require_unseen=bool(rng.getrandbits(1))
        request=routing_case(
            now=now,agents=candidate_list,states=states,
            exclude=sorted(exclude),seen=sorted(seen),
            require_unseen=require_unseen,
        )
        with patch.object(cooldown,"_load",lambda:loaded(states)), \
             patch.object(cooldown.time,"time",lambda:now), \
             patch.object(agents,"vendor_of",lambda name:vendor_of[name]), \
             patch.object(agents,"binary_available",lambda name:available[name]):
            agent_name,reason=agents.pick(
                names,exclude_agents=exclude,seen_vendors=seen,
                require_unseen_vendor=require_unseen,
            )
        expected={
            "selected_index":names.index(agent_name) if agent_name else -1,
            "reason":reason or "none",
            "ready_at":None,"stop_pass":False,"skip_item":False,
        }
        assert run(host,bundle,"routing",request)==expected,(i,request,expected)
    print("PASS 300 Celix routing picks match actual Python agents.pick")

    for i in range(160):
        now=1000000+rng.randint(0,100000)
        states=states_for(now)
        all_agents=[]
        role_list=[]
        for role in range(rng.randrange(1,6)):
            length=rng.randrange(0,5)
            offset=len(all_agents)
            all_agents.extend([
                agent(f"r{role}a{j}",rng.choice(vendors),rng.random()>0.28)
                for j in range(length)
            ])
            role_list.append({"offset":offset,"length":length})
        vendor_of={a["name"]:a["vendor"] for a in all_agents}
        available={a["name"]:a["available"] for a in all_agents}
        role_names=[f"role{i}" for i in range(len(role_list))]
        role_candidates={
            role_names[k]:[
                agent["name"] for agent in
                all_agents[r["offset"]:r["offset"]+r["length"]]
            ]
            for k,r in enumerate(role_list)
        }
        request=routing_case(
            op="roles",now=now,states=states,agents=all_agents,
            roles=role_list,
        )
        with patch.object(cooldown,"_load",lambda:loaded(states)), \
             patch.object(cooldown.time,"time",lambda:now), \
             patch.object(agents,"vendor_of",lambda name:vendor_of[name]), \
             patch.object(agents,"binary_available",lambda name:available[name]), \
             patch.object(agents,"role_candidates",lambda role:role_candidates[role]):
            ready=agents.roles_ready_at(role_names)
            picks=[agents.pick(role_candidates[role]) for role in role_names]
        stop=not any(name is not None for name,_ in picks) and any(
            reason=="cooldown" for _,reason in picks
        )
        skip=any(reason=="cooldown" for _,reason in picks)
        expected={
            "selected_index":-1,"reason":"none","ready_at":ready,
            "stop_pass":stop,"skip_item":skip,
        }
        assert run(host,bundle,"routing",request)==expected,(i,request,expected)
    print("PASS 160 Celix role readiness decisions match actual Python agents.roles_ready_at")
    print("PASS 160 derived stop/skip decisions from actual Python agents.pick")


def main():
    if len(sys.argv) not in (4,5):
        raise SystemExit(
            "usage: test.py ABS_CELIX_HOST ABS_WATCHCORE_BUNDLE ABS_OTHER_BUNDLE [ABS_RIZZER_ROOT]"
        )
    host,bundle,wrong=map(lambda x:Path(x).resolve(),sys.argv[1:4])
    for p in (host,bundle,wrong): assert p.is_file(),p
    public_tests(host,bundle,wrong)
    if len(sys.argv)==5:
        python_parity(host,bundle,Path(sys.argv[4]).resolve())
    return 0


if __name__=="__main__":
    raise SystemExit(main())
