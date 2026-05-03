#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "concurrentqueue.h"
#include "daking/MPSC_queue.hpp"

namespace {

constexpr std::size_t kTotalOps = 2000000;
constexpr std::size_t kBulkSize = 32;

struct Message {
    int producer_id;
    std::uint64_t sequence;
};

template <typename Queue>
struct QueueTraits;

template <typename T>
struct QueueTraits<daking::MPSC_queue<T>> {
    static constexpr const char* name = "daking_mpsc";

    static void enqueue(daking::MPSC_queue<T>& queue, T value) {
        queue.enqueue(std::move(value));
    }

    static bool try_dequeue(daking::MPSC_queue<T>& queue, T& value) {
        return queue.try_dequeue(value);
    }

    template <typename Iterator>
    static void enqueue_bulk(daking::MPSC_queue<T>& queue, Iterator begin, std::size_t count) {
        queue.enqueue_bulk(begin, count);
    }
};

template <typename T>
struct QueueTraits<moodycamel::ConcurrentQueue<T>> {
    static constexpr const char* name = "moody_mpmc";

    static void enqueue(moodycamel::ConcurrentQueue<T>& queue, T value) {
        queue.enqueue(std::move(value));
    }

    static bool try_dequeue(moodycamel::ConcurrentQueue<T>& queue, T& value) {
        return queue.try_dequeue(value);
    }

    template <typename Iterator>
    static void enqueue_bulk(moodycamel::ConcurrentQueue<T>& queue, Iterator begin, std::size_t count) {
        queue.enqueue_bulk(begin, count);
    }
};

template <typename Queue>
void drain_consumer(Queue& queue, std::size_t total_items, std::atomic_bool& start) {
    using Traits = QueueTraits<Queue>;

    Message message{};
    std::size_t popped_count = 0;
    while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    while (popped_count < total_items) {
        if (Traits::try_dequeue(queue, message)) {
            ++popped_count;
        }
        else {
            std::this_thread::yield();
        }
    }
}

template <typename Queue>
void run_uniform_single(std::size_t producer_count) {
    using Traits = QueueTraits<Queue>;

    Queue queue;
    std::atomic_bool start{ false };
    const std::size_t items_per_producer = kTotalOps / producer_count;
    const std::size_t total_items = items_per_producer * producer_count;

    std::thread consumer(drain_consumer<Queue>, std::ref(queue), total_items, std::ref(start));

    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::uint64_t sequence = 0; sequence < items_per_producer; ++sequence) {
                Traits::enqueue(queue, Message{ static_cast<int>(producer), sequence });
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& producer : producers) {
        producer.join();
    }
    consumer.join();
}

template <typename Queue>
void run_uneven_wave(std::size_t relay_denominator) {
    using Traits = QueueTraits<Queue>;

    Queue queue;
    constexpr std::size_t producer_count = 4;
    const std::size_t items_per_producer = kTotalOps / producer_count;
    const std::size_t total_items = items_per_producer * producer_count;
    std::vector<std::atomic_bool> producer_start(producer_count);
    std::atomic_bool consumer_start{ false };

    std::thread consumer(drain_consumer<Queue>, std::ref(queue), total_items, std::ref(consumer_start));

    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            while (!producer_start[producer].load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const std::size_t relay_at = items_per_producer - items_per_producer / relay_denominator;
            for (std::uint64_t sequence = 0; sequence < items_per_producer; ++sequence) {
                Traits::enqueue(queue, Message{ static_cast<int>(producer), sequence });
                if (sequence == relay_at && producer + 1 < producer_count) {
                    producer_start[producer + 1].store(true, std::memory_order_release);
                }
            }
        });
    }

    consumer_start.store(true, std::memory_order_release);
    producer_start[0].store(true, std::memory_order_release);

    for (auto& producer : producers) {
        producer.join();
    }
    consumer.join();
}

template <typename Queue>
void run_bulk(std::size_t producer_count) {
    using Traits = QueueTraits<Queue>;

    Queue queue;
    std::atomic_bool start{ false };
    const std::size_t items_per_producer = kTotalOps / producer_count;
    const std::size_t total_items = items_per_producer * producer_count;

    std::thread consumer(drain_consumer<Queue>, std::ref(queue), total_items, std::ref(start));

    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            std::array<Message, kBulkSize> batch{};
            for (std::size_t index = 0; index < batch.size(); ++index) {
                batch[index] = Message{ static_cast<int>(producer), static_cast<std::uint64_t>(index) };
            }

            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t sent = 0; sent < items_per_producer; sent += kBulkSize) {
                const std::size_t count = std::min(kBulkSize, items_per_producer - sent);
                Traits::enqueue_bulk(queue, batch.begin(), count);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& producer : producers) {
        producer.join();
    }
    consumer.join();
}

template <typename Queue>
void bm_uniform_single(benchmark::State& state) {
    const std::size_t producer_count = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        run_uniform_single<Queue>(producer_count);
    }
    state.SetItemsProcessed(static_cast<int64_t>(kTotalOps * state.iterations()));
    state.SetLabel(std::string(QueueTraits<Queue>::name) + ", uniform single enqueue");
}

template <typename Queue>
void bm_uneven_wave(benchmark::State& state) {
    const std::size_t relay_denominator = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        run_uneven_wave<Queue>(relay_denominator);
    }
    state.SetItemsProcessed(static_cast<int64_t>(kTotalOps * state.iterations()));
    state.SetLabel(
        std::string(QueueTraits<Queue>::name) +
        ", 4P uneven relay " +
        std::to_string(1.0 - 1.0 / static_cast<double>(relay_denominator))
    );
}

template <typename Queue>
void bm_bulk(benchmark::State& state) {
    const std::size_t producer_count = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        run_bulk<Queue>(producer_count);
    }
    state.SetItemsProcessed(static_cast<int64_t>(kTotalOps * state.iterations()));
    state.SetLabel(std::string(QueueTraits<Queue>::name) + ", bulk enqueue");
}

using DakingQueue = daking::MPSC_queue<Message>;
using MoodyQueue = moodycamel::ConcurrentQueue<Message>;

} // namespace

BENCHMARK_TEMPLATE(bm_uniform_single, DakingQueue)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime();
BENCHMARK_TEMPLATE(bm_uniform_single, MoodyQueue)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime();

BENCHMARK_TEMPLATE(bm_uneven_wave, DakingQueue)->Arg(2)->Arg(10)->Arg(50)->UseRealTime();
BENCHMARK_TEMPLATE(bm_uneven_wave, MoodyQueue)->Arg(2)->Arg(10)->Arg(50)->UseRealTime();

BENCHMARK_TEMPLATE(bm_bulk, DakingQueue)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime();
BENCHMARK_TEMPLATE(bm_bulk, MoodyQueue)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime();
