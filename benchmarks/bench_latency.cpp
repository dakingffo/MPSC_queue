#if defined(__x86_64__) || defined(__i386__)

#include "MPSC_queue.hpp" 
// #include <moodycamel/concurrentqueue.h>
#include <benchmark/benchmark.h>
#include <hdr/hdr_histogram.h>
#include <thread>
#include <atomic>
#include <vector>
#include <cstdio>

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


const double CYCLES_PER_NS    = 3.992; // My CPU
const bool   OUTPUT_DATA_FILE = false;  // Get hdr file
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
        for (int i = 0; i < 10000; ++i) {
            uint64_t start = __rdtsc();
            q.enqueue(42);
            uint64_t end = __rdtsc();
            hdr_record_value(hist, end - start);
        }
    }

    running = false;
    for (auto& t : other_producers) t.join();
    consumer.join();

    if (OUTPUT_DATA_FILE) {
        FILE* fp = fopen("mpsc_pure_enqueue_latency_dist.hgrm", "w");
        if (fp) {
            hdr_percentiles_print(hist, fp, 5, 1.0, CLASSIC); 
            fclose(fp);
        }
    }

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
        for(int i=0; i<10000; ++i) q.enqueue(i);
        state.ResumeTiming();

        for(int i=0; i<10000; ++i) {
            int val;
            uint64_t start = __rdtsc();
            if (q.try_dequeue(val)) {
                uint64_t end = __rdtsc();
                hdr_record_value(hist, end - start);
            }
        }
    }

    if (OUTPUT_DATA_FILE) {
        FILE* fp = fopen("mpsc_pure_dequeue_latency_dist.hgrm", "w");
        if (fp) {
            hdr_percentiles_print(hist, fp, 5, 1.0, CLASSIC); 
            fclose(fp);
        }
    }

    state.counters["P99_ns"] = hdr_value_at_percentile(hist, 99.0) / CYCLES_PER_NS;
    state.counters["P99.9_ns"] = hdr_value_at_percentile(hist, 99.9) / CYCLES_PER_NS;
    hdr_close(hist);
}

BENCHMARK(BM_MPSC_PureEnqueueLatency)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MPSC_PureDequeueLatency)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();

/*
daking:
Run on (16 X 3992 MHz CPU s)
CPU Caches:
  L1 Data 32 KiB (x8)
  L1 Instruction 32 KiB (x8)
  L2 Unified 1024 KiB (x8)
  L3 Unified 16384 KiB (x1)
----------------------------------------------------------------------------------------
Benchmark                              Time             CPU   Iterations UserCounters...
----------------------------------------------------------------------------------------
BM_MPSC_PureEnqueueLatency/1       0.028 us        0.027 us     25077890 P99.9_ns=80.1603 P99_ns=40.0802
BM_MPSC_PureEnqueueLatency/2       0.071 us        0.066 us     10681086 P99.9_ns=190.381 P99_ns=100.2
BM_MPSC_PureEnqueueLatency/4       0.085 us        0.079 us      8979347 P99.9_ns=300.601 P99_ns=120.24
BM_MPSC_PureEnqueueLatency/8       0.180 us        0.140 us      5931389 P99.9_ns=400.802 P99_ns=250.501
BM_MPSC_PureEnqueueLatency/16      0.618 us        0.196 us     11219862 P99.9_ns=561.373 P99_ns=340.681
BM_MPSC_PureDequeueLatency          21.9 us         20.3 us        34313 P99.9_ns=20.0401 P99_ns=10.02

moodycamel:  
moodycamel ConcurrentQueue is a MPMC queue, so this comparison is unfair.
Run on (16 X 3992 MHz CPU s)
CPU Caches:
  L1 Data 32 KiB (x8)
  L1 Instruction 32 KiB (x8)
  L2 Unified 1024 KiB (x8)
  L3 Unified 16384 KiB (x1)
  ----------------------------------------------------------------------------------------
Benchmark                              Time             CPU   Iterations UserCounters...
----------------------------------------------------------------------------------------
BM_MPSC_PureEnqueueLatency/1       0.043 us        0.043 us     15210240 P99.9_ns=340.681 P99_ns=90.1804
BM_MPSC_PureEnqueueLatency/2       0.028 us        0.028 us     23881411 P99.9_ns=1.81438k P99_ns=30.0601
BM_MPSC_PureEnqueueLatency/4       0.028 us        0.028 us     25712723 P99.9_ns=1.86448k P99_ns=20.0401
BM_MPSC_PureEnqueueLatency/8       0.132 us        0.121 us     10000000 P99.9_ns=7.03783k P99_ns=190.381
BM_MPSC_PureEnqueueLatency/16      0.736 us        0.328 us      2836924 P99.9_ns=11.3985k P99_ns=300.601
BM_MPSC_PureDequeueLatency          25.4 us         23.5 us        28047 P99.9_ns=20.0401 P99_ns=20.0401
*/

#else

int main(int argc, char** argv) {
    std::cout << "This test is only for x86." << std::endl;
    return 0;
}

#endif