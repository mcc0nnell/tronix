# TRONIX bounded verification batches (experimental)

This optional, **client-side** addition is stacked on PR #20, which is stacked
on PR #19. It preserves the original MessagePack v1 scheduler ABI, the
standalone typed verifier's MessagePack v2 wire protocol, Celix bundles,
operator grants, and existing manual Atlas Watcher approval boundaries.
It is **not** a parallel native execution worker pool.

## Purpose

Pass a finite set of candidate answers from a corpus/recipe generator through
the existing pinned typed-verification service without granting authority to
candidate programs. The batch helper is in src/verify_batch.mjs.

- Admits at most 128 candidate records and 1 MiB total answer bytes per call.
- Each candidate carries an explicit, unique candidateId, a valid native
  taskId, and binary answer of 0..65,536 bytes; all records are preflighted
  before any IPC request is made.
- Copies every answer before dispatch; caller buffer mutations cannot alter
  the bound local SHA-256.
- Bounded fan-out: concurrency 1..8, default 4. Preserves input order in
  results regardless of arrival order. Does not retry or deduplicate.
- Rejects the entire batch on transport failure, malformed/unbound receipts,
  contradictory route/status, unexpected authority, or wrong answer hash.
  Already dispatched requests may finish before rejection returns.
- Preserves VERIFIED, REJECTED and UNVERIFIABLE as distinct outcomes.
  UNVERIFIABLE carries a review-required marker, **not** approval or a
  Watcher invocation. No model, command execution, publication, or merge.
- A native verifier receipt is operator-local and unsigned. Binding receipt
  fields to local bytes does not authenticate the process or prove AI truth.

## Call from a trusted local operator

The caller explicitly injects the existing verifyRequest client; the batch
helper does not open sockets itself or accept model-supplied transport routes.

```js
import {verifyRequest} from './src/verify_client.mjs';
import {verifyBatch} from './src/verify_batch.mjs';

const socket='/absolute/private/typed.sock'; // operator-owned 0600 socket
const items=[
  {candidateId:'candidate_001',taskId:'golden',answer:Buffer.from('42\n')},
  {candidateId:'candidate_002',taskId:'golden',answer:Buffer.from('41\n')}
];
const outcomes=await verifyBatch(
  items,
  (taskId,answer)=>verifyRequest(socket,taskId,answer),
  {concurrency:4}
);
// Inspect statuses and receipts; do not translate results into execution grants.
```

## Other repositories reviewed

| Source | Relevant concept | TRONIX action |
| --- | --- | --- |
| [AFLplusplus/LibAFL](https://github.com/AFLplusplus/LibAFL) | Replaceable feedback, corpus scheduler, low-level message passing | Candidate selection belongs upstream in Rizzer/NeonOxide; consider feedback-guided scheduling after receipts are authenticated |
| [apache/celix](https://github.com/apache/celix) | Dynamic service lifecycle, events and dependency registration | Keep Celix's native services; do not run slow jobs on framework event callbacks |
| [bytecodealliance/wasmtime](https://github.com/bytecodealliance/wasmtime) | Reuse of instantiated resources, fuel and epoch interrupts | Investigate as a separate WASI worker gate; do not mistake timeouts for host-call isolation |
| [libuv/libuv](https://github.com/libuv/libuv) | Efficient event-driven IO | Benchmark before adding an alternative event loop to TRONIX |
| [mcc0nnell/SignPipe](https://github.com/mcc0nnell/SignPipe) | Typed streaming and backpressure (project-specific) | Reuse bounded stream concepts only after a distinct ABI is specified |
| [mcc0nnell/fineract-celix](https://github.com/mcc0nnell/fineract-celix) | Pinned capabilities and independently retained evidence (project-specific) | Maintain explicit operator identity; authenticate batches before production promotion |

This is an architectural comparison, not a dependency import. No third-party
code has been copied and no new dependency has been introduced.

## Testing and actual performance boundary

```sh
npm run test:verification:batch  # dependency-free Node unit tests
npm run test:verification        # existing native Celix integration suite
```

The batch tests cover preflight/admission, concurrent bounds, ordering,
snapshotting, invalid receipts, negative decisions and fail-closed behavior.
The existing native daemon **serially accepts and handles each connection**;
allowing multiple client requests in flight cannot establish improved native
throughput. Validate real hardware, end-to-end p50/p95/p99, CPU and queue
utilization at concurrency 1, 2, 4 and 8 before claiming a speedup.

The later major upgrade is a measured, bounded **native supervisor worker
pool** with separate identity pinning, fairness, cancellation, backpressure,
isolation and replay semantics. It must be independently reviewed and tested;
do not silently turn this client helper into execution authority.
