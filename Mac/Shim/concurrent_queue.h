// concurrency::concurrent_queue (the Parallel Patterns Library's), the part
// Norbyte's code uses: push, try_pop, empty, unsafe_size and clear.
//
// A FIFO that any number of threads may push to and pop from at once. This
// one serialises them with a mutex; PPL's is lock-free, which is a difference
// in speed, not in behaviour. As in PPL, clear() and unsafe_size() must not
// run concurrently with other calls, and the element type may be incomplete
// where a queue is declared (Norbyte's headers declare a queue of SDL_Event
// with only its forward declaration); it is complete where a member function
// is used.
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace concurrency {

template <class T, class Allocator = std::allocator<T>>
class concurrent_queue
{
    // A singly linked list, oldest first. Node is defined, and T needed, only
    // in the member functions.
    struct Node;
    using NodeAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<Node>;
    using NodeTraits = std::allocator_traits<NodeAllocator>;

public:
    using value_type = T;
    using allocator_type = Allocator;
    using size_type = std::size_t;
    using reference = T&;
    using const_reference = T const&;

    concurrent_queue() = default;
    explicit concurrent_queue(Allocator const& alloc) : alloc_(alloc) {}
    concurrent_queue(concurrent_queue const&) = delete;
    concurrent_queue& operator=(concurrent_queue const&) = delete;

    ~concurrent_queue()
    {
        clear();
    }

    void push(T const& value)
    {
        Append(value);
    }

    void push(T&& value)
    {
        Append(std::move(value));
    }

    // Moves the oldest element into `dest`; false (and `dest` unchanged) when empty.
    bool try_pop(T& dest)
    {
        Node* node;
        {
            std::lock_guard lock(mutex_);
            node = head_;
            if (node == nullptr) {
                return false;
            }
            head_ = node->Next;
            if (head_ == nullptr) {
                tail_ = nullptr;
            }
            size_--;
        }
        dest = std::move(node->Value);
        Destroy(node);
        return true;
    }

    bool empty() const
    {
        std::lock_guard lock(mutex_);
        return head_ == nullptr;
    }

    size_type unsafe_size() const
    {
        std::lock_guard lock(mutex_);
        return size_;
    }

    void clear()
    {
        Node* node;
        {
            std::lock_guard lock(mutex_);
            node = head_;
            head_ = tail_ = nullptr;
            size_ = 0;
        }
        while (node != nullptr) {
            Node* next = node->Next;
            Destroy(node);
            node = next;
        }
    }

    allocator_type get_allocator() const
    {
        return allocator_type(alloc_);
    }

private:
    struct Node
    {
        template <class U>
        explicit Node(U&& value) : Value(std::forward<U>(value)) {}

        Node* Next{ nullptr };
        T Value;
    };

    template <class U>
    void Append(U&& value)
    {
        // Constructed outside the lock: a throwing copy leaves the queue as it was.
        Node* node = NodeTraits::allocate(alloc_, 1);
        try {
            NodeTraits::construct(alloc_, node, std::forward<U>(value));
        } catch (...) {
            NodeTraits::deallocate(alloc_, node, 1);
            throw;
        }

        std::lock_guard lock(mutex_);
        if (tail_ != nullptr) {
            tail_->Next = node;
        } else {
            head_ = node;
        }
        tail_ = node;
        size_++;
    }

    void Destroy(Node* node)
    {
        NodeTraits::destroy(alloc_, node);
        NodeTraits::deallocate(alloc_, node, 1);
    }

    mutable std::mutex mutex_;
    [[no_unique_address]] NodeAllocator alloc_;
    Node* head_{ nullptr };
    Node* tail_{ nullptr };
    size_type size_{ 0 };
};

}

namespace Concurrency = concurrency;
