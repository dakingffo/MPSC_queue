#include "MPSC_queue.hpp" 
#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <atomic>
#include <string>

constexpr size_t TOTAL_OPS = 100000000;
//using TestQueue = moodycamel::ConcurrentQueue<int>;
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
        if (consumer.joinable()) {
            consumer.join();
        }
        state.PauseTiming();
        for (auto& p : producers) {
            p.join();
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
BM_MPSC_Throughput/1/min_warmup_time:2.000/real_time   789803100 ns        0.000 ns            1 items_per_second=126.614M/s P=1, C=1
BM_MPSC_Throughput/2/min_warmup_time:2.000/real_time  2059432600 ns        0.000 ns            1 items_per_second=48.5571M/s P=2, C=1
BM_MPSC_Throughput/4/min_warmup_time:2.000/real_time  1662831100 ns        0.000 ns            1 items_per_second=60.1384M/s P=4, C=1
BM_MPSC_Throughput/8/min_warmup_time:2.000/real_time  2407692500 ns        0.000 ns            1 items_per_second=41.5335M/s P=8, C=1
BM_MPSC_Throughput/16/min_warmup_time:2.000/real_time 2558583600 ns        0.000 ns            1 items_per_second=39.0841M/s P=16, C=1
*/