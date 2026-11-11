# cpp-bounded-work-queue

Portfolio systems demo (**Habib Noor**) — a **C++20** bounded **MPMC** queue and **worker pool** built with **mutex + condition variables** (not lock-free). Local CLI only: no network services, cloud deploy, or paid infrastructure.

## Quickstart

**Pinned CI toolchain:** Ubuntu 22.04, **GCC 12** (`g++-12`), CMake 3.20+.

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-12
cmake --build build -j
cd build && ctest --output-on-failure
./demo 2 8 32 block
```

Optional sanitizers (CMake options):

```bash
cmake -S . -B build-asan -DCMAKE_CXX_COMPILER=g++-12 -DBOUNDED_QUEUE_ENABLE_SANITIZERS=ON
cmake -S . -B build-tsan -DCMAKE_CXX_COMPILER=g++-12 -DBOUNDED_QUEUE_ENABLE_TSAN=ON
```

## Architecture

| Component | Role |
|-----------|------|
| `habib::BoundedQueue<T>` | Bounded buffer; `try_push`/`push`, `try_pop`/`pop`, `close`, `close_discard` |
| `habib::WorkerPool` | Fixed worker threads, bounded task queue, `submit` → `std::future`, cooperative `std::stop_token` |

Synchronization is **one mutex per queue** plus **two condition variables** (`not_empty`, `not_full`). The pool adds **accounting mutexes**, `std::once_flag` for shutdown, and `in_flight` tracking.

### Lifecycle / shutdown state

```mermaid
stateDiagram-v2
    [*] --> Running: construct workers
    Running --> Running: submit / pop / push
    Running --> ShuttingDown: shutdown() first call
    note right of ShuttingDown: std::once_flag — concurrent shutdown calls are no-ops
  ShuttingDown --> DrainWait: mode=Drain
    DrainWait --> Closed: queue empty AND in_flight==0
    Closed --> Joined: queue.close(), stop requested, join workers
    ShuttingDown --> Cancelled: mode=Cancel
    Cancelled --> Joined: discard queued tasks, close_discard, stop requested, join workers
    Joined --> [*]: destructor safe (shutdown idempotent)
```

### Semantics (unambiguous)

| Topic | Behavior |
|-------|----------|
| **Backpressure** | `SubmitPolicy::Block` blocks on full queue; `Reject` fails fast when full |
| **Capacity 0** | All pushes rejected; `pop` on empty closed queue returns `nullopt` |
| **Close** | Unblocks all waiters; further pushes fail; `pop` drains then returns `nullopt` |
| **Drain shutdown** | Reject new work; wait until **queue empty and in_flight==0**; then close and join |
| **Cancel shutdown** | Discard **queued** work (futures get `task discarded…`); **in-flight** tasks still run to completion (no force-kill) |
| **Cancellation** | `std::stop_token` is cooperative; long-running tasks must poll — **arbitrary code cannot be preempted** |
| **Exceptions** | Propagate via `std::future::get`; `stats.failed` incremented |
| **Destructor** | Calls `shutdown(Drain)`; repeated shutdown is safe (`std::once_flag`) |

## Design trade-offs

**Chosen:** simplicity, clear blocking semantics, standard-library-only concurrency primitives.

**Not chosen:** lock-free rings, work stealing, OS thread pools, fairness/latency tuning — no claims of wait-freedom, fairness, or production-grade throughput (no benchmarks included).

**Limits:** single-queue mutex scalability; shutdown `wait_idle` has a **30s** bounded wait (throws on timeout to avoid hung tests); TSan on Linux uses **GCC libtsan** (`g++-12`); no strong guarantee on task ordering under contention beyond queue FIFO.

## Tests

**CTest:** 2 tests (`bounded_queue`, `worker_pool`), **60s** timeout each.

Coverage (deterministic sync: `latch` / `barrier` / polling with `expect_within`, no flaky sleeps):

- capacity / zero-capacity reject  
- backpressure block vs reject  
- close wakeup for blocked producers and consumers  
- MPMC exactly-once delivery (1000 integers)  
- pool reject when queue full, exception propagation, drain vs cancel  
- concurrent `shutdown`, post-shutdown submit rejection, `stop_token` visibility  

## CI

GitHub Actions (`ubuntu-22.04`): **build+test**, **ASan+UBSan**, **TSan** (separate job, GCC), **Docker build**. Warnings are strict (`-Werror` by default). Jobs fail on sanitizer findings — nothing is skipped silently.

## AI assistance disclosure

This repository was **built and tested primarily with AI assistance** (Cursor Cloud Agent). It is a **portfolio artifact** for Habib Noor — not a claim that every line was personally reviewed in production, and **no employer affiliation or hiring representation**.

## Interview prep

See [`artifacts/interview-notes.md`](artifacts/interview-notes.md) for a walkthrough, tradeoffs, break experiments, ownership ideas, and **30 Q&As**.

## License

MIT — see [LICENSE](LICENSE).
