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

void sequenced_producer_thread(TestQueue* q, size_t items_to_push, std::vector<std::atomic_bool>* start, int pos) {
    auto& st = *start;
    while (!st[pos].load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    for (size_t i = 0; i < items_to_push; ++i) {
        q->enqueue(1);

        if (i == items_to_push - items_to_push / 20 && st[(pos + 1) % st.size()].load(std::memory_order_acquire) == false) {
            st[(pos + 1) % st.size()].store(true, std::memory_order_release);
        }
    }
}

static void BM_4x_UnevenWave_SPSClike_Aggregation(benchmark::State& state) {
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
            producers.emplace_back(sequenced_producer_thread, &q, items_per_producer, &start, i);
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
    state.SetLabel("P=4, C=1 (4x Uneven producer peak)");
}

// 附加新的基准测试调用
BENCHMARK(BM_4x_UnevenWave_SPSClike_Aggregation)
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
--------------------------------------------------------------------------------------------------------------------------------
Benchmark                                                                      Time             CPU   Iterations UserCounters...
--------------------------------------------------------------------------------------------------------------------------------
BM_MPSC_Throughput / 1 / min_warmup_time:2.000 / real_time                   707132400 ns        0.000 ns            1 items_per_second = 141.416M / s P = 1, C = 1
BM_MPSC_Throughput / 2 / min_warmup_time : 2.000 / real_time                  2236937200 ns        0.000 ns            1 items_per_second = 44.704M / s P = 2, C = 1
BM_MPSC_Throughput / 4 / min_warmup_time : 2.000 / real_time                  1741995700 ns        0.000 ns            1 items_per_second = 57.4054M / s P = 4, C = 1
BM_MPSC_Throughput / 8 / min_warmup_time : 2.000 / real_time                  1571191000 ns        0.000 ns            1 items_per_second = 63.646M / s P = 8, C = 1
BM_MPSC_Throughput / 16 / min_warmup_time : 2.000 / real_time                 2023721600 ns        0.000 ns            1 items_per_second = 49.4139M / s P = 16, C = 1
BM_4x_UnevenWave_SPSClike_Aggregation / min_warmup_time : 2.000 / real_time  788367500 ns        0.000 ns            1 items_per_second = 126.844M / s P = 4, C = 1 (4x Uneven producer peak)


moodycamel: 
moodycamel ConcurrentQueue is a MPMC queue, so this comparion is unfair.
Run on (16 X 3992 MHz CPU s)
CPU Caches:
  L1 Data 32 KiB (x8)
  L1 Instruction 32 KiB (x8)
  L2 Unified 1024 KiB (x8)
  L3 Unified 16384 KiB (x1)
--------------------------------------------------------------------------------------------------------------------------------
Benchmark                                                                      Time             CPU   Iterations UserCounters...
--------------------------------------------------------------------------------------------------------------------------------
BM_MPSC_Throughput/1/min_warmup_time:2.000/real_time                  2878925400 ns        0.000 ns            1 items_per_second=34.7352M/s P=1, C=1
BM_MPSC_Throughput/2/min_warmup_time:2.000/real_time                  1886539900 ns        0.000 ns            1 items_per_second=53.0071M/s P=2, C=1
BM_MPSC_Throughput/4/min_warmup_time:2.000/real_time                  1571032300 ns        0.000 ns            1 items_per_second=63.6524M/s P=4, C=1
BM_MPSC_Throughput/8/min_warmup_time:2.000/real_time                  1686614400 ns        0.000 ns            1 items_per_second=59.2904M/s P=8, C=1
BM_MPSC_Throughput/16/min_warmup_time:2.000/real_time                 2598044900 ns        0.000 ns            1 items_per_second=38.4905M/s P=16, C=1
BM_4x_UnevenWave_SPSClike_Aggregation/min_warmup_time:2.000/real_time 2879179000 ns        0.000 ns            1 items_per_second=34.7321M/s P=4, C=1 (4x Uneven producer peak)
*/