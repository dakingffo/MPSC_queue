# Elastic Reclaim Design Notes

This document records the design direction for an online elastic reclamation mode.
The existing queue is optimized for stable, low-jitter throughput and should remain
the default behavior until the elastic design is proven by benchmarks and stress
tests.

## Current Memory Model

The current implementation uses:

- Per-thread local node caches.
- A global lock-free chunk stack.
- Page allocation owned by a type-level global manager.
- Freely mixed chunks: nodes from different pages can be combined into the same
  logical chunk.

This is fast because a producer or consumer only deals with a local node list in
the hot path, and the global stack only moves chunk heads with a tagged pointer.

## Why Online Page Reclaim Is Not Safe Today

The current page list knows which contiguous allocation owns nodes, but the global
chunk stack does not preserve page ownership. After enough push/pop cycles, a chunk
can contain nodes from many pages. Because of that, the manager cannot prove that
all nodes from a page are back in the global pool while producers and the consumer
are still running.

There is also a producer publication window:

1. Producer allocates a node.
2. Producer exchanges `head_`.
3. Producer publishes `old_head->next_`.

During that window, `empty()` can be insufficient as a global quiescence test.
This is why the current `shrink_to_fit()` is intentionally limited to idle-time
usage after all producers have stopped.

## Target Modes

### stable

The stable mode keeps the current memory model:

- Freely mixed chunks.
- Minimal hot-path metadata.
- No online page reclamation.
- Lowest jitter and strongest throughput target.

### idle_reclaim

The idle reclaim mode is the current conservative `shrink_to_fit()` path:

- Requires a drained queue.
- Requires no active producers.
- Requires a single same-template queue instance.
- Rebuilds the minimum global pool after releasing all pages.

This mode is already benchmarked by `mpsc_bench_memory_cycle`.

### elastic

The future elastic mode must preserve page ownership well enough to reclaim pages
online. The important design point is that this cannot be a small modification of
the current global chunk stack. It needs a different ownership layout.

The likely direction:

- Allocate fixed-size slabs/pages.
- Keep chunks page-local or attach page ownership metadata to chunk groups.
- Track per-page free node counts.
- Reclaim only pages with all nodes returned and no active references.
- Avoid adding atomics to the single-element enqueue fast path unless the benchmark
  proves the trade-off is acceptable.

## Proposed Architecture

Use a policy split rather than a runtime branch:

```cpp
namespace daking::memory_policy {
    struct stable;
    struct idle_reclaim;
    struct elastic;
}
```

The queue can then grow toward:

```cpp
template <
    typename Ty,
    std::size_t ThreadLocalCapacity = 256,
    std::size_t Align = 64,
    typename Alloc = std::allocator<Ty>,
    typename MemoryPolicy = memory_policy::idle_reclaim
>
class MPSC_queue;
```

The default should keep today's behavior. Experimental policies should be opt-in.

Current branch status:

- `memory_policy::stable` exists and disables `shrink_to_fit()`.
- `memory_policy::idle_reclaim` is the default and keeps today's behavior.
- `memory_policy::elastic` exists only as an experimental placeholder. It does
  not implement online reclaim yet.

## Benchmark Gates

An elastic implementation should not be considered successful unless it passes:

- Existing functional tests.
- Linearizability benchmarks before and after reclaim.
- `mpsc_bench_memory_cycle`.
- A long-running grow/drain/reclaim/regrow soak.
- A throughput comparison against stable mode.
- Tail latency checks for enqueue and dequeue.

Acceptance target:

- No correctness regression.
- Online reclaim must reduce retained global nodes under bursty workloads.
- Stable mode throughput must not regress.
- Elastic mode overhead must be explicit and benchmarked, not assumed.

## Implementation Plan

1. Introduce policy type names without changing hot-path behavior. Done.
2. Add benchmarks that can compare policy types side by side.
3. Prototype page-local chunks behind an opt-in `elastic` policy.
4. Add reclaim accounting and tests.
5. Run soak and latency benchmarks before considering a PR upstream.
