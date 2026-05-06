#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <string>

#include "daking/MPSC_queue.hpp"

namespace {

using ElasticQueue = daking::MPSC_queue<int, 256, 64, std::allocator<int>, daking::memory_policy::elastic>;

static void BM_MPSC_ReclaimFreePages(benchmark::State& state) {
    const std::size_t reserve_chunks = static_cast<std::size_t>(state.range(0));

    for (auto _ : state) {
        state.PauseTiming();
        ElasticQueue q;
        ElasticQueue::reserve_global_chunk(reserve_chunks);
        const auto nodes_before = ElasticQueue::global_node_size_apprx();
        state.ResumeTiming();

        const auto reclaim_start = std::chrono::steady_clock::now();
        const auto reclaimed_nodes = ElasticQueue::reclaim_free_pages();
        const auto reclaim_end = std::chrono::steady_clock::now();
        const auto nodes_after = ElasticQueue::global_node_size_apprx();

        state.counters["global_nodes_before"] = static_cast<double>(nodes_before);
        state.counters["global_nodes_after"] = static_cast<double>(nodes_after);
        state.counters["reclaimed_nodes"] = static_cast<double>(reclaimed_nodes);
        state.counters["reclaim_us"] = std::chrono::duration<double, std::micro>(reclaim_end - reclaim_start).count();
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
    state.SetLabel("elastic, reclaim-only, chunks=" + std::to_string(static_cast<std::size_t>(state.range(0))));
}

} // namespace

BENCHMARK(BM_MPSC_ReclaimFreePages)
    ->Arg(1)
    ->Arg(4)
    ->Arg(16)
    ->Arg(64)
    ->Arg(256)
    ->UseRealTime()
    ->MinWarmUpTime(1.0);

BENCHMARK_MAIN();
