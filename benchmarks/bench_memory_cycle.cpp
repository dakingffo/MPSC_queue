#include <benchmark/benchmark.h>

#include <chrono>
#include <atomic>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "daking/MPSC_queue.hpp"

namespace {

using StableQueue = daking::MPSC_queue<int, 256, 64, std::allocator<int>, daking::memory_policy::stable>;
using IdleReclaimQueue = daking::MPSC_queue<int>;
using ElasticQueue = daking::MPSC_queue<int, 256, 64, std::allocator<int>, daking::memory_policy::elastic>;

constexpr std::size_t kItemsPerProducer = 200000;

enum class ReclaimMode {
    none,
    shrink_to_fit,
    free_pages
};

template <typename Queue>
void producer_thread(Queue* q, std::size_t items_to_push, std::atomic_bool* start) {
    while (!start->load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    for (std::size_t i = 0; i < items_to_push; ++i) {
        q->enqueue(1);
    }
}

template <typename Queue>
void consumer_thread(Queue* q, std::size_t total_items_to_pop, std::atomic_bool* start) {
    while (!start->load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::size_t popped_count = 0;
    int val = 0;
    while (popped_count < total_items_to_pop) {
        if (q->try_dequeue(val)) {
            ++popped_count;
        }
        else {
            std::this_thread::yield();
        }
    }
}

template <typename Queue>
void run_burst(Queue& q, int producers, std::size_t items_per_producer) {
    const std::size_t total_items = items_per_producer * static_cast<std::size_t>(producers);
    std::atomic_bool start{false};

    std::thread consumer(consumer_thread<Queue>, &q, total_items, &start);

    std::vector<std::thread> producer_threads;
    producer_threads.reserve(static_cast<std::size_t>(producers));
    for (int i = 0; i < producers; ++i) {
        producer_threads.emplace_back(producer_thread<Queue>, &q, items_per_producer, &start);
    }

    start.store(true, std::memory_order_release);
    for (auto& producer : producer_threads) {
        producer.join();
    }
    consumer.join();
}

template <typename Queue, ReclaimMode Mode>
static void BM_MPSC_MemoryCycle(benchmark::State& state) {
    const int producers = static_cast<int>(state.range(0));
    const std::size_t items_per_producer = kItemsPerProducer;
    const std::size_t total_items_per_burst = items_per_producer * static_cast<std::size_t>(producers);

    for (auto _ : state) {
        Queue q;
        Queue::reserve_global_chunk(64);

        run_burst(q, producers, items_per_producer);
        if (!q.empty()) {
            state.SkipWithError("Queue should be empty after first burst");
            break;
        }

        const auto nodes_before = Queue::global_node_size_apprx();
        double reclaim_us = 0.0;
        std::size_t reclaimed_nodes = 0;
        if constexpr (Mode == ReclaimMode::shrink_to_fit) {
            const auto reclaim_start = std::chrono::steady_clock::now();
            if (!q.shrink_to_fit()) {
                state.SkipWithError("shrink_to_fit failed unexpectedly");
                break;
            }
            const auto reclaim_end = std::chrono::steady_clock::now();
            reclaim_us = std::chrono::duration<double, std::micro>(reclaim_end - reclaim_start).count();
            reclaimed_nodes = nodes_before - Queue::global_node_size_apprx();
        }
        else if constexpr (Mode == ReclaimMode::free_pages) {
            const auto reclaim_start = std::chrono::steady_clock::now();
            reclaimed_nodes = Queue::reclaim_free_pages();
            const auto reclaim_end = std::chrono::steady_clock::now();
            reclaim_us = std::chrono::duration<double, std::micro>(reclaim_end - reclaim_start).count();
            reclaimed_nodes = nodes_before - Queue::global_node_size_apprx();
        }
        const auto nodes_after = Queue::global_node_size_apprx();

        run_burst(q, producers, items_per_producer);
        if (!q.empty()) {
            state.SkipWithError("Queue should be empty after second burst");
            break;
        }

        state.counters["global_nodes_before"] = static_cast<double>(nodes_before);
        state.counters["global_nodes_after"] = static_cast<double>(nodes_after);
        if constexpr (Mode != ReclaimMode::none) {
            state.counters["reclaim_us"] = reclaim_us;
            state.counters["reclaimed_nodes"] = static_cast<double>(reclaimed_nodes);
        }
    }

    state.SetItemsProcessed(static_cast<int64_t>(total_items_per_burst * 2 * state.iterations()));
    const char* policy = "unknown";
    const char* mode = "none";
    if constexpr (std::is_same_v<Queue, StableQueue>) {
        policy = "stable";
    }
    else if constexpr (std::is_same_v<Queue, IdleReclaimQueue>) {
        policy = "idle_reclaim";
    }
    else if constexpr (std::is_same_v<Queue, ElasticQueue>) {
        policy = "elastic";
    }

    if constexpr (Mode == ReclaimMode::shrink_to_fit) {
        mode = "shrink_to_fit";
    }
    else if constexpr (Mode == ReclaimMode::free_pages) {
        mode = "reclaim_free_pages";
    }

    state.SetLabel(std::string(policy) + ", " + mode + ", P=" + std::to_string(producers));
}

} // namespace

BENCHMARK_TEMPLATE(BM_MPSC_MemoryCycle, StableQueue, ReclaimMode::none)
    ->Arg(1)->Arg(4)->Arg(16)->UseRealTime()->MinWarmUpTime(1.0);
BENCHMARK_TEMPLATE(BM_MPSC_MemoryCycle, IdleReclaimQueue, ReclaimMode::shrink_to_fit)
    ->Arg(1)->Arg(4)->Arg(16)->UseRealTime()->MinWarmUpTime(1.0);
BENCHMARK_TEMPLATE(BM_MPSC_MemoryCycle, ElasticQueue, ReclaimMode::none)
    ->Arg(1)->Arg(4)->Arg(16)->UseRealTime()->MinWarmUpTime(1.0);
BENCHMARK_TEMPLATE(BM_MPSC_MemoryCycle, ElasticQueue, ReclaimMode::free_pages)
    ->Arg(1)->Arg(4)->Arg(16)->UseRealTime()->MinWarmUpTime(1.0);

BENCHMARK_MAIN();
