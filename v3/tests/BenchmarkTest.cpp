#include "../include/MemoryPool.h"
#include "../include/MemoryPoolStats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Avery_memoryPool;

namespace
{
using Clock = std::chrono::steady_clock;

std::atomic<unsigned long long> g_sink{0};

struct Result
{
    double poolMs;
    double mallocMs;
    double newMs;
};

struct TimingStats
{
    double avgMs = 0.0;
    double minMs = 0.0;
    double maxMs = 0.0;
    double stddevMs = 0.0;
};

unsigned long long touchMemory(void* ptr, size_t size, unsigned char value)
{
    if (!ptr || size == 0)
    {
        return 0;
    }

    size_t bytesToWrite = std::min<size_t>(size, 16);
    std::memset(ptr, value, bytesToWrite);
    return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(ptr) & 0xffu);
}

template <typename Func>
double averageTimeMs(Func func, int rounds)
{
    double totalMs = 0.0;
    for (int i = 0; i < rounds; ++i)
    {
        auto begin = Clock::now();
        func();
        auto end = Clock::now();
        totalMs += std::chrono::duration<double, std::milli>(end - begin).count();
    }
    return totalMs / rounds;
}

void warmUp(size_t size)
{
    constexpr size_t warmupIterations = 5000;
    constexpr size_t chunkSize = 200;
    std::vector<void*> ptrs;
    ptrs.reserve(chunkSize);

    for (size_t base = 0; base < warmupIterations; base += chunkSize)
    {
        ptrs.clear();
        size_t currentChunk = std::min(chunkSize, warmupIterations - base);
        for (size_t i = 0; i < currentChunk; ++i)
        {
            void* ptr = MemoryPool::allocate(size);
            g_sink.fetch_add(touchMemory(ptr, size, 0x11), std::memory_order_relaxed);
            ptrs.push_back(ptr);
        }
        for (void* ptr : ptrs)
        {
            MemoryPool::deallocate(ptr, size);
        }
    }

    for (size_t i = 0; i < warmupIterations; ++i)
    {
        void* ptr = std::malloc(size);
        g_sink.fetch_add(touchMemory(ptr, size, 0x22), std::memory_order_relaxed);
        std::free(ptr);
    }

    for (size_t i = 0; i < warmupIterations; ++i)
    {
        void* ptr = ::operator new(size);
        g_sink.fetch_add(touchMemory(ptr, size, 0x33), std::memory_order_relaxed);
        ::operator delete(ptr);
    }
}

double runPoolImmediate(size_t size, size_t iterations)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        for (size_t i = 0; i < iterations; ++i)
        {
            void* ptr = MemoryPool::allocate(size);
            localSink += touchMemory(ptr, size, static_cast<unsigned char>(i));
            MemoryPool::deallocate(ptr, size);
        }
        g_sink.fetch_add(localSink, std::memory_order_relaxed);
    }, 5);
}

double runMallocImmediate(size_t size, size_t iterations)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        for (size_t i = 0; i < iterations; ++i)
        {
            void* ptr = std::malloc(size);
            localSink += touchMemory(ptr, size, static_cast<unsigned char>(i));
            std::free(ptr);
        }
        g_sink.fetch_add(localSink, std::memory_order_relaxed);
    }, 5);
}

double runNewImmediate(size_t size, size_t iterations)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        for (size_t i = 0; i < iterations; ++i)
        {
            void* ptr = ::operator new(size);
            localSink += touchMemory(ptr, size, static_cast<unsigned char>(i));
            ::operator delete(ptr);
        }
        g_sink.fetch_add(localSink, std::memory_order_relaxed);
    }, 5);
}

