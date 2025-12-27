#include "MPSC_queue.hpp" 
#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <iostream>

#ifdef DAKING_HAS_CXX20_OR_ABOVE

struct Message {
    int producer_id;
    uint64_t seq;
};

using LinearQueue = daking::MPSC_queue<Message>;

static void BM_MPSC_Test_Linearizable(benchmark::State& state) {
    const int num_producers = (int)state.range(0);
    const uint64_t ops_per_producer = 1000000;
    
    for (auto _ : state) {
        state.PauseTiming();
        
        LinearQueue q;
        std::atomic<bool> start_signal{false};
        std::atomic<uint64_t> total_received{0};
        std::atomic<bool> linear_error{false};
        
        std::vector<uint64_t> last_seq(num_producers, 0);

        std::thread consumer([&]() {
            while (!start_signal.load(std::memory_order_acquire));
            
            const uint64_t total_expected = ops_per_producer * num_producers;
            for (uint64_t i = 0; i < total_expected; ++i) {
                Message msg;
                q.dequeue(msg); 
                if (msg.seq <= last_seq[msg.producer_id] && last_seq[msg.producer_id] != 0) {
                    linear_error.store(true, std::memory_order_relaxed);
                }
                last_seq[msg.producer_id] = msg.seq;
                total_received.fetch_add(1, std::memory_order_relaxed);
            }
        });

        std::vector<std::thread> producers;
        producers.reserve(num_producers);
        for (int p_id = 0; p_id < num_producers; ++p_id) {
            producers.emplace_back([&, p_id]() {
                while (!start_signal.load(std::memory_order_acquire));
                for (uint64_t s = 1; s <= ops_per_producer; ++s) {
                    q.enqueue({p_id, s});
                }
            });
        }

        state.ResumeTiming();
        start_signal.store(true, std::memory_order_release);

        for (auto& p : producers) p.join();
        consumer.join();

        if (linear_error.load(std::memory_order_relaxed)) {
            state.SkipWithError("Linearizability Violation Detected!");
            break; 
        }
    }

    state.SetItemsProcessed(state.range(0) * ops_per_producer * state.iterations());
    state.SetLabel("Linearizable when " + std::to_string(state.range(0)) + "P.");
}

BENCHMARK(BM_MPSC_Test_Linearizable)
    ->Arg(1)
    ->Arg(4)
    ->Arg(16)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();

#else

int main(int argc, char** argv) {
    std::cout << "This test is only for C++20 and above. Modify CMakeLists.txt \"set(CMAKE_CXX_STANDARD 17)\"->\"set(CMAKE_CXX_STANDARD 20)\"" << std::endl;
    return 0;
}

#endif