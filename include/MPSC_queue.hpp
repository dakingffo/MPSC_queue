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


#ifndef DAKING_NO_TSAN
#   if defined(__has_feature) 
#       if __has_feature(thread_sanitizer)
#           define DAKING_NO_TSAN __attribute__((no_sanitize("thread")))
#       else
#           define DAKING_NO_TSAN
#       endif
#   else
#       define DAKING_NO_TSAN
#   endif
#endif // !DAKING_NO_TSAN

#ifndef DAKING_HAS_CXX20_OR_ABOVE
    #if defined(_MSC_VER) 
        #define DAKING_HAS_CXX20_OR_ABOVE _MSVC_LANG >= 202002L
    #else 
        #define DAKING_HAS_CXX20_OR_ABOVE __cplusplus >= 202002L
    #endif
#endif // !DAKING_HAS_CXX20_OR_ABOVE

#ifndef DAKING_ALWAYS_INLINE
#   if defined(_MSC_VER)
#       define DAKING_ALWAYS_INLINE [[msvc::forceinline]]
#   else
#       define DAKING_ALWAYS_INLINE [[gnu::always_inline]] 
#   endif
#endif // !DAKING_ALWAYS_INLINE

#include <type_traits>
#include <iterator>
#include <utility>
#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>

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
				 ...              ABA problem exists when read next_chunk_ and compare stack top pointer, so we use tagged pointer to avoid it.
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
        using pointer         = value_type*;
        using reference       = value_type&;
        using const_reference = const value_type&;

        static constexpr std::size_t thread_local_capacity = ThreadLocalCapacity;
        static constexpr std::size_t align                 = Align;

    private:
        struct node {
            node() {
                next_.store(nullptr, std::memory_order_release);
            }
            ~node() { /* Don't call destuctor of value_ here*/}

            union {
                value_type value_;
                node*      next_chunk_;
            };
            std::atomic<node*>  next_;
        };

        struct page {
            page(node* p = nullptr, page* n = nullptr) : page_(p), next_(n) {}
            ~page() {
                delete[] page_;
                delete next_;
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

            DAKING_ALWAYS_INLINE void reset() noexcept {
                top.store(tagged_ptr{ nullptr, 0 });
			}

            DAKING_NO_TSAN void push(node* chunk) noexcept /*Pointer Swap*/ {
                tagged_ptr new_top{ chunk, 0 };
                tagged_ptr old_top = top.load(std::memory_order_relaxed);
                // If TB read old_top, and TA pop the old_top then
                do {
                    new_top.node_->next_chunk_ = old_top.node_;
                    // then B will read a invalid value(which is regard as object address at TA)
                    // but B will not pass CAS.
                    // Actually, this is a data race, but CAS protect B form UB. 
					new_top.tag_ = old_top.tag_ + 1;
                } while (!top.compare_exchange_weak(
                    old_top, new_top,    
                    std::memory_order_acq_rel, 
                    std::memory_order_relaxed
                ));
            }

            DAKING_NO_TSAN bool try_pop(node*& chunk) noexcept /*Pointer Swap*/ {
                tagged_ptr old_top = top.load(std::memory_order_relaxed);
                tagged_ptr new_top{};

                do {
                    if (!old_top.node_) {
                        return false;
                    }
                    new_top.node_ = old_top.node_->next_chunk_;
					new_top.tag_ = old_top.tag_ + 1;
                    // If TA and TB reach here at the same time
                    // And A pop the chunk successfully, then it will construct object at old_top.node_->next_chunk_, 
                    // so that B will read a invaild value, but this value will not pass the next CAS.(old_top have been updated by A)
                    // Actually, this is a data race, but CAS protect B form UB. 
                } while (!top.compare_exchange_weak(
                    old_top, new_top,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                ));
				chunk = old_top.node_;
                return true;
            }

            std::atomic<tagged_ptr> top{};
        };

    public:
        MPSC_queue() {
            global_instance_count_++;
            Register_global_manager();
            Initial();
        }
        explicit MPSC_queue(size_type initial_global_chunk_count) : MPSC_queue() {
            reserve_global_chunk(initial_global_chunk_count);
        }

        ~MPSC_queue() {
            /* Warning: If queue is not empty, the value in left nodes will not be destructed! */
            if (--global_instance_count_ == 0) {
                // only the last instance free the global resource
				std::lock_guard<std::mutex> lock(global_mutex_);
                // if a new instance constructed before i get mutex, I do nothing.
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
        DAKING_ALWAYS_INLINE void emplace(Args&&... args) {
            node* new_node = Allocate();
            Construct_at(std::addressof(new_node->value_), std::forward<Args>(args)...);

            node* old_head = head_.exchange(new_node, std::memory_order_acq_rel);
#if DAKING_HAS_CXX20_OR_ABOVE
            node* old_head_next = old_head->next_.load(std::memory_order_relaxed);
#endif 
            old_head->next_.store(new_node, std::memory_order_release);
#if DAKING_HAS_CXX20_OR_ABOVE
            if (old_head_next == nullptr) [[unlikely]] {
                old_head->next_.notify_one();
            }
#endif 
        }

        DAKING_ALWAYS_INLINE void enqueue(const_reference value) {
            emplace(value);
        }

        DAKING_ALWAYS_INLINE void enqueue(value_type&& value) {
            emplace(std::move(value));
        }

        DAKING_ALWAYS_INLINE void enqueue_bulk(const_reference value, size_type n) {
            // N times thread_local operation, One time CAS operation.
            // So it is more efficient than N times enqueue.
            node* first_new_node = Allocate();
            node* prev_node = first_new_node;
            Construct_at(std::addressof(first_new_node->value_), value);
            for (size_type i = 1; i < n; i++) {
                node* new_node = Allocate();
                Construct_at(std::addressof(new_node->value_), value);
                prev_node->next_.store(new_node, std::memory_order_relaxed);
                prev_node = new_node;
            }
            prev_node->next_.store(nullptr, std::memory_order_relaxed);
            node* old_head = head_.exchange(prev_node, std::memory_order_acq_rel);
#if DAKING_HAS_CXX20_OR_ABOVE
            node* old_head_next = old_head->next_.load(std::memory_order_relaxed);
#endif 
            old_head->next_.store(first_new_node, std::memory_order_release);
#if DAKING_HAS_CXX20_OR_ABOVE
            if (old_head_next == nullptr) [[unlikely]] {
                old_head->next_.notify_one();
            }
#endif
        }

		template <typename InputIt>
        DAKING_ALWAYS_INLINE void enqueue_bulk(InputIt it, size_type n) {
			// Enqueue n elements from input iterator.
            static_assert(std::is_base_of_v<std::input_iterator_tag,
                typename std::iterator_traits<InputIt>::iterator_category>,
                "Iterator must be at least input iterator.");
            static_assert(std::is_same_v<typename std::iterator_traits<InputIt>::value_type, value_type>,
                "The value type of iterator must be same as MPSC_queue::value_type.");

            node* first_new_node = Allocate();
            node* prev_node = first_new_node;
            Construct_at(std::addressof(first_new_node->value_), *it++);
            for (size_type i = 1; i < n; i++) {
                node* new_node = Allocate();
                Construct_at(std::addressof(new_node->value_), *it++);
                prev_node->next_.store(new_node,  std::memory_order_relaxed);
                prev_node = new_node;
            }
            prev_node->next_.store(nullptr, std::memory_order_relaxed);
			node* old_head = head_.exchange(prev_node, std::memory_order_acq_rel);
#if DAKING_HAS_CXX20_OR_ABOVE
            node* old_head_next = old_head->next_.load(std::memory_order_relaxed);
#endif 
			old_head->next_.store(first_new_node, std::memory_order_release);
#if DAKING_HAS_CXX20_OR_ABOVE
            if (old_head_next == nullptr) [[unlikely]] {
                old_head->next_.notify_one();
            }
#endif 
		}

        template <typename T>
        DAKING_ALWAYS_INLINE bool try_dequeue(T& value) noexcept {
            static_assert(std::is_assignable_v<T&, value_type&&>);
			node* next = tail_->next_.load(std::memory_order_acquire);
            if (next) [[likely]]{
                value = std::move(next->value_);
                Destruct_at(std::addressof(next->value_));
                Deallocate(std::exchange(tail_, next));
                return true;
            }
            else {
                return false;
            }
        }

        template <typename ForwardIt>
        DAKING_ALWAYS_INLINE size_type try_dequeue_bulk(ForwardIt it, size_type n) noexcept {
            static_assert(
                std::is_base_of_v<std::forward_iterator_tag, typename std::iterator_traits<ForwardIt>::iterator_category> &&
                std::is_assignable_v<typename std::iterator_traits<ForwardIt>::reference, value_type> ||
                std::is_same_v<typename std::iterator_traits<ForwardIt>::iterator_category, std::output_iterator_tag>,
                "Iterator must be at least output iterator or forward iterator.");

			size_type count = 0;
            while (count < n && try_dequeue(*it)) {
                count++;
                it++;
            }
			return count;
        }

#if DAKING_HAS_CXX20_OR_ABOVE
        template <typename T>
        void dequeue(T& result) noexcept {
            static_assert(std::is_assignable_v<T&, value_type&&>);
            while (true) {
                if (try_dequeue(result)) {
                    return;
                }
                tail_->next_.wait(nullptr, std::memory_order_acquire);
            }
        }

		template <typename ForwardIt>
        void dequeue_bulk(ForwardIt it, size_type n) noexcept {
            static_assert(
                std::is_base_of_v<std::forward_iterator_tag, typename std::iterator_traits<ForwardIt>::iterator_category> &&
                std::is_assignable_v<typename std::iterator_traits<ForwardIt>::reference, value_type> ||
                std::is_same_v<typename std::iterator_traits<ForwardIt>::iterator_category, std::output_iterator_tag>,
                "Iterator must be at least output iterator or forward iterator.");

            size_type count = 0;
            while (count < n) {
                if (try_dequeue(*it)) {
                    count++;
                    it++;
                }
                else {
                    tail_->next_.wait(nullptr, std::memory_order_acquire);
                }
			}
        }
#endif 

        DAKING_ALWAYS_INLINE bool empty() const noexcept {
            return tail_->next_.load(std::memory_order_acquire) == nullptr;
		}

        DAKING_ALWAYS_INLINE static size_type global_node_size_apprx() noexcept {
            return global_node_count_.load(std::memory_order_acquire);
        }

        DAKING_ALWAYS_INLINE static void reserve_global_chunk(size_type chunk_count){
            Reserve_global_external(chunk_count);
        }

    private:
        DAKING_ALWAYS_INLINE void Initial() {
            node* dummy = Allocate();
            head_.store(dummy, std::memory_order_release);
            tail_ = dummy;
        }
        
        template <typename...Args> DAKING_ALWAYS_INLINE 
        void Construct_at(pointer address, Args&&...args) noexcept(std::is_nothrow_constructible_v<value_type, Args...>) {
            new (address) value_type(std::forward<Args>(args)...);
        }

        DAKING_ALWAYS_INLINE void Destruct_at(pointer address) noexcept {
            address->~value_type();
        }

        DAKING_ALWAYS_INLINE node* Allocate() {
            if (!thread_local_node_list_) [[unlikely]] {
                while (!global_chunk_stack.try_pop(thread_local_node_list_)) {
                    Reserve_global_internal();
				}
            }
            node* res = std::exchange(thread_local_node_list_, thread_local_node_list_->next_);
            res->next_.store(nullptr, std::memory_order_relaxed);
            return res;
        }

        DAKING_ALWAYS_INLINE void Deallocate(node* nd) noexcept {
            nd->next_ = thread_local_node_list_;
            thread_local_node_list_ = nd;
            if (++thread_local_node_count_ >= thread_local_capacity) [[unlikely]] {
				global_chunk_stack.push(thread_local_node_list_);
                thread_local_node_list_ = nullptr;
                thread_local_node_count_ = 0;
            }
        }

        static void Register_global_manager() noexcept {
            std::lock_guard<std::mutex> lock(global_mutex_);
            if (!global_thread_local_manager_) {
                global_thread_local_manager_ = new std::vector<std::pair<node**, size_type*>>();
            }
            global_thread_local_manager_->emplace_back(&thread_local_node_list_, &thread_local_node_count_);
        }

        static void Reserve_global_external(size_type chunk_count) {
            std::lock_guard<std::mutex> lock(global_mutex_);
            size_type global_count = global_node_count_.load(std::memory_order_relaxed);
            if (global_count / thread_local_capacity >= chunk_count) {
                return;
            }
            size_type count = (chunk_count - global_node_count_ / thread_local_capacity) * thread_local_capacity;
            node* new_nodes = new node[count];
            global_page_list_ = new page(new_nodes, global_page_list_);

            for (size_type i = 0; i < count; i++) {
                new_nodes[i].next_ = new_nodes + i + 1;
                if ((i & (thread_local_capacity - 1)) == thread_local_capacity - 1) {
                    // chunk_count = count / ThreadLocalCapacity
                    new_nodes[i].next_ .store(nullptr, std::memory_order_release);
                    // mutex don't protect global_chunk_stack, so we need release
                    global_chunk_stack.push(&new_nodes[i - thread_local_capacity + 1]);
                }
            }

            global_node_count_.store(global_count + count, std::memory_order_release);
        }

        static void Reserve_global_internal() {
			std::lock_guard<std::mutex> lock(global_mutex_);
            if (global_chunk_stack.top.load(std::memory_order_acquire).node_) {
                // if anyone have already allocate chunks, I return.
                return;
			}
            size_type global_count = global_node_count_.load(std::memory_order_relaxed);
            size_type count = std::max(thread_local_capacity, global_count);
            node* new_nodes = new node[count];
            global_page_list_ = new page(new_nodes, global_page_list_);
            for (size_type i = 0; i < count; i++) {
                new_nodes[i].next_ = new_nodes + i + 1;
                if ((i & (thread_local_capacity - 1)) == thread_local_capacity - 1) {
                    // chunk_count = count / ThreadLocalCapacity
                    new_nodes[i].next_ = nullptr;
                    std::atomic_thread_fence(std::memory_order_acq_rel);
                    // mutex don't protect global_chunk_stack, so we need make a atomice fence
                    global_chunk_stack.push(&new_nodes[i - thread_local_capacity + 1]);
				}
            }

            global_node_count_.store(global_count + count, std::memory_order_release);
        }

        static void Free_global() {
            /* Already locked */
            delete std::exchange(global_page_list_, nullptr);

            // Now all thread_local variables are invalid.
            for (auto [ptr, size] : *global_thread_local_manager_) {
                *ptr = nullptr;
                *size = 0;
            }
            delete std::exchange(global_thread_local_manager_, nullptr);

            global_node_count_.store(0, std::memory_order_release);
            global_chunk_stack.reset();
        }

        /* Global LockFree*/
        static chunk_stack                                 global_chunk_stack;
        static std::atomic_size_t                          global_instance_count_;

        /* Global Mutex*/ 
        static std::mutex                                  global_mutex_;
        static std::atomic_size_t                          global_node_count_;
        static page*                                       global_page_list_;
        static std::vector<std::pair<node**, size_type*>>* global_thread_local_manager_;

        /* ThreadLocal*/
        thread_local static node*         thread_local_node_list_;
        thread_local static size_type     thread_local_node_count_;

        /* MPSC */
        alignas(align) std::atomic<node*> head_;
        alignas(align) node*              tail_;
    };

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    thread_local typename MPSC_queue<Ty, ThreadLocalCapacity, Align>::node*
        MPSC_queue<Ty, ThreadLocalCapacity, Align>::thread_local_node_list_ = nullptr;

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    thread_local std::size_t MPSC_queue<Ty, ThreadLocalCapacity, Align>::thread_local_node_count_ = 0;

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    alignas(Align) typename MPSC_queue<Ty, ThreadLocalCapacity, Align>::chunk_stack
        MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_chunk_stack{};

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    alignas(Align) std::atomic_size_t MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_instance_count_ = 0;

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    std::mutex MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_mutex_{};

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    std::vector<std::pair<typename MPSC_queue<Ty, ThreadLocalCapacity, Align>::node**, std::size_t*>>*
        MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_thread_local_manager_{};

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    std::atomic_size_t MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_node_count_ = 0;

    template <typename Ty, std::size_t ThreadLocalCapacity, std::size_t Align>
    typename MPSC_queue<Ty, ThreadLocalCapacity, Align>::page*
        MPSC_queue<Ty, ThreadLocalCapacity, Align>::global_page_list_;
}

#endif // !DAKING_MPSC_QUEUE_HPP