# v2 ThreadCache 总览

## 调试范围

本轮重点阅读并分析以下文件：

| 文件 | 角色 |
| --- | --- |
| `include/MemoryPool.h` | 对外入口，把 `allocate/deallocate` 转发给线程本地 `ThreadCache` |
| `include/ThreadCache.h` | 每线程本地缓存，维护 `freeList_` 和 `freeListSize_` |
| `src/ThreadCache.cpp` | 小对象分配、释放、本地 miss 后向 CentralCache 获取、超过阈值后归还 |
| `src/CentralCache.cpp` | 全局中心缓存，按 size class 管理空闲链表，必要时向 PageCache 申请 span |
| `src/PageCache.cpp` | 页级缓存，按页数管理 span，底层通过 `mmap` 申请内存 |
| `tests/UnitTest.cpp` | 覆盖基础分配、写入、多线程、边界和压力测试 |

当前工作区的 `build/unit_test` 是 Linux/ELF 产物，编译参数包含 `-g -O2`。本次 Windows 会话无法启动 WSL，因此未能实际运行 ELF；已生成 `gdb_threadcache_trace.gdb`，可在 WSL/Linux 下复跑观察点。

## 核心架构

v2 采用三级缓存结构：

| 层级 | 粒度 | 主要数据结构 | 目标 |
| --- | --- | --- | --- |
| ThreadCache | 每线程、每 size class | `std::array<void*, FREE_LIST_SIZE> freeList_`，`freeListSize_` | 减少多线程下频繁抢全局锁 |
| CentralCache | 进程全局、每 size class | `centralFreeList_`，每个 size class 一个自旋锁 | 在线程缓存之间调度小块内存 |
| PageCache | 页/span | `freeSpans_`，`spanMap_`，`mmap` | 管理大块连续页，向系统申请和回收 |

`FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT = 256KB / 8 = 32768`。`SizeClass::getIndex(size)` 以 8 字节为步长，例如：

| size | index | 对齐后块大小 |
| ---: | ---: | ---: |
| 0 | 0 | 8 |
| 1 | 0 | 8 |
| 8 | 0 | 8 |
| 128 | 15 | 128 |
| 1024 | 127 | 1024 |
| 256KB | 32767 | 256KB |

## allocate 调用链

```text
MemoryPool::allocate(size)
  -> ThreadCache::getInstance()->allocate(size)
     -> size == 0 时按 8 字节处理
     -> size > MAX_BYTES 时直接 malloc(size)
     -> index = SizeClass::getIndex(size)
     -> 尝试从 freeList_[index] 取块
     -> 本地为空时 fetchFromCentralCache(index)
        -> CentralCache::getInstance().fetchRange(index)
           -> centralFreeList_[index] 非空：摘一个块返回
           -> centralFreeList_[index] 为空：fetchFromPageCache(size)
              -> PageCache::allocateSpan(numPages)
                 -> 复用 freeSpans_ 或 mmap 新 span
              -> CentralCache 把 span 切成小块
              -> 返回第一个块，剩余块留在 CentralCache
```

## deallocate 调用链

```text
MemoryPool::deallocate(ptr, size)
  -> ThreadCache::getInstance()->deallocate(ptr, size)
     -> size > MAX_BYTES 时直接 free(ptr)
     -> index = SizeClass::getIndex(size)
     -> 头插到 freeList_[index]
     -> freeListSize_[index]++
     -> freeListSize_[index] > 256 时 returnToCentralCache(freeList_[index], size)
        -> 保留 1/4 在 ThreadCache
        -> 剩余链表调用 CentralCache::returnRange(nextNode, returnNum * alignedSize, index)
           -> 头插到 centralFreeList_[index]
           -> 延迟触发 performDelayedReturn()
              -> 统计 CentralCache 中各 span 空闲块
              -> 满足条件时 PageCache::deallocateSpan(spanAddr, numPages)
```

## 关键变量

| 变量 | 位置 | 含义 | 本轮观察 |
| --- | --- | --- | --- |
| `size` | Thread/Central/Page 路径 | 用户请求大小或对齐后的块大小 | `ThreadCache` 里先按用户 size 算 index，`CentralCache` 用 `(index + 1) * 8` |
| `index` | 各级缓存 | size class 下标 | 1024 字节对应 127，是你观察到异常的重点 |
| `ptr` | `deallocate` | 用户释放的块 | 释放时写入 next 指针并头插本地链表 |
| `start` | `fetchFromCentralCache/returnToCentralCache/returnRange` | 链表起点或 span 起点 | 当前 CentralCache 返回给 ThreadCache 的 `start` 实际通常是单块 |
| `result` | `fetchFromCentralCache/fetchRange` | 返回给调用方的块 | CentralCache 会把 `result` 与剩余链表断开 |
| `batchNum` | `fetchFromCentralCache/returnToCentralCache` | 代码认为的批量块数 | 当前实现中容易与真实链表长度不一致 |
| `current` | 链表遍历 | 遍历节点 | 用于统计链表长度 |
| `freeList_[index]` | ThreadCache | 本地空闲链表头 | miss 时为空，deallocate 后指向刚释放块 |
| `freeListSize_[index]` | ThreadCache | 本地空闲块数量 | 存在 size_t 下溢和计数不准问题 |

## 调试观察结论

| 现象 | 结论 |
| --- | --- |
| 第一次申请某个 size class 时 ThreadCache 本地链表是否为空 | 是。构造函数将 `freeList_` 初始化为 `nullptr` |
| 本地 miss 是否进入 `CentralCache::fetchRange` | 是。`ThreadCache::allocate` 在 `freeList_[index]` 为空时调用 `fetchFromCentralCache(index)` |
| CentralCache 是否批量返回一段链表 | 当前代码不是。它返回一个块，剩余块留在 `centralFreeList_[index]` |
| ThreadCache 是否返回第一个块并把剩余块挂到本地 freeList | 设计意图是这样，但当前 CentralCache 已把返回块 next 置空，所以 ThreadCache 通常拿不到剩余链表 |
| deallocate 是否通过头插法回到 ThreadCache | 是。`*ptr = freeList_[index]; freeList_[index] = ptr` |
| `freeListSize_[index]` 是否符合预期 | 不符合。`allocate` 在链表为空前先 `--`，可能从 0 下溢到 `SIZE_MAX` |

## 和 AI Infra / 大模型推理服务的关系

大模型推理服务会持续创建和释放大量小对象：请求上下文、token buffer、KV-cache 元数据、RPC 消息、日志结构、调度队列节点等。三级缓存的目标是把高频小对象分配尽量留在本线程，减少全局锁、系统调用和跨 NUMA/跨核竞争。

ThreadCache 对应推理服务里的 worker-local allocator 思路：每个推理线程或执行流维护局部缓存，快路径不碰全局结构。CentralCache 类似共享的内存中转站，PageCache 则接近从 OS 或大块 arena 获取页级资源。这个架构能解释为什么内存池不仅是 C++ 基础项目，也能映射到高并发推理系统里的尾延迟和吞吐优化。
