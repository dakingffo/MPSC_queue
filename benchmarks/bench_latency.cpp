#include "MPSC_queue.hpp" 
// #include <moodycamel/concurrentqueue.h>
#include <benchmark/benchmark.h>
#include <hdr/hdr_histogram.h>
#include <thread>
#include <atomic>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <intrin.h>
#else
#include <sched.h>
#include <pthread.h>
#include <x86intrin.h>
#endif

void pin_thread(int cpu_id) {
#if defined(_WIN32) || defined(_WIN64)
    HANDLE thread = GetCurrentThread();
    DWORD_PTR mask = (static_cast<DWORD_PTR>(1) << cpu_id);
    SetThreadAffinityMask(thread, mask);
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
}


const double CYCLES_PER_NS = 3.992; // My CPU
// using TestQueue = moodycamel::ConcurrentQueue<int>;
using TestQueue = daking::MPSC_queue<int>;

static void BM_MPSC_PureEnqueueLatency(benchmark::State& state) {
    TestQueue q;
    hdr_histogram* hist;
    hdr_init(1, 1000000, 3, &hist);

    const int num_producers = (int)state.range(0);
    std::atomic<bool> running{true};
    std::atomic<bool> start_signal{false};

    std::thread consumer([&]() {
        pin_thread(0);
        int val;
        while (running.load(std::memory_order_relaxed)) {
            q.try_dequeue(val);
        }
    });

    std::vector<std::thread> other_producers;
    for (int i = 1; i < num_producers; ++i) {
        other_producers.emplace_back([&, i]() {
            pin_thread(i + 1);
            while (!start_signal.load(std::memory_order_acquire));
            while (running.load(std::memory_order_relaxed)) {
                q.enqueue(42);
            }
        });
    }

    pin_thread(1);
    start_signal.store(true, std::memory_order_release);

    for (auto _ : state) {
        uint64_t start = __rdtsc();
        q.enqueue(42);
        uint64_t end = __rdtsc();
        hdr_record_value(hist, end - start);
    }

    running = false;
    for (auto& t : other_producers) t.join();
    consumer.join();

    state.counters["P99_ns"] = hdr_value_at_percentile(hist, 99.0) / CYCLES_PER_NS;
    state.counters["P99.9_ns"] = hdr_value_at_percentile(hist, 99.9) / CYCLES_PER_NS;
    hdr_close(hist);
}

static void BM_MPSC_PureDequeueLatency(benchmark::State& state) {
    TestQueue q;
    hdr_histogram* hist;
    hdr_init(1, 1000000, 3, &hist);

    pin_thread(0);
    for (auto _ : state) {
        state.PauseTiming();
        for(int i=0; i<1000; ++i) q.enqueue(i);
        state.ResumeTiming();

        for(int i=0; i<1000; ++i) {
            int val;
            uint64_t start = __rdtsc();
            if (q.try_dequeue(val)) {
                uint64_t end = __rdtsc();
                hdr_record_value(hist, end - start);
            }
        }
    }

    state.counters["P99_ns"] = hdr_value_at_percentile(hist, 99.0) / CYCLES_PER_NS;
    state.counters["P99.9_ns"] = hdr_value_at_percentile(hist, 99.9) / CYCLES_PER_NS;
    hdr_close(hist);
}

BENCHMARK(BM_MPSC_PureEnqueueLatency)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MPSC_PureDequeueLatency)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();