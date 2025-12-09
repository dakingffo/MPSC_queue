#pragma once

#ifndef DAKING_MPSC_QUEUE_HPP
#define DAKING_MPSC_QUEUE_HPP

#include <utility>
#include <atomic>
#include <mutex>

namespace daking {
    /*
                 SC                MP
         [tail]->[]->[]->[]->[]->[head]
         
                 SC                MP
         [tail]->[]->[]->[]->[]->[head]

                 SC                MP
         [tail]->[]->[]->[]->[]->[head]

                      ...

		 All MPSC_queue instances share a global pool of nodes to reduce memory allocation overhead.
         The consumer of each MPSC_queue could be different.

		 All producers has a thread_local pool of nodes to reduce contention on the global pool,
		 and the cost of getting nodes from the global pool is O(1) , no matter ThreadLocalCapacity is 256 or larger.
		 This is because the global pool is organized as a stack of chunks, each chunk contains ThreadLocalCapacity nodes,
		 when allocate nodes from the global pool, we always pop a chunk from the stack, this is a cheap pointer exchange operation.
		 And the consumer thread will push back the chunk to the global pool when its thread_local pool is full.

		 The chunk is freely combined of nodes, i.e., the nodes in a chunk are not required to be contiguous in memory.
		 To achieve this, every node has a next_chunk_ pointer , and all of the nodes in a chunk are linked together via next_ pointer,

		 In MPMC_queue instance, wo focus on the next_ pointer, which is used to link the nodes in the queue.
		 And in chunk_stack, we focus on the next_chunk_ pointer, which is used to link the nodes in a chunk.

		 The page list is used to manage the memory of nodes allocated from global pool, when main function ends, all pages will be deleted automatically.

         Page:
                Blue                                Green                               Red
         [ThreadLocalCapacity * node] -> [2 * ThreadLocalCapacity * node] -> [4 * ThreadLocalCapacity * node] -> ... -> nullptr
		 Color the nodes of contiguous memory with the same color for better illustration.

         GLOBAL:  
                 TOP
		 [[B][B][B][R][G][R]]     comsumers pop chunks from here and producers push chunks to here
                  ↓
         [[R][R][G][R][R][G]]
                  ↓
		 [[R][G][B][G][G][R]]     It is obvious that the nodes in a chunk are not required to be contiguous in memory.
				  ↓               Actually, they are freely combined of nodes, 
				 ...              There is no memory ABA problem because the global pool is just a stack of empty chunks, and We don't care about the order of nodes in the global pool,
				  ↓               But ABA problem still exists when read next_chunk_ and compare stack top pointer, so we use tagged pointer to avoid it.
               nullptr

		 ADVANTAGES:
		 1. Only log(N) times to lock the global mutex to new nodes, memory allocation overhead is greatly reduced.
		 2. Very fast enqueue and dequeue operations, both are O(1) operations.(Michael & Scott)
		 3. Thread local pool to reduce contention on the global pool.(Dmitry Vyukov)
		 3. Very fast allocation and deallocation of thread_local pool, both are O(1) operations by pointer exchange.
		 4. Relieve pointer chase by allocating nodes in pages.

		 DISADVANTAGES:
		 1. Slightly higher memory usage due to the next_chunk_ pointer in each node.
		 2. Can't free memory while the program is running, because all nodes have been disrupted and combined freely.
		 4. Should ensure all producers and consumer threads have ended before main function ends, otherwise will be an UB.
		 3. ThreadLocalCapacity is fixed at compile time.

		 FEATURES:
		 1. Multiple producers, single consumer.
		 2. All MPSC_queue instances share a global pool, but the consumer of each MPSC_queue could be different.
		 3. Customizable ThreadLocalCapacity and Alignment.
    */
    template <typename Ty, std::size_t ThreadLocalCapacity = 128,
        std::size_t Align = std::hardware_destructive_interference_size>
    class MPSC_queue {
    public:
		static_assert(std::is_default_constructible_v<Ty>, "Ty must be default constructible");
		static_assert((ThreadLocalCapacity & (ThreadLocalCapacity - 1)) == 0, "ThreadLocalCapacity must be a power of 2");

        using value_type = Ty;
        using size_type  = std::size_t;

        static constexpr std::size_t thread_local_capacity = ThreadLocalCapacity;
        static constexpr std::size_t align                 = Align;

    private:
        struct node {
            node()  = default;
            ~node() = default;

            value_type         value_{};
            std::atomic<node*> next_       = nullptr;
            node*              next_chunk_ = nullptr;
        };

        struct page {
            page(node* p = nullptr, page* n = nullptr) : page_(p), next_(n) {}
            ~page() {
                if (page_) {
                    delete[] page_;
                }
                if (next_) {
                    delete next_;
                }
            }

            node* page_;
            page* next_;
        };

