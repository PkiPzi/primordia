#include "core/ThreadPool.hpp"

#include <algorithm>
#include <chrono>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define PM_CPU_RELAX() _mm_pause()
#else
#define PM_CPU_RELAX() std::this_thread::yield()
#endif

namespace pm {

ThreadPool::ThreadPool(unsigned threads) {
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned i = 1; i < threads; ++i) workers_.emplace_back([this, i] { workerLoop(i); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        quit_.store(true);
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
}

void ThreadPool::drain(unsigned threadIndex) {
    for (;;) {
        int c = nextChunk_.fetch_add(1, std::memory_order_relaxed);
        if (c >= chunkCount_) break;
        int b = begin_ + c * chunkSize_;
        int e = std::min(end_, b + chunkSize_);
        (*job_)(b, e, int(threadIndex));
    }
}

void ThreadPool::run(int begin, int end, int minChunk, const std::function<void(int, int, int)>& job) {
    const int n = end - begin;
    const int threads = int(size());
    // ~4 chunks per thread balances uneven per-item cost (dense herds vs. empty plains).
    chunkSize_ = std::max(minChunk, (n + threads * 4 - 1) / (threads * 4));
    chunkCount_ = (n + chunkSize_ - 1) / chunkSize_;
    begin_ = begin;
    end_ = end;
    job_ = &job;
    nextChunk_.store(0, std::memory_order_relaxed);
    pending_.store(int(workers_.size()), std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(mutex_);
    }
    cv_.notify_all();

    drain(0);
    while (pending_.load(std::memory_order_acquire) != 0) PM_CPU_RELAX();
    job_ = nullptr;
}

void ThreadPool::workerLoop(unsigned index) {
    uint64_t seen = 0;
    for (;;) {
        int spins = 0;
        while (generation_.load(std::memory_order_acquire) == seen) {
            if (quit_.load(std::memory_order_relaxed)) return;
            if (++spins < 4000) {
                PM_CPU_RELAX();
                continue;
            }
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&] { return quit_.load() || generation_.load(std::memory_order_acquire) != seen; });
            if (quit_.load()) return;
        }
        seen = generation_.load(std::memory_order_acquire);
        drain(index);
        pending_.fetch_sub(1, std::memory_order_acq_rel);
    }
}

} // namespace pm
