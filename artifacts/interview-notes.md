# Interview notes — bounded MPMC queue & worker pool

**Author context:** Habib Noor portfolio demo. **AI-built** with automated tests and CI; use this doc to explain design choices in interviews.

## 60-second walkthrough

Two headers implement the system: a **bounded `std::deque` queue** guarded by a mutex and two condition variables, and a **worker pool** that enqueues type-erased tasks (`std::function`) with **promises/futures** for results. Backpressure is explicit: blocking or rejecting when the bounded buffer is full. Shutdown is **idempotent** (`std::once_flag`): **Drain** waits for queued and in-flight work; **Cancel** throws away queued tasks (futures excepted) but still **joins workers** so running tasks finish cooperatively. No lock-free atomics, no forced task termination.

## Ownership proposals (if this were production)

| Area | Today | Production follow-up |
|------|-------|----------------------|
| Queue | Header-only template | Extract `.cpp` instantiations or type-erased job queue |
| Shutdown | `wait_idle` + join | Metrics hook, deadline propagation, phased drain API |
| Testing | Assert-based binaries | Optionally gtest + fuzz of interleavings |
| Observability | `WorkerPoolStats` | Tracing spans, queue depth gauges |

## Break experiments (safe learning exercises)

1. Remove `notify_all` on `close()` → hangs in close-wakeup tests.  
2. Call `join()` twice without `once_flag` → UB; tests rely on once_flag guard.  
3. `Cancel` without `on_discard` → `std::future_error: Broken promise`.  
4. Set capacity 0 and use blocking `push` → immediate reject (no deadlock).  
5. Drain shutdown without `in_flight` → returns before running tasks finish.

## Trade-offs (talking points)

- **Mutex vs lock-free:** Chose clarity and Condition-variable semantics; accept mutex contention under extreme MPMC load.  
- **FIFO deque:** Simple; not cache-optimized; no steal between workers.  
- **Reject vs block:** Push policy applies to **queue capacity**, not “all workers busy” — document clearly to avoid user surprise.  
- **Cooperative cancel:** Matches `std::jthread`/stop_token model; cannot stop a spin loop without user checks.

## 30 Q&As

1. **Why not lock-free?** Scope control, demonstrable mutex/CV correctness, sanitizer-friendly; no fairness/perf claims.  
2. **MPMC safety?** All queue mutations under one mutex; tests assert 1000 unique values consumed once.  
3. **Spurious wakeups?** Loops on predicates in `wait`/`push`/`pop`.  
4. **What happens at capacity 0?** `push`/`try_push` fail immediately; no storage.  
5. **Difference between `close` and `close_discard`?** Former drains; latter clears pending items.  
6. **Blocked producer on `close`?** `push` returns false; thread wakes via `cv_not_full`.  
7. **Blocked consumer on `close`?** `pop` returns `nullopt` after drain.  
8. **Is `try_pop` on closed empty queue blocking?** No — immediate `nullopt`.  
9. **Reject policy when worker busy but queue has space?** Submit succeeds — backpressure is queue-sized, not worker-limited.  
10. **How are results returned?** `std::promise`/`std::future` per submit.  
11. **Exception path?** Caught in task wrapper; `set_exception` on promise; `stats.failed++`.  
12. **Drain shutdown ordering?** Flag shutdown → wait idle → request stop → close → join.  
13. **Cancel shutdown ordering?** Flag → pop/discard queued → `close_discard` → stop → join (in-flight continues).  
14. **Why join on Cancel without waiting for queue?** In-flight tracked separately; discard handles queued promises.  
15. **Concurrent `shutdown`?** `std::call_once`; first mode wins; others no-op safely.  
16. **Destructor behavior?** `shutdown(Drain)`; safe if user already shut down.  
17. **Can shutdown force-kill a task?** No — cooperative model only.  
18. **What is `stop_token` for?** Tasks may poll `pool.stop_token()`; stop requested after shutdown begins.  
19. **Shutdown timeout?** 30s `wait_for` on Drain idle wait throws `runtime_error` (tests bound hangs).  
20. **Why `in_flight` atomic?** Idle detection without holding queue mutex.  
21. **Double notify on discard?** `close_discard` notifies both CVs like `close`.  
22. **Thread count vs queue capacity?** Independent — classic thread-pool tuning knobs.  
23. **Memory ordering on `shutdown_initiated`?** acquire/release for submit fast path.  
24. **TSan strategy?** Separate CI job, `g++-12 -fsanitize=thread` on Linux x86_64.  
25. **ASan findings?** Treated as build failures; no `continue-on-error`.  
26. **Why latches in tests?** Deterministic readiness without `sleep`.  
27. **MPMC test avoids deadlock?** Producers `join` before `close`; consumers exit on closed empty `pop`.  
28. **Docker role?** Reproducible build+test image; not a runtime service.  
29. **Known scalability limit?** Single mutex queue; fine for demo, not for millions QPS.  
30. **What would you add next?** Metrics, timed shutdown, optional `stop_token` injected into task arity via detection — still out of scope here.

## Demo commands

```bash
./build/demo 4 4 20 reject   # expect some rejected if flooded
./build/demo 2 8 32 block
```

## Limits (repeat in interviews)

No lock-free/wait-free/fairness/performance guarantees; no measured benchmarks; AI-assisted implementation; not reviewed as production infrastructure.
