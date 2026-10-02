/* Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SPSC_QUEUE_HPP
#define SPSC_QUEUE_HPP

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace kws {

// Storage is allocated before either endpoint starts. One producer and one
// consumer own their respective cursors; neither endpoint waits or allocates.
template<class T>
class SpscQueue {
public:
    explicit SpscQueue(size_t capacity) : slots_(capacity) {}
    std::vector<T>& storage() { return slots_; } // Initialization only.
    size_t capacity() const { return slots_.size(); }
    size_t freeSlots() const noexcept {
        const size_t write = write_.load(std::memory_order_relaxed);
        return capacity() - (write - read_.load(std::memory_order_acquire));
    }
    T& writeSlot(size_t ahead = 0) noexcept {
        return slots_[(write_.load(std::memory_order_relaxed) + ahead) % capacity()];
    }
    void publish(size_t count = 1) noexcept {
        write_.store(write_.load(std::memory_order_relaxed) + count, std::memory_order_release);
    }
    const T* front() const noexcept {
        const size_t read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return nullptr;
        return &slots_[read % capacity()];
    }
    void pop() noexcept {
        read_.store(read_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
    void discard() noexcept { // Consumer only, or after both endpoints stop.
        read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    }
    size_t size() const noexcept { // Approximate observer snapshot.
        const size_t read = read_.load(std::memory_order_acquire);
        return std::min(capacity(), write_.load(std::memory_order_acquire) - read);
    }
private:
    static_assert(std::atomic<size_t>::is_always_lock_free, "Audio cursors must be lock-free");
    std::vector<T> slots_;
    alignas(64) std::atomic<size_t> read_{0};
    alignas(64) std::atomic<size_t> write_{0};
};
}  // namespace kws
#endif
