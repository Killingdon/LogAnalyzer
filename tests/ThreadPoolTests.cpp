#include "common/Errors.h"
#include "threading/BlockingQueue.h"
#include "threading/ThreadPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace loganalyzer;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// BlockingQueue
// ---------------------------------------------------------------------------

TEST(BlockingQueueTest, FifoOrder) {
    BlockingQueue<int> queue;
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(queue.push(i));
    }
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(queue.pop(), i);
    }
}

TEST(BlockingQueueTest, CloseDrainsRemainingItemsThenSignalsEnd) {
    BlockingQueue<int> queue;
    queue.push(1);
    queue.push(2);
    queue.close();
    EXPECT_FALSE(queue.push(3)); // rejected after close
    EXPECT_EQ(queue.pop(), 1);
    EXPECT_EQ(queue.pop(), 2);
    EXPECT_EQ(queue.pop(), std::nullopt);
}

TEST(BlockingQueueTest, CloseWakesBlockedConsumer) {
    BlockingQueue<int> queue;
    std::optional<int> received = 42;
    std::thread consumer([&] { received = queue.pop(); });
    std::this_thread::sleep_for(20ms); // let the consumer block
    queue.close();
    consumer.join();
    EXPECT_EQ(received, std::nullopt);
}

TEST(BlockingQueueTest, BoundedQueueBlocksProducerUntilSpaceAvailable) {
    BlockingQueue<int> queue(2);
    queue.push(1);
    queue.push(2);
    std::atomic<bool> thirdPushed{false};
    std::thread producer([&] {
        queue.push(3); // must block: queue is full
        thirdPushed = true;
    });
    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(thirdPushed.load());
    EXPECT_EQ(queue.pop(), 1); // make room
    producer.join();
    EXPECT_TRUE(thirdPushed.load());
    EXPECT_EQ(queue.size(), 2u);
}

TEST(BlockingQueueTest, ManyProducersManyConsumersLoseNothing) {
    constexpr int kProducers = 4;
    constexpr int kItemsPerProducer = 5000;
    BlockingQueue<int> queue(64);
    std::atomic<long long> sum{0};
    std::atomic<int> count{0};

    std::vector<std::thread> consumers;
    for (int c = 0; c < 4; ++c) {
        consumers.emplace_back([&] {
            while (auto item = queue.pop()) {
                sum += *item;
                ++count;
            }
        });
    }
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kItemsPerProducer; ++i) {
                queue.push(p * kItemsPerProducer + i);
            }
        });
    }
    for (auto& t : producers) {
        t.join();
    }
    queue.close();
    for (auto& t : consumers) {
        t.join();
    }
    constexpr long long n = static_cast<long long>(kProducers) * kItemsPerProducer;
    EXPECT_EQ(count.load(), n);
    EXPECT_EQ(sum.load(), n * (n - 1) / 2);
}

// ---------------------------------------------------------------------------
// ThreadPool
// ---------------------------------------------------------------------------

TEST(ThreadPoolTest, StartsRequestedNumberOfThreads) {
    ThreadPool pool(3);
    EXPECT_EQ(pool.threadCount(), 3u);
    ThreadPool automatic(0);
    EXPECT_EQ(automatic.threadCount(), ThreadPool::hardwareThreads());
}

TEST(ThreadPoolTest, ReturnsTaskResultsThroughFutures) {
    ThreadPool pool(4);
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 100; ++i) {
        futures.push_back(pool.submit([i] { return i * i; }));
    }
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(futures[static_cast<std::size_t>(i)].get(), i * i);
    }
}

TEST(ThreadPoolTest, RunsTasksOnMultipleThreads) {
    ThreadPool pool(4);
    std::mutex mutex;
    std::set<std::thread::id> ids;
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 64; ++i) {
        futures.push_back(pool.submit([&] {
            std::this_thread::sleep_for(2ms);
            const std::lock_guard lock(mutex);
            ids.insert(std::this_thread::get_id());
        }));
    }
    for (auto& f : futures) {
        f.get();
    }
    EXPECT_GT(ids.size(), 1u);
    EXPECT_LE(ids.size(), 4u);
    EXPECT_EQ(ids.count(std::this_thread::get_id()), 0u); // never on the caller's thread
}

TEST(ThreadPoolTest, ExceptionIsDeliveredThroughFutureAndPoolKeepsWorking) {
    ThreadPool pool(2);
    auto failing = pool.submit([]() -> int { throw std::runtime_error("boom"); });
    EXPECT_THROW(failing.get(), std::runtime_error);

    // The worker survived the exception and still executes tasks.
    auto ok = pool.submit([] { return 7; });
    EXPECT_EQ(ok.get(), 7);
    EXPECT_EQ(pool.tasksFailed(), 1u);
}

TEST(ThreadPoolTest, AcceptsMoveOnlyTasks) {
    ThreadPool pool(1);
    auto data = std::make_unique<int>(41);
    auto future = pool.submit([owned = std::move(data)] { return *owned + 1; });
    EXPECT_EQ(future.get(), 42);
}

TEST(ThreadPoolTest, ShutdownCompletesQueuedTasksAndRejectsNewOnes) {
    std::atomic<int> executed{0};
    ThreadPool pool(2);
    for (int i = 0; i < 50; ++i) {
        (void)pool.submit([&] {
            std::this_thread::sleep_for(100us);
            ++executed;
        });
    }
    pool.shutdown();
    EXPECT_EQ(executed.load(), 50); // nothing queued was dropped
    EXPECT_EQ(pool.tasksCompleted(), 50u);
    EXPECT_EQ(pool.tasksSubmitted(), 50u);
    EXPECT_THROW((void)pool.submit([] {}), ThreadPoolError);
    pool.shutdown(); // idempotent
}

TEST(ThreadPoolTest, DestructorJoinsAllWorkers) {
    std::atomic<int> executed{0};
    {
        ThreadPool pool(4);
        for (int i = 0; i < 200; ++i) {
            (void)pool.submit([&] { ++executed; });
        }
    } // destructor: finish the queue, join threads
    EXPECT_EQ(executed.load(), 200);
}

TEST(ThreadPoolTest, ConcurrentSubmittersAreSafe) {
    ThreadPool pool(4);
    std::atomic<int> sum{0};
    std::vector<std::thread> submitters;
    std::mutex futuresMutex;
    std::vector<std::future<void>> futures;
    for (int t = 0; t < 4; ++t) {
        submitters.emplace_back([&] {
            for (int i = 0; i < 250; ++i) {
                auto f = pool.submit([&] { sum.fetch_add(1); });
                const std::lock_guard lock(futuresMutex);
                futures.push_back(std::move(f));
            }
        });
    }
    for (auto& t : submitters) {
        t.join();
    }
    for (auto& f : futures) {
        f.get();
    }
    EXPECT_EQ(sum.load(), 1000);
}