double runPoolBatch(size_t size, size_t iterations)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        std::vector<void*> ptrs;
        constexpr size_t chunkSize = 200;
        ptrs.reserve(chunkSize);
        for (size_t base = 0; base < iterations; base += chunkSize)
        {
            ptrs.clear();
            size_t currentChunk = std::min(chunkSize, iterations - base);
            for (size_t i = 0; i < currentChunk; ++i)
            {
                void* ptr = MemoryPool::allocate(size);
                localSink += touchMemory(ptr, size, static_cast<unsigned char>(base + i));
                ptrs.push_back(ptr);
            }
            for (void* ptr : ptrs)
            {
                MemoryPool::deallocate(ptr, size);
            }
        }
        g_sink.fetch_add(localSink, std::memory_order_relaxed);
    }, 5);
}

double runMallocBatch(size_t size, size_t iterations)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        std::vector<void*> ptrs;
        constexpr size_t chunkSize = 200;
        ptrs.reserve(chunkSize);
        for (size_t base = 0; base < iterations; base += chunkSize)
        {
            ptrs.clear();
            size_t currentChunk = std::min(chunkSize, iterations - base);
            for (size_t i = 0; i < currentChunk; ++i)
            {
                void* ptr = std::malloc(size);
                localSink += touchMemory(ptr, size, static_cast<unsigned char>(base + i));
                ptrs.push_back(ptr);
            }
            for (void* ptr : ptrs)
            {
                std::free(ptr);
            }
        }
        g_sink.fetch_add(localSink, std::memory_order_relaxed);
    }, 5);
}

double runNewBatch(size_t size, size_t iterations)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        std::vector<void*> ptrs;
        constexpr size_t chunkSize = 200;
        ptrs.reserve(chunkSize);
        for (size_t base = 0; base < iterations; base += chunkSize)
        {
            ptrs.clear();
            size_t currentChunk = std::min(chunkSize, iterations - base);
            for (size_t i = 0; i < currentChunk; ++i)
            {
                void* ptr = ::operator new(size);
                localSink += touchMemory(ptr, size, static_cast<unsigned char>(base + i));
                ptrs.push_back(ptr);
            }
            for (void* ptr : ptrs)
            {
                ::operator delete(ptr);
            }
        }
        g_sink.fetch_add(localSink, std::memory_order_relaxed);
    }, 5);
}

double runPoolReuse(size_t size, size_t objectsPerRound, size_t reuseRounds)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        std::vector<void*> ptrs;
        ptrs.reserve(objectsPerRound);
        for (size_t round = 0; round < reuseRounds; ++round)
        {
            ptrs.clear();
            for (size_t i = 0; i < objectsPerRound; ++i)
            {
                void* ptr = MemoryPool::allocate(size);
                localSink += touchMemory(ptr, size, static_cast<unsigned char>(round + i));
                ptrs.push_back(ptr);
            }
            for (void* ptr : ptrs)
            {
                MemoryPool::deallocate(ptr, size);
            }
        }
        g_sink.fetch_add(localSink, std::memory_order_relaxed);
    }, 5);
}

double runMallocReuse(size_t size, size_t objectsPerRound, size_t reuseRounds)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        std::vector<void*> ptrs;
        ptrs.reserve(objectsPerRound);
        for (size_t round = 0; round < reuseRounds; ++round)
        {
            ptrs.clear();
            for (size_t i = 0; i < objectsPerRound; ++i)
            {
                void* ptr = std::malloc(size);
                localSink += touchMemory(ptr, size, static_cast<unsigned char>(round + i));
                ptrs.push_back(ptr);
            }
            for (void* ptr : ptrs)
            {
                std::free(ptr);
            }
        }
        g_sink.fetch_add(localSink, std::memory_order_relaxed);
    }, 5);
}

double runNewReuse(size_t size, size_t objectsPerRound, size_t reuseRounds)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        std::vector<void*> ptrs;
        ptrs.reserve(objectsPerRound);
        for (size_t round = 0; round < reuseRounds; ++round)
        {
            ptrs.clear();
            for (size_t i = 0; i < objectsPerRound; ++i)
            {
                void* ptr = ::operator new(size);
                localSink += touchMemory(ptr, size, static_cast<unsigned char>(round + i));
                ptrs.push_back(ptr);
            }
            for (void* ptr : ptrs)
            {
                ::operator delete(ptr);
            }
        }
        g_sink.fetch_add(localSink, std::memory_order_relaxed);
    }, 5);
}

