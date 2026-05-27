#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../include/MemoryPool.h"

using namespace Avery_memoryPool;

namespace
{
std::atomic<unsigned long long> g_sink{0};

struct Result
{
    double poolMs;
    double mallocMs;
    double newMs;
};

unsigned long long touchMemory(void* ptr, size_t size, unsigned char value)
{
    if (!ptr || size == 0)
    {
        return 0;
    }
    std::memset(ptr, value, size);
    return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(ptr) & 0xffu);
}

template <typename Func>
double averageTimeMs(Func func, int rounds)
{
    using Clock = std::chrono::steady_clock;
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
    const size_t warmupIterations = 10000;
    std::vector<void*> ptrs;
    ptrs.reserve(warmupIterations);

    for (size_t i = 0; i < warmupIterations; ++i)
    {
        void* ptr = HashBucket::useMemory(size);
        g_sink.fetch_add(touchMemory(ptr, size, 0x11), std::memory_order_relaxed);
        ptrs.push_back(ptr);
    }
    for (void* ptr : ptrs)
    {
        HashBucket::freeMemory(ptr, size);
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
            void* ptr = HashBucket::useMemory(size);
            localSink += touchMemory(ptr, size, static_cast<unsigned char>(i));
            HashBucket::freeMemory(ptr, size);
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
        ptrs.reserve(iterations);
        for (size_t i = 0; i < iterations; ++i)
        {
            void* ptr = HashBucket::useMemory(size);
            localSink += touchMemory(ptr, size, static_cast<unsigned char>(i));
            ptrs.push_back(ptr);
        }
        for (void* ptr : ptrs)
        {
            HashBucket::freeMemory(ptr, size);
        }
        g_sink.fetch_add(localSink, std::memory_order_relaxed);
    }, 5);
}

double runMallocBatch(size_t size, size_t iterations)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        std::vector<void*> ptrs;
        ptrs.reserve(iterations);
        for (size_t i = 0; i < iterations; ++i)
        {
            void* ptr = std::malloc(size);
            localSink += touchMemory(ptr, size, static_cast<unsigned char>(i));
            ptrs.push_back(ptr);
        }
        for (void* ptr : ptrs)
        {
            std::free(ptr);
        }
        g_sink.fetch_add(localSink, std::memory_order_relaxed);
    }, 5);
}

