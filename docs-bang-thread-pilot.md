# 6lowBang OpenThread POSIX evidence — source ownership migrated

The earlier Tronix two-node OpenThread CLI experiment remains documented as historical validation. Its signed binary codec, POSIX UDP backend, and simulated-Thread test harness now belong to **mcc0nnell/6lowstick**, PR #32, at signed commit `800ec1c84cf045362cfe528c2c79008021e2c898`.

The maintained 6s source paths are `native/bang/src/lowbang_wire.c`, `native/bang/src/lowbang_udp.c`, `native/bang/examples/posix_codec.c`, `experiments/bang/posix-lab.mjs`, and `docs/6lowbang-transport.md`.

Tronix maintains only Celix lifecycle, typed stream subscriptions, fanout, and a narrow adapter that links to the reviewed, pinned 6s `lowbang_udp` library. The Tronix-specific duplicate wire codec and CLI harness have been deleted.

The older `evidence/bang-thread-20261023.json` remains a historical observation of simulated OpenThread UDP, not proof of physical RF or a native C OpenThread backend. The 6s-owned harness has also been independently exercised on Butcher.
