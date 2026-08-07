// src/core/thread_pool.hpp — bounded thread pool for async task execution.
#pragma once
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace progressive::desktop {

class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads = std::thread::hardware_concurrency());
    ~ThreadPool();

    static ThreadPool& instance() {
        static ThreadPool pool{4};
        return pool;
    }

    void enqueue(std::function<void()> task);
    size_t pending() const;

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
};

} // namespace progressive::desktop
