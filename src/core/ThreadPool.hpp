#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace pm {

// Fork-join pool tuned for many short parallel loops per frame. Workers spin
// briefly between jobs (fast-forward issues thousands of loops per second)
// and fall back to sleeping on a condition variable when idle.
class ThreadPool {
public:
    explicit ThreadPool(unsigned threads = 0); // 0 = hardware concurrency
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    unsigned size() const { return unsigned(workers_.size()) + 1; } // workers + caller

    // Calls fn(begin, end, threadIndex) over chunks of [begin, end).
    template <class F>
    void parallelFor(int begin, int end, int minChunk, F&& fn) {
        const int n = end - begin;
        if (n <= 0) return;
        if (workers_.empty() || n <= minChunk) {
            fn(begin, end, 0);
            return;
        }
        std::function<void(int, int, int)> job = std::forward<F>(fn);
        run(begin, end, minChunk, job);
    }

private:
    void run(int begin, int end, int minChunk, const std::function<void(int, int, int)>& job);
    void workerLoop(unsigned index);
    void drain(unsigned threadIndex);

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<uint64_t> generation_{0};
    std::atomic<bool> quit_{false};
    std::atomic<int> pending_{0};

    const std::function<void(int, int, int)>* job_ = nullptr;
    std::atomic<int> nextChunk_{0};
    int chunkCount_ = 0, chunkSize_ = 1, begin_ = 0, end_ = 0;
};

} // namespace pm
