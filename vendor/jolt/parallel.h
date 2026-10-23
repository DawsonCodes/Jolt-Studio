#pragma once

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace jolt {

inline int all_cores() { return int(std::max(1u, std::thread::hardware_concurrency())); }

// ffmpeg threads its own decoding, a third as many decoders as cores was fastest
inline int default_jobs() { return std::max(1, all_cores() / 3); }

// first exception gets rethrown here, indexes not started yet are skipped
template <typename Work>
void for_each_index(int count, int threads, Work work) {
    std::atomic<int> next{0};
    std::mutex guard;
    std::exception_ptr failure;
    std::vector<std::thread> pool;
    for (int thread = 0; thread < std::min(count, threads); ++thread) {
        pool.emplace_back([&] {
            try {
                for (int index; (index = next++) < count;) work(index);
            } catch (...) {
                next = count;
                const std::lock_guard<std::mutex> lock(guard);
                if (!failure) failure = std::current_exception();
            }
        });
    }
    for (std::thread& thread : pool) thread.join();
    if (failure) std::rethrow_exception(failure);
}

}  // namespace jolt
