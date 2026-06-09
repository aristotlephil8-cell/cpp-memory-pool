# allocate 调试路径

## 调用链

```text
MemoryPool::allocate(size)
  -> ThreadCache::allocate(size)
     -> SizeClass::getIndex(size)
     -> freeListSize_[index]--
     -> if (freeList_[index]) pop head
     -> else fetchFromCentralCache(index)
        -> CentralCache::fetchRange(index)
           -> if centralFreeList_[index] empty:
              -> fetchFromPageCache((index + 1) * ALIGNMENT)
                 -> PageCache::allocateSpan(numPages)
              -> split span into blocks
              -> return first block as result
              -> put remaining blocks into centralFreeList_[index]
           -> else:
              -> pop one block from centralFreeList_[index]
```

## 关键源码点

| 文件 | 行号 | 观察点 |
| --- | ---: | --- |
| `src/ThreadCache.cpp` | 22 | 计算 `index` |
| `src/ThreadCache.cpp` | 25 | `freeListSize_[index]--`，存在下溢风险 |
| `src/ThreadCache.cpp` | 29-36 | 本地命中或 miss 后进入 CentralCache |
| `src/ThreadCache.cpp` | 74-79 | `start/result/freeList_[index]` 关系 |
| `src/ThreadCache.cpp` | 82-93 | 遍历 `current` 统计 `batchNum` |
| `src/CentralCache.cpp` | 53-59 | 读取中心链表，空则向 PageCache 申请 |
| `src/CentralCache.cpp` | 71-74 | 计算 `numPages` 和 `blockNum` |
| `src/CentralCache.cpp` | 87-94 | 断开 `result`，把剩余块留在 CentralCache |
| `src/PageCache.cpp` | 8-63 | 按页分配 span |

## 预期 gdb 观察方式

当前 Windows 会话无法执行 Linux ELF。可在 WSL/Linux 中运行：

```bash
cd /mnt/f/Work/master/CodingLearning/JobApplicationProject/RefProject/memory-pool/v2
gdb -q -x docs/v2_debug_notes/gdb_threadcache_trace.gdb --args build/unit_test
```

为了避免 `-O2` 让局部变量被优化掉，建议临时重新构建 Debug/O0 版本：

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS_DEBUG="-g -O0"
cmake --build build-debug
gdb -q -x docs/v2_debug_notes/gdb_threadcache_trace.gdb --args build-debug/unit_test
```

## allocate 关键变量表

以 `testBasicAllocation()` 中 `MemoryPool::allocate(1024)` 为例：

| 变量 | 理论值/来源 | 观察含义 |
| --- | --- | --- |
| `size` | 1024 | 用户请求 1024 字节 |
| `index` | `(1024 + 7) / 8 - 1 = 127` | size class 127 |
| `freeList_[127]` | 首次请求时为 `nullptr` | ThreadCache 本地 miss |
| `freeListSize_[127]` | 初始 0，但第 25 行后变为 `SIZE_MAX` | 直接证明下溢路径 |
| `start` | `CentralCache::fetchRange(127)` 返回值 | 当前代码实际为单个块 |
| `result` | 等于 `start` | 返回给用户的块 |
| `batchNum` | 当前通常为 1 | 因 CentralCache 已把 `result->next` 置空 |
| `current` | 遍历链表用 | 从 `start` 遍历到 `nullptr` |
| CentralCache 返回链表长度 | 当前通常为 1 | 不符合“批量返回给 ThreadCache”的注释意图 |
| PageCache 申请页数 | 对 1024 字节为 8 页 | `SPAN_PAGES = 8`，共 32768 字节 |
| CentralCache 切分块数 | `8 * 4096 / 1024 = 32` | 返回 1 个，中心缓存留下 31 个 |

## 调试观察结果

| 检查项 | 结果 |
| --- | --- |
| 首次申请本地 freeList 是否为空 | 是，构造函数清空了本地链表 |
| 本地 miss 后是否进入 `CentralCache::fetchRange` | 是 |
| CentralCache 是否批量返回链表给 ThreadCache | 否。它批量切分 span，但只返回一个块 |
| ThreadCache 是否把剩余块挂到本地 freeList | 当前通常否，因为 `start->next` 已被 CentralCache 置空 |
| `freeListSize_[127]` 是否可能巨大 | 是。首次 1024 分配时从 0 减 1，变为 `18446744073709551615` |

## 源码级推演：1024 首次分配

```text
ThreadCache::allocate(1024)
  index = 127
  freeListSize_[127]--       // 0 -> SIZE_MAX
  freeList_[127] == nullptr  // 本地 miss
  fetchFromCentralCache(127)
    CentralCache::fetchRange(127)
      centralFreeList_[127] == nullptr
      fetchFromPageCache(1024)
        PageCache::allocateSpan(8)
      blockNum = 32
      result = span start
      centralFreeList_[127] = 第二个块
      *result = nullptr
      return result
    start = result，且 *start == nullptr
    freeList_[127] = nullptr
    batchNum = 1
    freeListSize_[127] += 1  // SIZE_MAX + 1 -> 0
    return result
```

这解释了一个微妙现象：单次 miss 可能先下溢再被 `+1` 绕回 0，所以不一定每次都能在测试末尾看到巨大数。但如果后续路径不是精确加 1，或者 free/return 逻辑介入，计数会变得不可解释。
