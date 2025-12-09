#include "MPSC_queue.hpp" 
#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <atomic>
#include <string>

constexpr size_t TOTAL_OPS = 10000000;
using TestQueue = daking::MPSC_queue<int>;

void producer_thread(TestQueue* q, size_t items_to_push, std::atomic_bool* start) {
    while (!start->load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
    for (size_t i = 0; i < items_to_push; ++i) {
        q->enqueue(1);
    }
}

void consumer_thread(TestQueue* q, size_t total_items_to_pop, std::atomic_bool* start) {
    while (!start->load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    size_t popped_count = 0;
    int val = 0;
    while (popped_count < total_items_to_pop) {
        if (q->try_dequeue(val)) {
            popped_count++;
        }
    }
}

static void BM_MPSC_Throughput(benchmark::State& state) {
    const int num_producers = (int)state.range(0);
    const size_t items_per_producer = TOTAL_OPS / num_producers;

    TestQueue q;

    for (auto _ : state) {
        state.PauseTiming();
        std::vector<std::thread> producers;
        producers.reserve(num_producers);
		std::atomic_bool start{ false };
        std::thread consumer(consumer_thread, &q, TOTAL_OPS, &start);
        for (int i = 0; i < num_producers; ++i) {
            producers.emplace_back(producer_thread, &q, items_per_producer, &start);
        }
        state.ResumeTiming();
        start.store(true, std::memory_order_release);
        for (auto& p : producers) {
            p.join();
        }
        state.PauseTiming();
        if (consumer.joinable()) {
            consumer.join();
        }
        state.ResumeTiming();
    }

    state.SetItemsProcessed(TOTAL_OPS * state.iterations());
    state.SetLabel("P=" + std::to_string(num_producers) + ", C=1");
}

BENCHMARK(BM_MPSC_Throughput)
    ->Args({ 1 })
    ->Args({ 2 })
    ->Args({ 4 })
    ->Args({ 8 })
    ->Args({ 16 })
    ->UseRealTime()
	->MinWarmUpTime(2.0);
BENCHMARK_MAIN();

/*
----------------------------------------------------------------------------------------------------------------
Benchmark                                                      Time             CPU   Iterations UserCounters...
----------------------------------------------------------------------------------------------------------------
BM_MPSC_Throughput/1/min_warmup_time:1.000/real_time    85810287 ns        0.000 ns            8 items_per_second=116.536M/s P=1, C=1
BM_MPSC_Throughput/2/min_warmup_time:1.000/real_time   225980800 ns        0.000 ns            3 items_per_second=44.2515M/s P=2, C=1
BM_MPSC_Throughput/4/min_warmup_time:1.000/real_time   175344750 ns        0.000 ns            4 items_per_second=57.0305M/s P=4, C=1
BM_MPSC_Throughput/8/min_warmup_time:1.000/real_time   155284100 ns        0.000 ns            4 items_per_second=64.3981M/s P=8, C=1
BM_MPSC_Throughput/16/min_warmup_time:1.000/real_time  134480060 ns        0.000 ns            5 items_per_second=74.3605M/s P=16, C=1
*/