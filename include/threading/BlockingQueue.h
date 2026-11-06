#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace loganalyzer {

/// A thread-safe FIFO queue for producer/consumer hand-off.
///
/// - push() adds an item; if the queue has a capacity and is full, it blocks
///   until a consumer makes room (back-pressure).
/// - pop() blocks until an item is available, or the queue is closed *and*
///   drained, in which case it returns std::nullopt - the consumer's signal to stop.
/// - close() wakes every waiting thread. Items already queued are still
///   delivered, so closing never loses work.
///
/// All state is guarded by one mutex. Two condition variables avoid waking
/// producers when only consumers can make progress, and vice versa.
template <typename T>
class BlockingQueue {
public:
    /// capacity == 0 means unbounded.
    explicit BlockingQueue(std::size_t capacity = 0) : capacity_(capacity) {}

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    /// Returns false if the queue is closed; the item is then discarded.
    bool push(T item) {
        {
            std::unique_lock lock(mutex_);
            // wait() with a predicate re-checks the condition after every wake-up,
            // which protects against spurious wake-ups.
            notFull_.wait(lock, [this] { return closed_ || capacity_ == 0 || items_.size() < capacity_; });
            if (closed_) {
                return false;
            }
            items_.push_back(std::move(item));
        }
        // Notify after unlocking so the woken thread does not immediately block
        // on a mutex we still hold.
        notEmpty_.notify_one();
        return true;
    }

    /// Blocks until an item is available; std::nullopt once closed and empty.
    std::optional<T> pop() {
        std::optional<T> item;
        {
            std::unique_lock lock(mutex_);
            notEmpty_.wait(lock, [this] { return closed_ || !items_.empty(); });
            if (items_.empty()) {
                return std::nullopt; // closed and fully drained
            }
            item.emplace(std::move(items_.front()));
            items_.pop_front();
        }
        notFull_.notify_one();
        return item;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

private:
    mutable std::mutex mutex_; // mutable: locked by const observers too
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<T> items_;
    std::size_t capacity_;
    bool closed_ = false;
};

} // namespace loganalyzer
