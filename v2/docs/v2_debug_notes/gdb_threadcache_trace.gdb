set pagination off
set print pretty on
set print elements 20
set breakpoint pending on

break Kama_memoryPool::ThreadCache::allocate
commands
  silent
  printf "\n[ThreadCache::allocate] size=%zu\n", size
  continue
end

break src/ThreadCache.cpp:22
commands
  silent
  printf "[allocate:index-ready] size=%zu\n", size
  continue
end

break src/ThreadCache.cpp:25
commands
  silent
  printf "[allocate:before-dec] size=%zu index=%zu freeList=%p freeListSize=%zu\n", size, index, freeList_[index], freeListSize_[index]
  continue
end

break src/ThreadCache.cpp:29
commands
  silent
  printf "[allocate:after-dec] index=%zu freeList=%p freeListSize=%zu\n", index, freeList_[index], freeListSize_[index]
  continue
end

break Kama_memoryPool::ThreadCache::fetchFromCentralCache
commands
  silent
  printf "\n[ThreadCache::fetchFromCentralCache] index=%zu before freeList=%p freeListSize=%zu\n", index, freeList_[index], freeListSize_[index]
  continue
end

break src/ThreadCache.cpp:75
commands
  silent
  printf "[fetchFromCentralCache:after-fetchRange] index=%zu start=%p next=%p freeListSize=%zu\n", index, start, start ? *(void**)start : 0, freeListSize_[index]
  continue
end

break src/ThreadCache.cpp:93
commands
  silent
  printf "[fetchFromCentralCache:before-add] index=%zu result=%p batchNum=%zu current=%p freeList=%p freeListSize=%zu\n", index, result, batchNum, current, freeList_[index], freeListSize_[index]
  continue
end

break Kama_memoryPool::ThreadCache::deallocate
commands
  silent
  printf "\n[ThreadCache::deallocate] ptr=%p size=%zu\n", ptr, size
  continue
end

break src/ThreadCache.cpp:54
commands
  silent
  printf "[deallocate:before-inc] ptr=%p size=%zu index=%zu freeList=%p freeListSize=%zu\n", ptr, size, index, freeList_[index], freeListSize_[index]
  continue
end

break src/ThreadCache.cpp:57
commands
  silent
  printf "[deallocate:after-inc] index=%zu freeList=%p freeListSize=%zu\n", index, freeList_[index], freeListSize_[index]
  continue
end

break Kama_memoryPool::ThreadCache::returnToCentralCache
commands
  silent
  printf "\n[ThreadCache::returnToCentralCache] start=%p size=%zu\n", start, size
  continue
end

break src/ThreadCache.cpp:112
commands
  silent
  printf "[returnToCentralCache:counts] index=%zu alignedSize=%zu batchNum=%zu keepNum=%zu returnNum=%zu start=%p freeListSize=%zu\n", index, alignedSize, batchNum, keepNum, returnNum, start, freeListSize_[index]
  continue
end

break Kama_memoryPool::CentralCache::fetchRange
commands
  silent
  printf "\n[CentralCache::fetchRange] index=%zu\n", index
  continue
end

break src/CentralCache.cpp:59
commands
  silent
  printf "[CentralCache::fetchRange:miss] index=%zu blockSize=%zu result(before page)=%p\n", index, size, result
  continue
end

break src/CentralCache.cpp:74
commands
  silent
  printf "[CentralCache::fetchRange:span] index=%zu size=%zu numPages=%zu blockNum=%zu start=%p\n", index, size, numPages, blockNum, start
  continue
end

break src/CentralCache.cpp:137
commands
  silent
  printf "[CentralCache::fetchRange:return] index=%zu result=%p next=%p\n", index, result, result ? *(void**)result : 0
  continue
end

break Kama_memoryPool::CentralCache::returnRange
commands
  silent
  printf "\n[CentralCache::returnRange] start=%p size=%zu index=%zu\n", start, size, index
  continue
end

break src/CentralCache.cpp:164
commands
  silent
  printf "[CentralCache::returnRange:stored] start=%p blockSize=%zu blockCount=%zu counted=%zu end=%p\n", start, blockSize, blockCount, count, end
  continue
end

break Kama_memoryPool::PageCache::allocateSpan
commands
  silent
  printf "\n[PageCache::allocateSpan] numPages=%zu\n", numPages
  continue
end

run