double runNewBatch(size_t size, size_t iterations)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        std::vector<void*> ptrs;
        ptrs.reserve(iterations);
        for (size_t i = 0; i < iterations; ++i)
        {
            void* ptr = ::operator new(size);
            localSink += touchMemory(ptr, size, static_cast<unsigned char>(i));
            ptrs.push_back(ptr);
        }
        for (void* ptr : ptrs)
        {
            ::operator delete(ptr);
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
                void* ptr = HashBucket::useMemory(size);
                localSink += touchMemory(ptr, size, static_cast<unsigned char>(round + i));
                ptrs.push_back(ptr);
            }
            for (void* ptr : ptrs)
            {
                HashBucket::freeMemory(ptr, size);
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

double runPoolMixed(const std::vector<size_t>& sizes)
{
    return averageTimeMs([&]() {
        unsigned long long localSink = 0;
        for (size_t i = 0; i < sizes.size(); ++i)
        {
            size_t size = sizes[i];
            void* ptr = HashBucket::useMemory(size);
            localSink += touchMemory(ptr, size, static_cast<unsigned char>(i));
            HashBucket::freeMemory(ptr, size);
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

size_t threadLocalSize(size_t threadIndex)
{
    const size_t sizes[] = {32, 64, 96, 128, 160, 192, 224, 256};
    return sizes[threadIndex % (sizeof(sizes) / sizeof(sizes[0]))];
}

double runPoolMultiThread(size_t size, size_t iterationsPerThread, size_t threadCount)
{
    return averageTimeMs([&]() {
        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        for (size_t t = 0; t < threadCount; ++t)
        {
            threads.emplace_back([=]() {
                size_t localSize = (size == 0) ? threadLocalSize(t) : size;
                unsigned long long localSink = 0;
                for (size_t i = 0; i < iterationsPerThread; ++i)
                {
                    void* ptr = HashBucket::useMemory(localSize);
                    localSink += touchMemory(ptr, localSize, static_cast<unsigned char>(i + t));
                    HashBucket::freeMemory(ptr, localSize);
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

double runMallocMultiThread(size_t size, size_t iterationsPerThread, size_t threadCount)
{
    return averageTimeMs([&]() {
        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        for (size_t t = 0; t < threadCount; ++t)
        {
            threads.emplace_back([=]() {
                size_t localSize = (size == 0) ? threadLocalSize(t) : size;
                unsigned long long localSink = 0;
                for (size_t i = 0; i < iterationsPerThread; ++i)
                {
                    void* ptr = std::malloc(localSize);
                    localSink += touchMemory(ptr, localSize, static_cast<unsigned char>(i + t));
                    std::free(ptr);
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

double runNewMultiThread(size_t size, size_t iterationsPerThread, size_t threadCount)
{
    return averageTimeMs([&]() {
        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        for (size_t t = 0; t < threadCount; ++t)
        {
            threads.emplace_back([=]() {
                size_t localSize = (size == 0) ? threadLocalSize(t) : size;
                unsigned long long localSink = 0;
                for (size_t i = 0; i < iterationsPerThread; ++i)
                {
                    void* ptr = ::operator new(localSize);
                    localSink += touchMemory(ptr, localSize, static_cast<unsigned char>(i + t));
                    ::operator delete(ptr);
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

void printResult(const std::string& name, size_t size, size_t iterations,
    size_t threads, const Result& result)
{
    double mallocRatio = result.poolMs > 0.0 ? result.mallocMs / result.poolMs : 0.0;
    double newRatio = result.poolMs > 0.0 ? result.newMs / result.poolMs : 0.0;
    std::string sizeLabel = (size == 0) ? "mixed" : std::to_string(size);

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

std::vector<size_t> makeMixedSizes(size_t iterations)
{
    const size_t candidates[] = {8, 16, 32, 64, 128, 256, 512};
    std::mt19937 rng(20260526);
    std::uniform_int_distribution<size_t> dist(0, sizeof(candidates) / sizeof(candidates[0]) - 1);
    std::vector<size_t> sizes;
    sizes.reserve(iterations);
    for (size_t i = 0; i < iterations; ++i)
    {
        sizes.push_back(candidates[dist(rng)]);
    }
    return sizes;
}

} // namespace

int main()
{
    HashBucket::initMemoryPool();

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "v1 memory pool benchmark (average over multiple rounds)" << std::endl;
    std::cout << "A ratio above 1.0 means the memory pool is faster than that baseline." << std::endl;
    printHeader();

    const size_t fixedIterations = 500000;
    const size_t fixedSizes[] = {32, 64, 128};
    for (size_t size : fixedSizes)
    {
        warmUp(size);
        Result result{
            runPoolImmediate(size, fixedIterations),
            runMallocImmediate(size, fixedIterations),
            runNewImmediate(size, fixedIterations)
        };
        printResult("fixed immediate", size, fixedIterations, 1, result);
    }

    const size_t batchIterations = 200000;
    for (size_t size : fixedSizes)
    {
        warmUp(size);
        Result result{
            runPoolBatch(size, batchIterations),
            runMallocBatch(size, batchIterations),
            runNewBatch(size, batchIterations)
        };
        printResult("batch alloc/free", size, batchIterations, 1, result);
    }

    const size_t objectsPerRound = 20000;
    const size_t reuseRounds = 50;
    warmUp(64);
    Result reuseResult{
        runPoolReuse(64, objectsPerRound, reuseRounds),
        runMallocReuse(64, objectsPerRound, reuseRounds),
        runNewReuse(64, objectsPerRound, reuseRounds)
    };
    printResult("repeated reuse", 64, objectsPerRound * reuseRounds, 1, reuseResult);

    const size_t mtIterations = 20000;
    const size_t threadCounts[] = {2, 4, 8};
    for (size_t threads : threadCounts)
    {
        for (size_t t = 0; t < threads; ++t)
        {
            warmUp(threadLocalSize(t));
        }
        Result result{
            runPoolMultiThread(0, mtIterations, threads),
            runMallocMultiThread(0, mtIterations, threads),
            runNewMultiThread(0, mtIterations, threads)
        };
        printResult("multi-thread small", 0, mtIterations, threads, result);
    }

    const size_t mixedIterations = 300000;
    std::vector<size_t> mixedSizes = makeMixedSizes(mixedIterations);
    warmUp(64);
    Result mixedResult{
        runPoolMixed(mixedSizes),
        runMallocMixed(mixedSizes),
        runNewMixed(mixedSizes)
    };
    printResult("mixed small sizes", 0, mixedIterations, 1, mixedResult);

    const size_t largeSize = MAX_SLOT_SIZE * 2;
    const size_t largeIterations = 200000;
    warmUp(largeSize);
    Result largeResult{
        runPoolImmediate(largeSize, largeIterations),
        runMallocImmediate(largeSize, largeIterations),
        runNewImmediate(largeSize, largeIterations)
    };
    printResult("large bypass", largeSize, largeIterations, 1, largeResult);

    std::cout << "sink=" << g_sink.load(std::memory_order_relaxed) << std::endl;
    return 0;
}
