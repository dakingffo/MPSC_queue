#include "gtest/gtest.h"
#include "include/MPSC_queue.hpp"
#include <thread>
#include <vector>
#include <numeric>
#include <algorithm>
#include <future>

using daking::MPSC_queue;

// Use default template parameters for testing
using TestQueue = MPSC_queue<int>;
using StringQueue = MPSC_queue<std::string>;

// -------------------------------------------------------------------------
// I. Basic Functionality Tests (SPSC Scenario)
// -------------------------------------------------------------------------

TEST(MPSCQueueBasicTest, InitialStateAndEmptyCheck) {
	TestQueue queue;
	EXPECT_TRUE(queue.empty());
	EXPECT_EQ(TestQueue::global_node_size_apprx(), 256); // ThreadLocalCapacity default is 256
}

TEST(MPSCQueueBasicTest, EnqueueAndTryDequeueSingle) {
	TestQueue queue;
	int result = 0;

	queue.enqueue(42);
	EXPECT_FALSE(queue.empty());

	EXPECT_TRUE(queue.try_dequeue(result));
	EXPECT_EQ(result, 42);
	EXPECT_TRUE(queue.empty());

	EXPECT_FALSE(queue.try_dequeue(result)); // Trying again should fail
}

TEST(MPSCQueueBasicTest, EmplaceMove) {
	StringQueue queue;
	std::string result;

	// Test emplace with copy
	queue.emplace("Hello");
	EXPECT_TRUE(queue.try_dequeue(result));
	EXPECT_EQ(result, "Hello");

	// Test enqueue with move
	std::string temp = "World";
	queue.enqueue(std::move(temp));
	EXPECT_EQ(temp.empty(), true); // Ensure temp is moved

	EXPECT_TRUE(queue.try_dequeue(result));
	EXPECT_EQ(result, "World");
	EXPECT_TRUE(queue.empty());
}

// -------------------------------------------------------------------------
// II. Memory and Resource Management Tests
// -------------------------------------------------------------------------

TEST(MPSCQueueMemoryTest, GlobalResourceSharingAndDestruction) {
	// Two instances with the same template parameters share global resources
	MPSC_queue<double, 128>* q1 = new MPSC_queue<double, 128>();
	MPSC_queue<double, 128>* q2 = new MPSC_queue<double, 128>();

	// The current thread now has 126 free nodes, q1 and q2 each have one dummy node

	// Pre-allocate some global nodes
	MPSC_queue<double, 128>::reserve_global_chunk(5);
	size_t initial_global_size = MPSC_queue<double, 128>::global_node_size_apprx();
	EXPECT_GE(initial_global_size, (size_t)5 * 128);

	delete q1;
	size_t after_delete_size = MPSC_queue<double, 128>::global_node_size_apprx();
	// q2 still exists, global resources should not be released
	EXPECT_GE(after_delete_size, (size_t)initial_global_size);

	delete q2;
	after_delete_size = MPSC_queue<double, 128>::global_node_size_apprx();
	// The last instance is destructed, global resources should be released
	EXPECT_EQ(after_delete_size, (size_t)0);
}

TEST(MPSCQueueMemoryTest, ReserveGlobalChunk) {
	using Q = MPSC_queue<long, 64>;
	size_t initial_size = Q::global_node_size_apprx(); // Initial value is 0
	Q q; // Create an instance to register global manager
	Q::reserve_global_chunk(10); // Reserve 10 * 64 nodes
	size_t reserved_size = Q::global_node_size_apprx();
	EXPECT_GE(reserved_size, initial_size + 10 * 64);

	// Reserve again, but with a smaller number, should not allocate
	Q::reserve_global_chunk(5);
	EXPECT_EQ(Q::global_node_size_apprx(), reserved_size);
}

// -------------------------------------------------------------------------
// III. Bulk Operation Tests
// -------------------------------------------------------------------------

TEST(MPSCQueueBulkTest, EnqueueBulk_ConstReference) {
	TestQueue queue;
	const int value = 99;
	const size_t n = 100;

	queue.enqueue_bulk(value, n);
	EXPECT_FALSE(queue.empty());

	int result;
	for (size_t i = 0; i < n; ++i) {
		EXPECT_TRUE(queue.try_dequeue(result));
		EXPECT_EQ(result, value);
	}
	EXPECT_TRUE(queue.empty());
}

TEST(MPSCQueueBulkTest, EnqueueBulk_InputIterator) {
	TestQueue queue;
	std::vector<int> data(50);
	std::iota(data.begin(), data.end(), 100);

	queue.enqueue_bulk(data.begin(), data.size());
	EXPECT_FALSE(queue.empty());

	int result;
	for (size_t i = 0; i < data.size(); ++i) {
		EXPECT_TRUE(queue.try_dequeue(result));
		EXPECT_EQ(result, data[i]);
	}
	EXPECT_TRUE(queue.empty());
}

TEST(MPSCQueueBulkTest, TryDequeueBulk_Partial) {
	TestQueue queue;
	queue.enqueue(1);
	queue.enqueue(2);
	queue.enqueue(3); // There are 3 elements in the queue

	std::vector<int> results;
	size_t max_fetch = 5;
	size_t count = queue.try_dequeue_bulk(std::back_inserter(results), max_fetch);

	EXPECT_EQ(count, 3);
	EXPECT_EQ(results.size(), 3);
	EXPECT_EQ(results[0], 1);
	EXPECT_EQ(results[2], 3);
	EXPECT_TRUE(queue.empty());
}

