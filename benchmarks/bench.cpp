#include "MPSC_queue.hpp" 
// #include <moodycamel/concurrentqueue.h>
#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <atomic>
#include <string>

constexpr size_t TOTAL_OPS = 100000000;
// using TestQueue = moodycamel::ConcurrentQueue<int>;
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

void sequenced_producer_thread(TestQueue* q, size_t items_to_push, std::vector<std::atomic_bool>* start, int pos, int den) {
    auto& st = *start;
    while (!st[pos].load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    for (size_t i = 0; i < items_to_push; ++i) {
        q->enqueue(1);

        if (i == items_to_push - items_to_push / den && st[(pos + 1) % st.size()].load(std::memory_order_acquire) == false) {
            st[(pos + 1) % st.size()].store(true, std::memory_order_release);
        }
    }
}

static void BM_4x_UnevenWave_SPSClike_Aggregation(benchmark::State& state) {
    const int den = (int)state.range(0);
    const int num_producers = 4;
    const size_t items_per_producer = TOTAL_OPS / num_producers;

    TestQueue q;

    for (auto _ : state) {
        state.PauseTiming();
        std::vector<std::thread> producers;
        producers.reserve(num_producers);
        std::vector<std::atomic_bool> start(num_producers + 1);
        std::thread consumer(consumer_thread, &q, TOTAL_OPS, &start[num_producers]);
        for (int i = 0; i < num_producers; ++i) {
            producers.emplace_back(sequenced_producer_thread, &q, items_per_producer, &start, i, den);
        }
        state.ResumeTiming();
        start[0].store(true, std::memory_order_release);
        start[num_producers].store(std::memory_order_release);
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
    state.SetLabel("P=4, C=1 (4x Uneven producer peak relay at " + std::to_string(1 - 1.0 / den) + ")");
}

BENCHMARK(BM_4x_UnevenWave_SPSClike_Aggregation)
    ->Args({ 2 })
    ->Args({ 5 })
    ->Args({ 10 })
    ->Args({ 20 })
    ->Args({ 50 })
    ->UseRealTime()
    ->MinWarmUpTime(2.0);

BENCHMARK_MAIN();

/*
daking:
Run on(16 X 3992 MHz CPU s)
CPU Caches :
L1 Data 32 KiB(x8)
L1 Instruction 32 KiB(x8)
L2 Unified 1024 KiB(x8)
L3 Unified 16384 KiB(x1)
------------------------------------------------------------------------------------------------------------------------------------------------
Benchmark                                                                                      Time             CPU   Iterations UserCounters...
------------------------------------------------------------------------------------------------------------------------------------------------
BM_MPSC_Throughput/1/min_warmup_time:2.000/iterations:3/real_time                      666017800 ns        0.000 ns            3 items_per_second=150.146M/s P=1, C=1
BM_MPSC_Throughput/2/min_warmup_time:2.000/iterations:3/real_time                     2129343400 ns        0.000 ns            3 items_per_second=46.9628M/s P=2, C=1
BM_MPSC_Throughput/4/min_warmup_time:2.000/iterations:3/real_time                     2116579700 ns        0.000 ns            3 items_per_second=47.246M/s P=4, C=1
BM_MPSC_Throughput/8/min_warmup_time:2.000/iterations:3/real_time                     2237837467 ns        0.000 ns            3 items_per_second=44.686M/s P=8, C=1
BM_MPSC_Throughput/16/min_warmup_time:2.000/iterations:3/real_time                    2314983567 ns        0.000 ns            3 items_per_second=43.1969M/s P=16, C=1
BM_4x_UnevenWave_SPSClike_Aggregation/2/min_warmup_time:2.000/iterations:3/real_time  1667029300 ns        0.000 ns            3 items_per_second=59.9869M/s P=4, C=1 (4x Uneven producer peak relay at 0.500000)
BM_4x_UnevenWave_SPSClike_Aggregation/5/min_warmup_time:2.000/iterations:3/real_time  1309859867 ns        0.000 ns            3 items_per_second=76.344M/s P=4, C=1 (4x Uneven producer peak relay at 0.800000)
BM_4x_UnevenWave_SPSClike_Aggregation/10/min_warmup_time:2.000/iterations:3/real_time 1162913500 ns        0.000 ns            3 items_per_second=85.9909M/s P=4, C=1 (4x Uneven producer peak relay at 0.900000)
BM_4x_UnevenWave_SPSClike_Aggregation/20/min_warmup_time:2.000/iterations:3/real_time  840474567 ns        0.000 ns            3 items_per_second=118.98M/s P=4, C=1 (4x Uneven producer peak relay at 0.950000)
BM_4x_UnevenWave_SPSClike_Aggregation/50/min_warmup_time:2.000/iterations:3/real_time  729438933 ns        0.000 ns            3 items_per_second=137.092M/s P=4, C=1 (4x Uneven producer peak relay at 0.980000)

moodycamel: 
moodycamel ConcurrentQueue is a MPMC queue, so this comparion is unfair.
Run on (16 X 3992 MHz CPU s)
CPU Caches:
  L1 Data 32 KiB (x8)
  L1 Instruction 32 KiB (x8)
  L2 Unified 1024 KiB (x8)
  L3 Unified 16384 KiB (x1)
------------------------------------------------------------------------------------------------------------------------------------------------
Benchmark                                                                                      Time             CPU   Iterations UserCounters...
------------------------------------------------------------------------------------------------------------------------------------------------
BM_MPSC_Throughput/1/min_warmup_time:2.000/iterations:3/real_time                     2329350600 ns        0.000 ns            3 items_per_second=42.9304M/s P=1, C=1
BM_MPSC_Throughput/2/min_warmup_time:2.000/iterations:3/real_time                     1875352533 ns        0.000 ns            3 items_per_second=53.3233M/s P=2, C=1
BM_MPSC_Throughput/4/min_warmup_time:2.000/iterations:3/real_time                     1703935767 ns        0.000 ns            3 items_per_second=58.6877M/s P=4, C=1
BM_MPSC_Throughput/8/min_warmup_time:2.000/iterations:3/real_time                     1743191867 ns        0.000 ns            3 items_per_second=57.366M/s P=8, C=1
BM_MPSC_Throughput/16/min_warmup_time:2.000/iterations:3/real_time                    2503733233 ns        0.000 ns            3 items_per_second=39.9404M/s P=16, C=1
BM_4x_UnevenWave_SPSClike_Aggregation/2/min_warmup_time:2.000/iterations:3/real_time  2090831100 ns        0.000 ns            3 items_per_second=47.8279M/s P=4, C=1 (4x Uneven producer peak relay at 0.500000)
BM_4x_UnevenWave_SPSClike_Aggregation/5/min_warmup_time:2.000/iterations:3/real_time  2223506400 ns        0.000 ns            3 items_per_second=44.974M/s P=4, C=1 (4x Uneven producer peak relay at 0.800000)
BM_4x_UnevenWave_SPSClike_Aggregation/10/min_warmup_time:2.000/iterations:3/real_time 2152426600 ns        0.000 ns            3 items_per_second=46.4592M/s P=4, C=1 (4x Uneven producer peak relay at 0.900000)
BM_4x_UnevenWave_SPSClike_Aggregation/20/min_warmup_time:2.000/iterations:3/real_time 2261306433 ns        0.000 ns            3 items_per_second=44.2222M/s P=4, C=1 (4x Uneven producer peak relay at 0.950000)
BM_4x_UnevenWave_SPSClike_Aggregation/50/min_warmup_time:2.000/iterations:3/real_time 2191420133 ns        0.000 ns            3 items_per_second=45.6325M/s P=4, C=1 (4x Uneven producer peak relay at 0.980000)
*/