        struct chunk_stack {
            chunk_stack()  = default;
            ~chunk_stack() = default;

            void push(node* chunk) noexcept {
                std::pair<node*, std::size_t> new_top(chunk, 0);
                std::pair<node*, std::size_t> old_top = top.load(std::memory_order_relaxed);

                do {
                    new_top.first->next_chunk_ = old_top.first;
					new_top.second = old_top.second + 1;
                } while (!top.compare_exchange_weak(
                    old_top, new_top,    
                    std::memory_order_acq_rel, 
                    std::memory_order_relaxed
                ));
            }

            bool try_pop(node*& chunk) noexcept {
                std::pair<node*, std::size_t> old_top = top.load(std::memory_order_relaxed);
                std::pair<node*, std::size_t> new_top;
                do {
                    if (!old_top.first) {
                        return false;
                    }
                    new_top.first = old_top.first->next_chunk_;
					new_top.second = old_top.second + 1;
                } while (!top.compare_exchange_weak(
                    old_top, new_top,   
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed
                ));
				chunk = old_top.first;
                return true;
            }

            std::atomic<std::pair<node*, std::size_t>> top{};
        };

    public:
        MPSC_queue() {
            node* dummy = Allocate();
            head_.store(dummy, std::memory_order_release);
            tail_ = dummy;
        }

        ~MPSC_queue() = default; // Not manage memory

        MPSC_queue(const MPSC_queue&)            = delete;
        MPSC_queue(MPSC_queue&&)                 = delete;
        MPSC_queue& operator=(const MPSC_queue&) = delete;
        MPSC_queue& operator=(MPSC_queue&&)      = delete;

        template <typename Ref>
        void enqueue(Ref&& value) {
            node* new_node = Allocate();
            new_node->value_ = std::forward<Ref>(value);
            node* old_head = head_.exchange(new_node, std::memory_order_release);
            old_head->next_.store(new_node, std::memory_order_release);
        }

        bool try_dequeue(value_type& result) noexcept {
			node* next = tail_->next_.load(std::memory_order_acquire);
            if (!next) {
                return false;
            }
            else {
                result = std::move(next->value_);
                node* old_tail = tail_;
                tail_ = next;
                Deallocate(old_tail);
                return true;
            }
        }

        bool empty_approx() const noexcept {
            std::atomic_thread_fence(std::memory_order_acquire);
            return tail_->next_ == nullptr;
		}

    private:
        node* Allocate() {
            if (!thread_local_node_list_) [[unlikely]] {
                while (!global_chunk_stack.try_pop(thread_local_node_list_)) {
					Reserve_global();
				}
            }
            node* res = std::exchange(thread_local_node_list_, thread_local_node_list_->next_);
            res->next_.store(nullptr, std::memory_order_relaxed);
            return res;
        }

        void Deallocate(node* nd) noexcept {
            thread_local static size_type count = 0;

            nd->next_ = thread_local_node_list_;
            thread_local_node_list_ = nd;
            if (++count >= thread_local_capacity) [[unlikely]] {
				global_chunk_stack.push(thread_local_node_list_);
                thread_local_node_list_ = nullptr;
                count = 0;
            }
        }

        void Reserve_global() {
			std::lock_guard<std::mutex> lock(global_mutex_);
            if (global_chunk_stack.top.load(std::memory_order_acquire).first) {
                return;
			}
            size_type count = std::max(thread_local_capacity, global_node_count_);
            node* new_nodes = new node[count];
            global_page_list_.next_ = new page(new_nodes, global_page_list_.next_);
            for (size_type i = 0; i < count - 1; i++) {
                new_nodes[i].next_ = new_nodes + i + 1;
                if ((i & (thread_local_capacity - 1)) == thread_local_capacity - 1) {
                    new_nodes[i].next_ = nullptr;
                    global_chunk_stack.push(&new_nodes[i - thread_local_capacity + 1]);
				}
            }
            global_node_count_ += count;
        }

        thread_local static node* thread_local_node_list_;

        static chunk_stack global_chunk_stack;
        static std::size_t global_node_count_;
        static page        global_page_list_;
        static std::mutex  global_mutex_;

        alignas(align) std::atomic<node*> head_;
        alignas(align) node*              tail_;
    };

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    thread_local typename MPSC_queue<Ty, ThreadLocalCapacity, Align>::node* MPSC_queue<Ty, ThreadLocalCapacity, Align>::thread_local_node_list_ = nullptr;

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    typename MPSC_queue<Ty, ThreadLocalCapacity, Align>::chunk_stack MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_chunk_stack{};

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    std::size_t MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_node_count_ = 0;

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    typename MPSC_queue<Ty, ThreadLocalCapacity, Align>::page MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_page_list_;

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    std::mutex MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_mutex_{};
}

#endif // !DAKING_MPSC_QUEUE_HPP