std::vector<size_t> makeMixedSizes(size_t iterations)
{
    const size_t candidates[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    std::mt19937 rng(20260527);
    std::uniform_int_distribution<size_t> dist(0, sizeof(candidates) / sizeof(candidates[0]) - 1);
    std::vector<size_t> sizes;
    sizes.reserve(iterations);
    for (size_t i = 0; i < iterations; ++i)
    {
        sizes.push_back(candidates[dist(rng)]);
    }
    return sizes;
}

double runPoolMixed(const std::vector<size_t>& sizes)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        for (size_t i = 0; i < sizes.size(); ++i)
        {
            size_t size = sizes[i];
            void* ptr = MemoryPool::allocate(size);
            localSink += touchMemory(ptr, size, static_cast<unsigned char>(i));
            MemoryPool::deallocate(ptr, size);
        }
        g_sink.fetch_add(localSink, std::memory_order_relaxed);
    }, 5);
}

double runMallocMixed(const std::vector<size_t>& sizes)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        for (size_t i = 0; i < sizes.size(); ++i)
        {
            size_t size = sizes[i];
            void* ptr = std::malloc(size);
            localSink += touchMemory(ptr, size, static_cast<unsigned char>(i));
            std::free(ptr);
        }
        g_sink.fetch_add(localSink, std::memory_order_relaxed);
    }, 5);
}

double runNewMixed(const std::vector<size_t>& sizes)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        for (size_t i = 0; i < sizes.size(); ++i)
        {
            size_t size = sizes[i];
            void* ptr = ::operator new(size);
            localSink += touchMemory(ptr, size, static_cast<unsigned char>(i));
            ::operator delete(ptr);
        }
        g_sink.fetch_add(localSink, std::memory_order_relaxed);
    }, 5);
}

