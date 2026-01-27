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
        start[num_producers].store(true, std::memory_order_release);
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

void producer_thread_enqueue_bulk(TestQueue* q, size_t items_to_push, std::atomic_bool* start) {
    while (!start->load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    int nums[32]{};
    for (size_t i = 0; i < items_to_push; i += 32) {
        q->enqueue_bulk(nums, std::min(items_to_push - i, (std::size_t)32));
    }
}

// consumer thread remains the same, because dequeue_bulk is just a while loop of try_dequeue

static void BM_MPSC_Throughput_Bulk(benchmark::State& state) {
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
            producers.emplace_back(producer_thread_enqueue_bulk, &q, items_per_producer, &start);
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

BENCHMARK(BM_MPSC_Throughput_Bulk)
    ->Args({ 1 })
    ->Args({ 2 })
    ->Args({ 4 })
    ->Args({ 8 })
    ->Args({ 16 })
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
-----------------------------------------------------------------------------------------------------------------------------------
Benchmark                                                                         Time             CPU   Iterations UserCounters...
-----------------------------------------------------------------------------------------------------------------------------------
BM_MPSC_Throughput/1/min_warmup_time:2.000/real_time                      548685900 ns        0.000 ns            1 items_per_second=182.254M/s P=1, C=1
BM_MPSC_Throughput/2/min_warmup_time:2.000/real_time                     2156405800 ns        0.000 ns            1 items_per_second=46.3735M/s P=2, C=1
BM_MPSC_Throughput/4/min_warmup_time:2.000/real_time                     1725362300 ns        0.000 ns            1 items_per_second=57.9588M/s P=4, C=1
BM_MPSC_Throughput/8/min_warmup_time:2.000/real_time                     1785052100 ns        0.000 ns            1 items_per_second=56.0208M/s P=8, C=1
BM_MPSC_Throughput/16/min_warmup_time:2.000/real_time                    2386402000 ns        0.000 ns            1 items_per_second=41.9041M/s P=16, C=1
BM_4x_UnevenWave_SPSClike_Aggregation/2/min_warmup_time:2.000/real_time  1464824500 ns        0.000 ns            1 items_per_second=68.2676M/s P=4, C=1 (4x Uneven producer peak relay at 0.500000)
BM_4x_UnevenWave_SPSClike_Aggregation/5/min_warmup_time:2.000/real_time  1250410200 ns        0.000 ns            1 items_per_second=79.9738M/s P=4, C=1 (4x Uneven producer peak relay at 0.800000)
BM_4x_UnevenWave_SPSClike_Aggregation/10/min_warmup_time:2.000/real_time  852162300 ns        0.000 ns            1 items_per_second=117.349M/s P=4, C=1 (4x Uneven producer peak relay at 0.900000)
BM_4x_UnevenWave_SPSClike_Aggregation/20/min_warmup_time:2.000/real_time  780626900 ns        0.000 ns            1 items_per_second=128.102M/s P=4, C=1 (4x Uneven producer peak relay at 0.950000)
BM_4x_UnevenWave_SPSClike_Aggregation/50/min_warmup_time:2.000/real_time  664480300 ns        0.000 ns            1 items_per_second=150.494M/s P=4, C=1 (4x Uneven producer peak relay at 0.980000)
BM_MPSC_Throughput_Bulk/1/min_warmup_time:2.000/real_time                 418930100 ns        0.000 ns            2 items_per_second=238.703M/s P=1, C=1
BM_MPSC_Throughput_Bulk/2/min_warmup_time:2.000/real_time                 603823900 ns        0.000 ns            1 items_per_second=165.611M/s P=2, C=1
BM_MPSC_Throughput_Bulk/4/min_warmup_time:2.000/real_time                 592862800 ns        0.000 ns            1 items_per_second=168.673M/s P=4, C=1
BM_MPSC_Throughput_Bulk/8/min_warmup_time:2.000/real_time                 603418100 ns        0.000 ns            1 items_per_second=165.723M/s P=8, C=1
BM_MPSC_Throughput_Bulk/16/min_warmup_time:2.000/real_time                619393900 ns        0.000 ns            1 items_per_second=161.448M/s P=16, C=1

moodycamel:  
moodycamel ConcurrentQueue is a MPMC queue, so this comparison is unfair.
Run on (16 X 3992 MHz CPU s)
CPU Caches:
  L1 Data 32 KiB (x8)
  L1 Instruction 32 KiB (x8)
  L2 Unified 1024 KiB (x8)
  L3 Unified 16384 KiB (x1)
-----------------------------------------------------------------------------------------------------------------------------------
Benchmark                                                                         Time             CPU   Iterations UserCounters...
-----------------------------------------------------------------------------------------------------------------------------------
BM_MPSC_Throughput/1/min_warmup_time:2.000/real_time                     2334726200 ns        0.000 ns            1 items_per_second=42.8316M/s P=1, C=1
BM_MPSC_Throughput/2/min_warmup_time:2.000/real_time                     1921455900 ns        0.000 ns            1 items_per_second=52.0439M/s P=2, C=1
BM_MPSC_Throughput/4/min_warmup_time:2.000/real_time                     1634391700 ns        0.000 ns            1 items_per_second=61.1848M/s P=4, C=1
BM_MPSC_Throughput/8/min_warmup_time:2.000/real_time                     1694708800 ns        0.000 ns            1 items_per_second=59.0072M/s P=8, C=1
BM_MPSC_Throughput/16/min_warmup_time:2.000/real_time                    2257349600 ns        0.000 ns            1 items_per_second=44.2997M/s P=16, C=1
BM_4x_UnevenWave_SPSClike_Aggregation/2/min_warmup_time:2.000/real_time  1962954000 ns        0.000 ns            1 items_per_second=50.9436M/s P=4, C=1 (4x Uneven producer peak relay at 0.500000)
BM_4x_UnevenWave_SPSClike_Aggregation/5/min_warmup_time:2.000/real_time  2089906300 ns        0.000 ns            1 items_per_second=47.849M/s P=4, C=1 (4x Uneven producer peak relay at 0.800000)
BM_4x_UnevenWave_SPSClike_Aggregation/10/min_warmup_time:2.000/real_time 2116677800 ns        0.000 ns            1 items_per_second=47.2438M/s P=4, C=1 (4x Uneven producer peak relay at 0.900000)
BM_4x_UnevenWave_SPSClike_Aggregation/20/min_warmup_time:2.000/real_time 2194175900 ns        0.000 ns            1 items_per_second=45.5752M/s P=4, C=1 (4x Uneven producer peak relay at 0.950000)
BM_4x_UnevenWave_SPSClike_Aggregation/50/min_warmup_time:2.000/real_time 2208764200 ns        0.000 ns            1 items_per_second=45.2742M/s P=4, C=1 (4x Uneven producer peak relay at 0.980000)
BM_MPSC_Throughput_Bulk/1/min_warmup_time:2.000/real_time                1577722200 ns        0.000 ns            1 items_per_second=63.3825M/s P=1, C=1
BM_MPSC_Throughput_Bulk/2/min_warmup_time:2.000/real_time                1451039700 ns        0.000 ns            1 items_per_second=68.9161M/s P=2, C=1
BM_MPSC_Throughput_Bulk/4/min_warmup_time:2.000/real_time                1491950000 ns        0.000 ns            1 items_per_second=67.0264M/s P=4, C=1
BM_MPSC_Throughput_Bulk/8/min_warmup_time:2.000/real_time                1674253600 ns        0.000 ns            1 items_per_second=59.7281M/s P=8, C=1
BM_MPSC_Throughput_Bulk/16/min_warmup_time:2.000/real_time               2299654400 ns        0.000 ns            1 items_per_second=43.4848M/s P=16, C=1
*/