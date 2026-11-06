#include "threading/ThreadPool.h"

#include <string>
#include <system_error>

namespace loganalyzer {

std::size_t ThreadPool::hardwareThreads() noexcept {
    // hardware_concurrency() may return 0 when the value is not computable.
    const unsigned count = std::thread::hardware_concurrency();
    return count == 0 ? 1 : count;
}

ThreadPool::ThreadPool(std::size_t threadCount) {
    const std::size_t count = threadCount == 0 ? hardwareThreads() : threadCount;
    workers_.reserve(count);
    try {
        for (std::size_t i = 0; i < count; ++i) {
            workers_.emplace_back(&ThreadPool::workerLoop, this);
        }
    } catch (const std::system_error& e) {
        // The destructor does not run when a constructor throws, so the
        // threads that did start must be stopped here or std::terminate is
        // called when their std::thread objects are destroyed while joinable.
        shutdown();
        throw ThreadPoolError("failed to start worker thread " + std::to_string(workers_.size() + 1) + " of " +
                              std::to_string(count) + ": " + e.what());
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    queue_.close(); // no new tasks; workers drain the queue then see nullopt
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::workerLoop() {
    while (std::optional<std::function<void()>> task = queue_.pop()) {
        // Every task is a packaged_task wrapper, which stores exceptions in
        // its future instead of throwing. The catch is a last line of defence:
        // an exception escaping a thread function would call std::terminate.
        try {
            (*task)();
        } catch (...) {
            tasksFailed_.fetch_add(1, std::memory_order_relaxed);
        }
        tasksCompleted_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace loganalyzer
