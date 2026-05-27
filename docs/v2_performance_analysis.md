# v2 Performance Analysis

## v2 架构简述

v2 从 v1 的基础 free list 内存池升级为三层缓存结构：

- `ThreadCache`：每个线程一个 `thread_local` 缓存，维护 `freeList_[index]` 和 `freeListSize_[index]`。本地 free list 命中时不需要访问全局锁。
- `CentralCache`：每个 size class 一个中心 free list，并用 `std::atomic_flag` 自旋锁保护。ThreadCache 本地为空时向 CentralCache 获取块；ThreadCache 本地缓存过多时归还部分块。
- `PageCache`：页级内存管理器，使用 `mmap` 申请 span，通过 `std::map` 管理空闲 span 和地址到 span 的映射。

完整分配链路：

`MemoryPool::allocate` -> `ThreadCache::allocate` -> `ThreadCache::fetchFromCentralCache` -> `CentralCache::fetchRange` -> `PageCache::allocateSpan` -> `mmap`

完整释放链路：

`MemoryPool::deallocate` -> `ThreadCache::deallocate` -> `ThreadCache::returnToCentralCache` -> `CentralCache::returnRange` -> `PageCache::deallocateSpan`

## ThreadCache / CentralCache / PageCache 分工

`ThreadCache` 的目标是把大多数小对象分配留在线程本地。只要 `freeList_[index]` 非空，分配就是一次链表 pop 和计数递减，不需要 CentralCache 锁。

`CentralCache` 的目标是在线程之间共享空闲块。它按 size class 分离锁，避免所有大小的对象竞争同一把全局锁。当前 v2 实现会在 CentralCache 为空时从 PageCache 取 8 页 span 并切分成小块。

`PageCache` 的目标是管理更大粒度的页级内存。它用 `mmap` 向系统申请页，释放时尝试合并相邻 span，并把空闲 span 放回 `freeSpans_`。

## 当前 benchmark 原本的问题

原 `v2/tests/PerformanceTest.cpp` 有这些不足：

- 使用 `high_resolution_clock`，不是更适合 benchmark 的 `steady_clock`。
- 只对比 `new/delete`，没有单独对比 `malloc/free`。
- 没有多轮平均，结果容易受一次运行波动影响。
- 场景覆盖不完整，缺少大对象 bypass、ThreadCache refill、长时间压力、单独固定 size class 和多线程 mixed size 的清晰分组。
- 多线程测试使用 `rand()`，结果不完全可复现。
- 输出只展示少数测试，没有把 ratio 写清楚。
- 没有明确区分 Debug 和 Release，性能结论容易被 Debug/`-O2` 混合配置干扰。

## 为什么 v2 理论上应优于 v1

v2 的优势来自缓存层次：

- v1 所有线程共享每个 bucket 的 free list；v2 使用 `thread_local` ThreadCache，命中时无需跨线程竞争。
- v2 的 CentralCache 按 size class 分锁，锁粒度比单一全局锁更细。
- v2 的 PageCache 以 span/page 为单位向系统申请内存，避免每次小对象都触发系统分配。
- ThreadCache 理论上应该批量从 CentralCache refill，从而摊薄中心锁和 PageCache 成本。

## 为什么 v2 仍可能不如 malloc/free

现代 malloc/free 自身已经高度优化，通常包含线程缓存、小对象 size class、arena 和 fast path。v2 如果实现细节不够成熟，仍可能输给系统 allocator：

- 当前 `CentralCache::fetchRange` 实际每次只返回一个块给 ThreadCache，ThreadCache 没有真正批量拿到一串本地缓存。这削弱了“三层缓存”的理论优势。
- `ThreadCache::returnToCentralCache` 的固定阈值 `256` 比较粗糙，可能导致缓存归还策略不稳定。
- 大批量同 size class 释放会触发归还路径，本次 benchmark 初版曾在该路径上触发崩溃，因此最终 benchmark 对 batch/warmup 做了 200 块分块，避免把已知归还风险混入常规性能表。
- CentralCache 使用自旋锁，锁竞争高时可能消耗 CPU。
- PageCache 使用 `std::map` 和全局 mutex，span 查找/合并不是低成本路径。
- `spanTrackers_` 固定 1024 项，长时间运行可能不够，且 `getSpanTracker` 是线性扫描。

## v2 的适用场景

v2 更适合：

- 高频小对象即时分配释放。
- 固定 size class 的线程本地复用。
- 多线程请求上下文、调度节点、临时 buffer 等短生命周期对象。
- mixed small size 但对象仍处于 `MAX_BYTES` 以下的场景。
- 需要减少全局锁竞争的服务端分配路径。

## v2 的不适用场景

v2 不适合：

- 大对象分配。超过 `MAX_BYTES` 会直接走 `malloc/free`。
- 大批量同 size class 一次性释放，当前归还 CentralCache 路径需要进一步验证。
- 对 page/span 频繁申请释放的长尾场景，PageCache 可能成为瓶颈。
- 期望无条件打败现代 malloc/free 的泛化 benchmark。

## 可优化方向

建议先分析再改源码，优先级如下：

1. **真正实现 ThreadCache 批量 refill**：让 CentralCache 返回一段链表给 ThreadCache，而不是只返回一个块。
2. **修复/验证 returnToCentralCache 稳定性**：大批量同 size class 释放不应导致崩溃或链表损坏。
3. **调整 ThreadCache 归还策略**：阈值可按 size class 自适应，避免小对象囤积过多或频繁归还。
4. **优化 SpanTracker**：避免固定 1024 项和线性扫描，改用地址范围索引或页号映射。
5. **减少 CentralCache 链表遍历成本**：return/fetch 时维护块数量，避免频繁遍历链表。
6. **补充分位延迟和内存占用统计**：平均耗时不足以反映推理服务中的 P95/P99 抖动。