// -------------------------------------------------------------------------
// IV. Concurrency Safety Tests (MPSC Scenario)
// -------------------------------------------------------------------------

TEST(MPSCQueueConcurrentTest, MultipleProducersSingleConsumer_Bulk) {
	const size_t num_producers = 8;
	const size_t items_per_producer = 50000;
	const size_t total_items = num_producers * items_per_producer;
	const size_t bulk_size = 100;

	TestQueue queue;
	std::atomic_size_t popped_count{ 0 };
	std::atomic_bool start_flag{ false };

	std::thread consumer([&] {
		int result;
		while (!start_flag.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
		while (popped_count.load(std::memory_order_relaxed) < total_items) {
			if (queue.try_dequeue(result)) {
				popped_count.fetch_add(1, std::memory_order_relaxed);
			}
		}
		});

	std::vector<std::thread> producers;
	for (size_t i = 0; i < num_producers; ++i) {
		producers.emplace_back([&] {
			while (!start_flag.load(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
			std::vector<int> bulk_data(bulk_size); // Assume data is unimportant, only testing count
			for (size_t j = 0; j < items_per_producer; j += bulk_size) {
				queue.enqueue_bulk(bulk_data.begin(), std::min(items_per_producer - j, bulk_size));
			}
			});
	}

	// Go!
	start_flag.store(true, std::memory_order_release);
	for (auto& p : producers) {
		p.join();
	}
	consumer.join();

	EXPECT_EQ(popped_count.load(), total_items);
	EXPECT_TRUE(queue.empty());
}

// -------------------------------------------------------------------------
// IV. Custom Allocator Tests
// -------------------------------------------------------------------------

struct Counter {
	static std::size_t alloc_count;
	static std::size_t dealloc_count;
};

std::size_t Counter::alloc_count = 0;
std::size_t Counter::dealloc_count = 0;

template <typename T>
struct CountingAllocator : Counter {
	using value_type = T;
	CountingAllocator() = default;
	template <typename U>
	CountingAllocator(const CountingAllocator<U>&) {}
	// MPSC will protect allocate/deallocate with mutex.
	T* allocate(std::size_t n) {
		alloc_count += n;
		return static_cast<T*>(::operator new(n * sizeof(T)));
	}
	void deallocate(T* p, std::size_t n) noexcept {
		dealloc_count += n;
		::operator delete(p);
	}
};

TEST(MPSCQueueAllocatorTest, CustomAllocatorUsage) {
	using AllocQueue = MPSC_queue<int, 256, 64, CountingAllocator<int>>;
	AllocQueue* queue = new AllocQueue();
	const size_t n = 1000;
	for (size_t i = 0; i < n; ++i) {
		queue->enqueue(i);
	}

	int result;
	for (size_t i = 0; i < n; ++i) {
		EXPECT_TRUE(queue->try_dequeue(result));
		EXPECT_EQ(result, i);
	}
	EXPECT_TRUE(queue->empty());
	// Check that allocations and deallocations occurred
	EXPECT_GT(CountingAllocator<int>::alloc_count, 0);
	delete queue;
	EXPECT_GT(CountingAllocator<int>::dealloc_count, 0);
	EXPECT_EQ(CountingAllocator<int>::alloc_count, CountingAllocator<int>::dealloc_count);
}

// -------------------------------------------------------------------------
// VI. C++20 Blocking Operation Tests
// -------------------------------------------------------------------------

#if DAKING_HAS_CXX20_OR_ABOVE
TEST(MPSCQueueBlockTest, Dequeue_BlockAndWait) {
	TestQueue queue;
	int result = 0;
	std::atomic_bool pushed{ false };

	// Consumer thread: block and wait
	auto consumer_future = std::async(std::launch::async, [&] {
		int val = -1;
		queue.dequeue(val);
		return val;
		});

	// Wait for a short time to ensure the consumer thread starts blocking
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// Producer thread: enqueue and notify
	queue.enqueue(123);

	// Check result
	EXPECT_EQ(consumer_future.get(), 123);
	EXPECT_TRUE(queue.empty());
}

TEST(MPSCQueueBlockTest, DequeueBulk_BlockAndWait) {
	TestQueue queue;
	std::vector<int> results;
	const size_t n = 3;

	// Consumer thread: block and wait for 3 elements
	auto consumer_future = std::async(std::launch::async, [&] {
		std::vector<int> vals;
		vals.resize(n);
		queue.dequeue_bulk(vals.begin(), n);
		return vals;
		});

	// Wait for a short time to ensure the consumer thread starts blocking
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	// Producer thread: bulk enqueue and notify
	queue.enqueue(1);
	queue.enqueue(2);
	queue.enqueue(3);

	// Check result
	results = consumer_future.get();
	EXPECT_EQ(results.size(), 3);
	EXPECT_EQ(results[0], 1);
	EXPECT_EQ(results[2], 3);
	EXPECT_TRUE(queue.empty());
}
#endif