template <typename Alloc, typename Free>
double runMultiThread(size_t size, size_t iterationsPerThread, size_t threadCount, Alloc alloc, Free freeFn)
{
    return averageTimeMs([&]() {
        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        for (size_t t = 0; t < threadCount; ++t)
        {
            threads.emplace_back([=]() {
                unsigned long long localSink = 0;
                for (size_t i = 0; i < iterationsPerThread; ++i)
                {
                    void* ptr = alloc(size);
                    localSink += touchMemory(ptr, size, static_cast<unsigned char>(i + t));
                    freeFn(ptr, size);
                }
                g_sink.fetch_add(localSink, std::memory_order_relaxed);
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
    }, 3);
}

template <typename Alloc, typename Free>
double runMultiThreadMixed(const std::vector<size_t>& sizes, size_t threadCount, Alloc alloc, Free freeFn)
{
    return averageTimeMs([&]() {
        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        size_t perThread = sizes.size() / threadCount;
        for (size_t t = 0; t < threadCount; ++t)
        {
            threads.emplace_back([&, t]() {
                unsigned long long localSink = 0;
                size_t begin = t * perThread;
                size_t end = (t == threadCount - 1) ? sizes.size() : begin + perThread;
                for (size_t i = begin; i < end; ++i)
                {
                    size_t size = sizes[i];
                    void* ptr = alloc(size);
                    localSink += touchMemory(ptr, size, static_cast<unsigned char>(i + t));
                    freeFn(ptr, size);
                }
                g_sink.fetch_add(localSink, std::memory_order_relaxed);
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
    }, 3);
}

void printHeader()
{
    std::cout << std::left
              << std::setw(30) << "test"
              << std::setw(10) << "size"
              << std::setw(12) << "iters"
              << std::setw(10) << "threads"
              << std::setw(14) << "pool_ms"
              << std::setw(14) << "malloc_ms"
              << std::setw(14) << "new_ms"
              << std::setw(16) << "malloc/pool"
              << std::setw(12) << "new/pool"
              << std::endl;
}

void printResult(const std::string& name, const std::string& sizeLabel,
    size_t iterations, size_t threads, const Result& result)
{
    double mallocRatio = result.poolMs > 0.0 ? result.mallocMs / result.poolMs : 0.0;
    double newRatio = result.poolMs > 0.0 ? result.newMs / result.poolMs : 0.0;
    std::cout << std::left
              << std::setw(30) << name
              << std::setw(10) << sizeLabel
              << std::setw(12) << iterations
              << std::setw(10) << threads
              << std::setw(14) << result.poolMs
              << std::setw(14) << result.mallocMs
              << std::setw(14) << result.newMs
              << std::setw(16) << mallocRatio
              << std::setw(12) << newRatio
              << std::endl;
}

Result immediateResult(size_t size, size_t iterations)
{
    warmUp(size);
    return {
        runPoolImmediate(size, iterations),
        runMallocImmediate(size, iterations),
        runNewImmediate(size, iterations)
    };
}

Result batchResult(size_t size, size_t iterations)
{
    warmUp(size);
    return {
        runPoolBatch(size, iterations),
        runMallocBatch(size, iterations),
        runNewBatch(size, iterations)
    };
}

TimingStats summarizeTimings(const std::vector<double>& timings)
{
    TimingStats stats;
    if (timings.empty())
    {
        return stats;
    }

    stats.minMs = timings.front();
    stats.maxMs = timings.front();
    double totalMs = 0.0;
    for (double value : timings)
    {
        totalMs += value;
        stats.minMs = std::min(stats.minMs, value);
        stats.maxMs = std::max(stats.maxMs, value);
    }
    stats.avgMs = totalMs / timings.size();

    double variance = 0.0;
    for (double value : timings)
    {
        double delta = value - stats.avgMs;
        variance += delta * delta;
    }
    stats.stddevMs = std::sqrt(variance / timings.size());
    return stats;
}

void printStatsHeader(const std::string& mode, const std::string& name,
    const std::vector<double>& timings)
{
    TimingStats timing = summarizeTimings(timings);
    std::cout << "\n[stats][" << mode << "] " << name
              << " rounds=" << timings.size()
              << " avg_ms=" << timing.avgMs
              << " min_ms=" << timing.minMs
              << " max_ms=" << timing.maxMs
              << " stddev_ms=" << timing.stddevMs
              << std::endl;
}

void runPoolBatchWorkload(size_t size, size_t iterations)
{
    unsigned long long localSink = 0;
    std::vector<void*> ptrs;
    constexpr size_t chunkSize = 200;
    ptrs.reserve(chunkSize);
    for (size_t base = 0; base < iterations; base += chunkSize)
    {
        ptrs.clear();
        size_t currentChunk = std::min(chunkSize, iterations - base);
        for (size_t i = 0; i < currentChunk; ++i)
        {
            void* ptr = MemoryPool::allocate(size);
            localSink += touchMemory(ptr, size, static_cast<unsigned char>(base + i));
            ptrs.push_back(ptr);
        }
        for (void* ptr : ptrs)
        {
            MemoryPool::deallocate(ptr, size);
        }
    }
    g_sink.fetch_add(localSink, std::memory_order_relaxed);
}

void runPoolReuseWorkload(size_t size, size_t objectsPerRound, size_t reuseRounds)
{
    unsigned long long localSink = 0;
    std::vector<void*> ptrs;
    ptrs.reserve(objectsPerRound);
    for (size_t round = 0; round < reuseRounds; ++round)
    {
        ptrs.clear();
        for (size_t i = 0; i < objectsPerRound; ++i)
        {
            void* ptr = MemoryPool::allocate(size);
            localSink += touchMemory(ptr, size, static_cast<unsigned char>(round + i));
            ptrs.push_back(ptr);
        }
        for (void* ptr : ptrs)
        {
            MemoryPool::deallocate(ptr, size);
        }
    }
    g_sink.fetch_add(localSink, std::memory_order_relaxed);
}

void runPoolMixedWorkload(const std::vector<size_t>& sizes)
{
    unsigned long long localSink = 0;
    for (size_t i = 0; i < sizes.size(); ++i)
    {
        size_t size = sizes[i];
        void* ptr = MemoryPool::allocate(size);
        localSink += touchMemory(ptr, size, static_cast<unsigned char>(i));
        MemoryPool::deallocate(ptr, size);
    }
    g_sink.fetch_add(localSink, std::memory_order_relaxed);
}

void runStatsExperiment()
{
#ifdef ENABLE_MEMORY_POOL_STATS
    constexpr int statsRounds = 3;

    auto runColdScenario = [](const std::string& name, auto&& func) {
        std::vector<double> timings;
        timings.reserve(statsRounds);
        MemoryPoolStats::reset();
        MemoryPoolStats::setEnabled(true);

        for (int i = 0; i < statsRounds; ++i)
        {
            double elapsedMs = 0.0;
            std::thread worker([&]() {
                auto begin = Clock::now();
                func();
                auto end = Clock::now();
                elapsedMs = std::chrono::duration<double, std::milli>(end - begin).count();
            });
            worker.join();
            timings.push_back(elapsedMs);
        }

        MemoryPoolStats::setEnabled(false);
        printStatsHeader("cold", name, timings);
        MemoryPoolStats::print(std::cout);
    };

    auto runWarmScenario = [](const std::string& name, auto&& warmFunc, auto&& func) {
        std::vector<double> timings;
        timings.reserve(statsRounds);

        std::thread worker([&]() {
            MemoryPoolStats::setEnabled(false);
            warmFunc();

            MemoryPoolStats::reset();
            MemoryPoolStats::setEnabled(true);
            for (int i = 0; i < statsRounds; ++i)
            {
                auto begin = Clock::now();
                func();
                auto end = Clock::now();
                timings.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
            }
            MemoryPoolStats::setEnabled(false);
        });
        worker.join();

        printStatsHeader("warm", name, timings);
        MemoryPoolStats::print(std::cout);
    };

    std::cout << "\nallocator stats cold/warm experiment" << std::endl;
    std::cout << "Cold mode uses a new worker thread per measured round to isolate thread_local ThreadCache." << std::endl;
    std::cout << "CentralCache and PageCache are process-global and are not fully reset between cases." << std::endl;
    std::cout << "Warm mode performs warm-up and measured rounds in the same worker thread." << std::endl;

    const std::vector<size_t> mixedSizes = makeMixedSizes(200000);
    const std::vector<size_t> stressSizes = makeMixedSizes(500000);

    runColdScenario("fixed 64B repeated reuse", []() {
        runPoolReuseWorkload(64, 200, 2000);
    });
    runWarmScenario("fixed 64B repeated reuse",
        []() { runPoolReuseWorkload(64, 200, 200); },
        []() { runPoolReuseWorkload(64, 200, 2000); });

    runColdScenario("mixed small sizes", [&]() {
        runPoolMixedWorkload(mixedSizes);
    });
    runWarmScenario("mixed small sizes",
        [&]() { runPoolMixedWorkload(mixedSizes); },
        [&]() { runPoolMixedWorkload(mixedSizes); });

    runColdScenario("refill pressure 4096B", []() {
        runPoolBatchWorkload(4096, 60000);
    });
    runWarmScenario("refill pressure 4096B",
        []() { runPoolBatchWorkload(4096, 60000); },
        []() { runPoolBatchWorkload(4096, 60000); });

    runColdScenario("cold span allocation 8192B", []() {
        runPoolBatchWorkload(8192, 4000);
    });
    runWarmScenario("warm span allocation 8192B",
        []() { runPoolBatchWorkload(8192, 4000); },
        []() { runPoolBatchWorkload(8192, 4000); });

    runColdScenario("long stress mixed", [&]() {
        runPoolMixedWorkload(stressSizes);
    });
    runWarmScenario("long stress mixed",
        [&]() { runPoolMixedWorkload(stressSizes); },
        [&]() { runPoolMixedWorkload(stressSizes); });
#else
    std::cout << "\nallocator stats cold/warm experiment skipped: rebuild with ENABLE_MEMORY_POOL_STATS."
              << std::endl;
#endif
}

} // namespace

int main()
{
    std::cout << std::fixed << std::setprecision(3);
    runStatsExperiment();

    std::cout << "v3 memory pool benchmark (Release recommended)" << std::endl;
    std::cout << "A ratio above 1.0 means the memory pool is faster than that baseline." << std::endl;
    printHeader();

    const size_t fixedIterations = 200000;
    const size_t fixedSizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 4096};
    for (size_t size : fixedSizes)
    {
        printResult("fixed immediate", std::to_string(size), fixedIterations, 1,
            immediateResult(size, fixedIterations));
    }

    const size_t batchIterations = 80000;
    for (size_t size : {64ul, 256ul, 1024ul, 4096ul})
    {
        printResult("batch alloc/free", std::to_string(size), batchIterations, 1,
            batchResult(size, batchIterations));
    }

    const size_t objectsPerRound = 200;
    const size_t reuseRounds = 2000;
    warmUp(64);
    Result reuseResult{
        runPoolReuse(64, objectsPerRound, reuseRounds),
        runMallocReuse(64, objectsPerRound, reuseRounds),
        runNewReuse(64, objectsPerRound, reuseRounds)
    };
    printResult("repeated reuse", "64", objectsPerRound * reuseRounds, 1, reuseResult);

    const size_t mtIterations = 50000;
    for (size_t threads : {2ul, 4ul, 8ul})
    {
        warmUp(64);
        Result result{
            runMultiThread(64, mtIterations, threads,
                [](size_t s) { return MemoryPool::allocate(s); },
                [](void* p, size_t s) { MemoryPool::deallocate(p, s); }),
            runMultiThread(64, mtIterations, threads,
                [](size_t s) { return std::malloc(s); },
                [](void* p, size_t) { std::free(p); }),
            runMultiThread(64, mtIterations, threads,
                [](size_t s) { return ::operator new(s); },
                [](void* p, size_t) { ::operator delete(p); })
        };
        printResult("multi same class", "64", mtIterations, threads, result);
    }

    const size_t mixedIterations = 200000;
    std::vector<size_t> mixedSizes = makeMixedSizes(mixedIterations);
    warmUp(128);
    Result mixedResult{
        runPoolMixed(mixedSizes),
        runMallocMixed(mixedSizes),
        runNewMixed(mixedSizes)
    };
    printResult("mixed small sizes", "mixed", mixedIterations, 1, mixedResult);

    for (size_t threads : {2ul, 4ul, 8ul})
    {
        warmUp(128);
        Result result{
            runMultiThreadMixed(mixedSizes, threads,
                [](size_t s) { return MemoryPool::allocate(s); },
                [](void* p, size_t s) { MemoryPool::deallocate(p, s); }),
            runMultiThreadMixed(mixedSizes, threads,
                [](size_t s) { return std::malloc(s); },
                [](void* p, size_t) { std::free(p); }),
            runMultiThreadMixed(mixedSizes, threads,
                [](size_t s) { return ::operator new(s); },
                [](void* p, size_t) { ::operator delete(p); })
        };
        printResult("multi mixed sizes", "mixed", mixedIterations / threads, threads, result);
    }

    const size_t refillIterations = 60000;
    printResult("refill pressure", "4096", refillIterations, 1,
        batchResult(4096, refillIterations));

    const size_t largeSize = MAX_BYTES * 2;
    const size_t largeIterations = 20000;
    printResult("large bypass", std::to_string(largeSize), largeIterations, 1,
        immediateResult(largeSize, largeIterations));

    const size_t stressIterations = 500000;
    std::vector<size_t> stressSizes = makeMixedSizes(stressIterations);
    warmUp(256);
    Result stressResult{
        runPoolMixed(stressSizes),
        runMallocMixed(stressSizes),
        runNewMixed(stressSizes)
    };
    printResult("long stress mixed", "mixed", stressIterations, 1, stressResult);

    std::cout << "sink=" << g_sink.load(std::memory_order_relaxed) << std::endl;
    return 0;
}
