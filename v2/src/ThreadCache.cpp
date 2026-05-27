#include <cstdlib>
#include "../include/ThreadCache.h"
#include "../include/CentralCache.h"

namespace Avery_memoryPool
{

void* ThreadCache::allocate(size_t size)
{
    if (size == 0)
    {
        size = ALIGNMENT;
    }

    if (size > MAX_BYTES)
    {
        return malloc(size);
    }

    size_t index = SizeClass::getIndex(size);

    if (void* ptr = freeList_[index])
    {
        freeList_[index] = *reinterpret_cast<void**>(ptr);
        // freeListSize_[index] tracks real free blocks kept in this
        // thread-local free list. Decrement only after a successful pop
        // so an empty list cannot trigger size_t underflow.
        freeListSize_[index]--;
        return ptr;
    }

    return fetchFromCentralCache(index);
}

void ThreadCache::deallocate(void* ptr, size_t size)
{
    if (size > MAX_BYTES)
    {
        free(ptr);
        return;
    }

    size_t index = SizeClass::getIndex(size);

    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;

    freeListSize_[index]++;

    if (shouldReturnToCentralCache(index))
    {
        returnToCentralCache(freeList_[index], size);
    }
}

bool ThreadCache::shouldReturnToCentralCache(size_t index)
{
    size_t threshold = 256;
    return (freeListSize_[index] > threshold);
}

void* ThreadCache::fetchFromCentralCache(size_t index)
{
    void* start = CentralCache::getInstance().fetchRange(index);
    if (!start) return nullptr;

    void* result = start;
    freeList_[index] = *reinterpret_cast<void**>(start);

    // Count only the blocks that remain in ThreadCache. The first block
    // is returned to the caller, so counting from start would overstate
    // the local cache size and distort the return-to-central policy.
    size_t batchNum = 0;
    void* current = freeList_[index];
    while (current != nullptr)
    {
        batchNum++;
        current = *reinterpret_cast<void**>(current);
    }

    freeListSize_[index] += batchNum;

    return result;
}

void ThreadCache::returnToCentralCache(void* start, size_t size)
{
    size_t index = SizeClass::getIndex(size);
    size_t alignedSize = SizeClass::roundUp(size);

    size_t batchNum = freeListSize_[index];
    if (batchNum <= 1) return;

    size_t keepNum = std::max(batchNum / 4, size_t(1));
    size_t returnNum = batchNum - keepNum;

    char* current = static_cast<char*>(start);
    char* splitNode = current;
    for (size_t i = 0; i < keepNum - 1; ++i)
    {
        splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
        if (splitNode == nullptr)
        {
            returnNum = batchNum - (i + 1);
            break;
        }
    }

    if (splitNode != nullptr)
    {
        void* nextNode = *reinterpret_cast<void**>(splitNode);
        *reinterpret_cast<void**>(splitNode) = nullptr;

        freeList_[index] = start;

        // After splitting the list, freeListSize_ must equal the number
        // of nodes still retained by this thread-local free list.
        freeListSize_[index] = keepNum;

        if (returnNum > 0 && nextNode != nullptr)
        {
            CentralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index);
        }
    }
}

} // namespace memoryPool
