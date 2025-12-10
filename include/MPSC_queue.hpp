/*
MIT License

Copyright (c) 2025 dakingffo

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#if defined(_MSC_VER) && _MSC_VER > 1000 || defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 3)
#pragma once
#endif

#ifndef DAKING_MPSC_QUEUE_HPP
#define DAKING_MPSC_QUEUE_HPP

#include <utility>
#include <atomic>
#include <mutex>
#include <iostream>
#include <vector>

namespace daking {
    /*
                 SC                MP
         [tail]->[]->[]->[]->[]->[head]
         
                 SC                MP
         [tail]->[]->[]->[]->[]->[head]

                 SC                MP
         [tail]->[]->[]->[]->[]->[head]

                      ...

		 Although all alive MPSC_queue instances share a global pool of nodes to reduce memory allocation overhead,
         the consumer of each MPSC_queue could be different.

		 All producers has a thread_local pool of nodes to reduce contention on the global pool,
		 and the cost of getting nodes from the global pool is O(1) , no matter ThreadLocalCapacity is 256 or larger.
		 This is because the global pool is organized as a stack of chunks, each chunk contains ThreadLocalCapacity nodes,
		 when allocate nodes from the global pool, we always pop a chunk from the stack, this is a cheap pointer exchange operation.
		 And the consumer thread will push back the chunk to the global pool when its thread_local pool is full.

		 The chunk is freely combined of nodes, and the nodes in a chunk are not required to be contiguous in memory.
		 To achieve this, every node has a next_chunk_ pointer , and all of the nodes in a chunk are linked together via next_ pointer,

		 In MPMC_queue instance, wo focus on the next_ pointer, which is used to link the nodes in the queue.
		 And in chunk_stack, we focus on the next_chunk_ pointer, which is used to link the nodes in a chunk.

		 The page list is used to manage the memory of nodes allocated from global pool, when the last instance is destructed, all pages will be deleted automatically.

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
    */
    template <typename Ty, std::size_t ThreadLocalCapacity = 256,
        std::size_t Align = 64 /* std::hardware_destructive_interference_size */>
    class MPSC_queue {
    public:
        static_assert(std::is_object_v<Ty>, "Ty must be object.");
        static_assert(std::is_move_constructible_v<Ty>, "Ty must be move constructible.");
		static_assert((ThreadLocalCapacity & (ThreadLocalCapacity - 1)) == 0, "ThreadLocalCapacity must be a power of 2.");

        using value_type      = Ty;
        using size_type       = std::size_t;
        using reference       = value_type&;
        using const_reference = const value_type&;

        static constexpr std::size_t thread_local_capacity = ThreadLocalCapacity;
        static constexpr std::size_t align                 = Align;

    private:
        struct node {
            node() : next_chunk_(nullptr), next_(nullptr) {}
            ~node() = default;

            union {
                value_type      value_;
                node*           next_chunk_;
            };
            std::atomic<node*>  next_;
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
            struct tagged_ptr {
                node*     node_ = nullptr;
                size_type tag_  = 0;
            };

            chunk_stack()  = default;
            ~chunk_stack() = default;

            void reset() noexcept {
                top.store(tagged_ptr{ nullptr, 0 });
			}

            void push(node* chunk) noexcept {
                tagged_ptr new_top{ chunk, 0 };
                tagged_ptr old_top = top.load(std::memory_order_relaxed);

                do {
                    new_top.node_->next_chunk_ = old_top.node_;
					new_top.tag_ = old_top.tag_ + 1;
                } while (!top.compare_exchange_weak(
                    old_top, new_top,    
                    std::memory_order_acq_rel, 
                    std::memory_order_relaxed
                ));
            }

            bool try_pop(node*& chunk) noexcept {
                tagged_ptr old_top = top.load(std::memory_order_relaxed);
                tagged_ptr new_top;

                do {
                    if (!old_top.node_) {
                        return false;
                    }
                    new_top.node_ = old_top.node_->next_chunk_;
					new_top.tag_ = old_top.tag_ + 1;
                } while (!top.compare_exchange_weak(
                    old_top, new_top,   
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed
                ));

				chunk = old_top.node_;
                return true;
            }

            std::atomic<tagged_ptr> top{};
        };

    public:
        MPSC_queue() {
            global_instance_count_++;
            {   
                std::lock_guard<std::mutex> lock(global_mutex_);
                global_thread_local_manager_.push_back({ &thread_local_node_list_, &thread_local_node_count_ });
            }
            Initial();
        }

        ~MPSC_queue() {
            Final();
            if (--global_instance_count_ == 0) {
				std::lock_guard<std::mutex> lock(global_mutex_);
                if (global_instance_count_ == 0) {
                    Free_global();
                }
            }
        }

        MPSC_queue(const MPSC_queue&)            = delete;
        MPSC_queue(MPSC_queue&&)                 = delete;
        MPSC_queue& operator=(const MPSC_queue&) = delete;
        MPSC_queue& operator=(MPSC_queue&&)      = delete;

        template <typename...Args>
        void emplace(Args&&... args) {
            node* new_node = Allocate();
            new (std::addressof(new_node->value_)) value_type(std::forward<Args>(args)...);

            node* old_head = head_.exchange(new_node, std::memory_order_release);
            old_head->next_.store(new_node, std::memory_order_release);
        }

        void enqueue(const_reference value) {
            emplace(value);
        }

        void enqueue(value_type&& value) {
            emplace(std::move(value));
        }

        bool try_dequeue(value_type& result) noexcept {
			node* next = tail_->next_.load(std::memory_order_acquire);
            if (next) [[likely]]{
                result = std::move(next->value_);
                next->value_.~value_type();

                node* old_tail = tail_;
                tail_ = next;
                Deallocate(old_tail);
                return true;
            }
            else {
                return false;
            }
        }

        bool empty_approx() const noexcept {
            return tail_->next_.load(std::memory_order_acquire) == nullptr;
		}

    private:
        void Initial() {
            node* dummy = Allocate();
            head_.store(dummy, std::memory_order_release);
            tail_ = dummy;
        }

        void Final() {
            while (tail_->next_) {
                tail_ = tail_->next_;
                tail_->value_.~value_type();
            }
        }

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
            nd->next_ = thread_local_node_list_;
            thread_local_node_list_ = nd;
            if (++thread_local_node_count_ >= thread_local_capacity) [[unlikely]] {
				global_chunk_stack.push(thread_local_node_list_);
                thread_local_node_list_ = nullptr;
                thread_local_node_count_ = 0;
            }
        }

        void Reserve_global() {
			std::lock_guard<std::mutex> lock(global_mutex_);
            if (global_chunk_stack.top.load(std::memory_order_acquire).node_) {
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

        void Free_global() {
            /* Already locked */
            delete global_page_list_.next_;
            global_page_list_.next_ = nullptr;

            for (auto [ptr, size] : global_thread_local_manager_) {
                *ptr = nullptr;
                *size = 0;
            }
            global_thread_local_manager_.clear();

            global_node_count_ = 0;
            global_chunk_stack.reset();
        }

        static chunk_stack                                global_chunk_stack;
        static std::atomic_size_t                         global_instance_count_;

        static std::mutex                                 global_mutex_;
        static size_type                                  global_node_count_;
        static page                                       global_page_list_;
        static std::vector<std::pair<node**, size_type*>> global_thread_local_manager_;

        thread_local static node*         thread_local_node_list_;
        thread_local static size_type     thread_local_node_count_;

        alignas(align) std::atomic<node*> head_;
        alignas(align) node*              tail_;
    };

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    thread_local typename MPSC_queue<Ty, ThreadLocalCapacity, Align>::node* 
        MPSC_queue<Ty, ThreadLocalCapacity, Align>::thread_local_node_list_ = nullptr;

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    thread_local std::size_t MPSC_queue<Ty, ThreadLocalCapacity, Align>::thread_local_node_count_ = 0;

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    typename MPSC_queue<Ty, ThreadLocalCapacity, Align>::chunk_stack 
        MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_chunk_stack{};

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    std::atomic_size_t MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_instance_count_ = 0;

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    std::mutex MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_mutex_{};

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    std::vector<std::pair<typename MPSC_queue<Ty, ThreadLocalCapacity, Align>::node**, std::size_t*>>
        MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_thread_local_manager_{};

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    std::size_t MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_node_count_ = 0;

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    typename MPSC_queue<Ty, ThreadLocalCapacity, Align>::page 
        MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_page_list_;
}

#endif // !DAKING_MPSC_QUEUE_HPP