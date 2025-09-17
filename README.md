# OpenAutoCore Transport

A small transport layer using Cap'n Proto to frame messages and a shared-memory duplex transport for fast interprocess communication.

This package contains:
- `wire.capnp` — schema defining `MsgType` and `Envelope`
- `open_auto_transport` — C++ library implementing the transport
- `transport_demo` — demo CLI to send/receive messages between two processes

## Bazel targets

In this directory:
- `//:wire_capnp_gen` — genrule that runs `capnp` to generate `wire.capnp.h` and `wire.capnp.c++`
- `//:wire_capnp` — cc_library exposing the generated sources
- `//:open_auto_transport` — cc_library for the transport itself
- `//:transport_demo` — demo binary

External deps expected by the BUILD file:
- `@capnp-cpp` (Cap'n Proto and KJ)
- `@boost.asio` (standalone Asio)
- `@capnproto_shm_transport` (shared-memory duplex transport)

## Build

From this folder:

```bash
bazel build //:open_auto_transport
bazel build //:transport_demo
```

From the repo root (equivalent):

```bash
bazel build //src/autoapp/Transport:open_auto_transport
bazel build //src/autoapp/Transport:transport_demo
```

## Run the demo

Use two terminals. One side acts as A (creator), the other as B (opener).

Terminal 1 (Side A):

```bash
bazel run //:transport_demo -- --a --clean --interval 200 --count 0
```

Terminal 2 (Side B):

```bash
bazel run //:transport_demo -- --b --wait 5000 --interval 200 --count 0
```

Flags:
- `--a` or `--b` choose the side.
- `--clean` removes any existing shared memory before starting (A only).
- `--poll <usec>` polling interval for RX loop (default 1000us).
- `--wait <ms>` how long B waits for A to appear (default 5000ms).
- `--interval <ms>` send interval (default 200ms).
- `--count <n>` number of messages to send; `0` means infinite.