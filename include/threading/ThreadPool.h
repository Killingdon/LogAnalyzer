#pragma once

#include "common/Errors.h"
#include "threading/BlockingQueue.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace loganalyzer {

/// A fixed-size pool of worker threads that execute submitted tasks.
///
/// Creating a thread costs tens of microseconds; a pool creates its threads
/// once and reuses them for every task.
///
/// Lifecycle and guarantees:
/// - submit() returns a std::future. The task's return value - or the
///   exception it threw - is delivered through that future, so a failing task
///   never kills a worker thread or the process.
/// - shutdown() (also called by the destructor) stops accepting tasks, lets the
///   workers finish everything already queued, then joins them. No thread is
///   ever left running when the pool is destroyed (RAII).
class ThreadPool {
public:
    /// Starts `threadCount` workers (0 = one per hardware thread).
    /// Throws ThreadPoolError if the operating system refuses to create a thread;
    /// workers that did start are shut down cleanly first.
    explicit ThreadPool(std::size_t threadCount = 0);
    ~ThreadPool();

    // A pool owns threads that point back at `this`: copying or moving it
    // would leave those threads with a dangling pointer.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// Queues `task` for execution. Throws ThreadPoolError after shutdown().
    template <typename F>
    [[nodiscard]] auto submit(F&& task) -> std::future<std::invoke_result_t<std::decay_t<F>&>> {
        using Result = std::invoke_result_t<std::decay_t<F>&>;

        // std::function (the queue's element type) must be copyable, but a
        // packaged_task is move-only - so it lives in a shared_ptr. (C++23's
        // std::move_only_function removes the need for this.)
        auto packaged = std::make_shared<std::packaged_task<Result()>>(
            [this, fn = std::forward<F>(task)]() mutable -> Result {
                try {
                    return fn();
                } catch (...) {
                    tasksFailed_.fetch_add(1, std::memory_order_relaxed);
                    throw; // captured by packaged_task, rethrown by future::get()
                }
            });
        std::future<Result> future = packaged->get_future();

        // Count before queueing so that tasksCompleted() never exceeds tasksSubmitted().
        tasksSubmitted_.fetch_add(1, std::memory_order_relaxed);
        if (!queue_.push([packaged]() { (*packaged)(); })) {
            tasksSubmitted_.fetch_sub(1, std::memory_order_relaxed);
            throw ThreadPoolError("cannot submit a task: the thread pool has been shut down");
        }
        return future;
    }

    /// Finishes queued tasks, then joins all workers. Idempotent.
    void shutdown();

    [[nodiscard]] std::size_t threadCount() const noexcept { return workers_.size(); }

    // Statistics. Atomics let any thread read them at any time without a lock;
    // relaxed ordering suffices because they are independent counters that
    // nothing else synchronises on.
    [[nodiscard]] std::size_t tasksSubmitted() const noexcept {
        return tasksSubmitted_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t tasksCompleted() const noexcept {
        return tasksCompleted_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t tasksFailed() const noexcept { return tasksFailed_.load(std::memory_order_relaxed); }

    /// Number of hardware threads, never less than 1.
    [[nodiscard]] static std::size_t hardwareThreads() noexcept;

private:
    void workerLoop();

    BlockingQueue<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    std::atomic<std::size_t> tasksSubmitted_{0};
    std::atomic<std::size_t> tasksCompleted_{0};
    std::atomic<std::size_t> tasksFailed_{0};
};

} // namespace loganalyzer
