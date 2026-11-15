// concurrency::concurrent_vector (the Parallel Patterns Library's), the part
// Norbyte's code uses: push_back, operator[], size, reserve and iteration.
//
// As in PPL, elements never move once constructed, so references and iterators
// stay valid while other threads append; any number of threads may append and
// read existing elements at once. Storage is a table of segments that double
// in size. Appends are serialised with a mutex (PPL's are lock-free); reads
// take no lock. As in PPL, clear() must not run concurrently with other calls.
#pragma once

#include <atomic>
#include <cstddef>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace concurrency {

template <class T, class Allocator = std::allocator<T>>
class concurrent_vector
{
    using Traits = std::allocator_traits<Allocator>;

    // Segment s holds the indices [2^(s+FirstBits) - FirstSize, 2^(s+FirstBits+1) - FirstSize).
    static constexpr std::size_t FirstBits = 4;
    static constexpr std::size_t FirstSize = std::size_t(1) << FirstBits;
    static constexpr std::size_t SegmentCount = 64 - FirstBits;

    static std::size_t SegmentOf(std::size_t index)
    {
        return std::size_t(63 - __builtin_clzll(index + FirstSize)) - FirstBits;
    }

    static std::size_t SegmentBase(std::size_t segment)
    {
        return (std::size_t(1) << (segment + FirstBits)) - FirstSize;
    }

    static std::size_t SegmentSize(std::size_t segment)
    {
        return std::size_t(1) << (segment + FirstBits);
    }

public:
    using value_type = T;
    using allocator_type = Allocator;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = T const&;

    template <class Vector, class Value>
    class Iterator
    {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = Value*;
        using reference = Value&;

        Iterator() = default;
        Iterator(Vector* vector, std::size_t index) : vector_(vector), index_(index) {}
        // iterator -> const_iterator
        template <class V, class U>
        Iterator(Iterator<V, U> const& o) : vector_(o.vector_), index_(o.index_) {}

        reference operator*() const { return (*vector_)[index_]; }
        pointer operator->() const { return &(*vector_)[index_]; }
        reference operator[](difference_type n) const { return (*vector_)[index_ + n]; }

        Iterator& operator++() { ++index_; return *this; }
        Iterator operator++(int) { auto it = *this; ++index_; return it; }
        Iterator& operator--() { --index_; return *this; }
        Iterator operator--(int) { auto it = *this; --index_; return it; }
        Iterator& operator+=(difference_type n) { index_ += n; return *this; }
        Iterator& operator-=(difference_type n) { index_ -= n; return *this; }
        Iterator operator+(difference_type n) const { return Iterator(vector_, index_ + n); }
        friend Iterator operator+(difference_type n, Iterator const& it) { return it + n; }
        Iterator operator-(difference_type n) const { return Iterator(vector_, index_ - n); }
        template <class V, class U>
        difference_type operator-(Iterator<V, U> const& o) const { return difference_type(index_) - difference_type(o.index_); }

        template <class V, class U>
        bool operator==(Iterator<V, U> const& o) const { return index_ == o.index_; }
        template <class V, class U>
        auto operator<=>(Iterator<V, U> const& o) const { return index_ <=> o.index_; }

    private:
        template <class V, class U> friend class Iterator;

        Vector* vector_{ nullptr };
        std::size_t index_{ 0 };
    };

    using iterator = Iterator<concurrent_vector, T>;
    using const_iterator = Iterator<concurrent_vector const, T const>;

    concurrent_vector() = default;
    explicit concurrent_vector(Allocator const& alloc) : alloc_(alloc) {}
    concurrent_vector(concurrent_vector const&) = delete;
    concurrent_vector& operator=(concurrent_vector const&) = delete;

    ~concurrent_vector()
    {
        clear();
        for (std::size_t s = 0; s < SegmentCount; s++) {
            if (auto seg = segments_[s].load(std::memory_order_relaxed)) {
                Traits::deallocate(alloc_, seg, SegmentSize(s));
            }
        }
    }

    iterator push_back(T const& value)
    {
        return emplace_back(value);
    }

    iterator push_back(T&& value)
    {
        return emplace_back(std::move(value));
    }

    template <class... Args>
    iterator emplace_back(Args&&... args)
    {
        std::lock_guard lock(mutex_);
        auto index = size_.load(std::memory_order_relaxed);
        auto segment = SegmentOf(index);
        auto storage = Allocate(segment);
        Traits::construct(alloc_, storage + (index - SegmentBase(segment)), std::forward<Args>(args)...);
        size_.store(index + 1, std::memory_order_release);
        return iterator(this, index);
    }

    // Allocates storage for at least `count` elements; constructs nothing.
    void reserve(size_type count)
    {
        if (count == 0) return;
        std::lock_guard lock(mutex_);
        auto last = SegmentOf(count - 1);
        for (std::size_t s = 0; s <= last; s++) {
            Allocate(s);
        }
    }

    reference operator[](size_type index)
    {
        auto segment = SegmentOf(index);
        return segments_[segment].load(std::memory_order_acquire)[index - SegmentBase(segment)];
    }

    const_reference operator[](size_type index) const
    {
        auto segment = SegmentOf(index);
        return segments_[segment].load(std::memory_order_acquire)[index - SegmentBase(segment)];
    }

    size_type size() const
    {
        return size_.load(std::memory_order_acquire);
    }

    bool empty() const
    {
        return size() == 0;
    }

    // Destroys every element; the storage is kept.
    void clear()
    {
        std::lock_guard lock(mutex_);
        auto count = size_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < count; i++) {
            auto segment = SegmentOf(i);
            Traits::destroy(alloc_, segments_[segment].load(std::memory_order_relaxed) + (i - SegmentBase(segment)));
        }
        size_.store(0, std::memory_order_release);
    }

    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, size()); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, size()); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    allocator_type get_allocator() const { return alloc_; }

private:
    // Called with mutex_ held.
    T* Allocate(std::size_t segment)
    {
        auto storage = segments_[segment].load(std::memory_order_relaxed);
        if (!storage) {
            storage = Traits::allocate(alloc_, SegmentSize(segment));
            segments_[segment].store(storage, std::memory_order_release);
        }
        return storage;
    }

    [[no_unique_address]] Allocator alloc_;
    std::mutex mutex_;
    std::atomic<size_type> size_{ 0 };
    std::atomic<T*> segments_[SegmentCount]{};
};

}

namespace Concurrency = concurrency;
