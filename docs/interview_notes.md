# Interview Notes

## 项目一句话介绍

这是一个基于公开 memory-pool 项目学习和二次优化的 C++ 高性能内存池项目，重点实现高频小对象分配复用、三层缓存架构、锁竞争优化和可解释的性能测试。

## v1/v2/v3 差异

- `v1`：基础内存池。通过 `HashBucket` 按大小分桶，每个 bucket 对应一个 fixed-size `MemoryPool`，释放后的 slot 进入 free list 复用。
- `v2`：三层缓存。`ThreadCache` 负责线程本地复用，`CentralCache` 负责跨线程共享，`PageCache` 负责页级 span 申请。
- `v3`：在 v2 基础上调整批量获取策略，使不同 size class 可以按不同 batch 数量从 CentralCache 获取内存块。

## allocate 完整链路

1. 用户调用 `MemoryPool::allocate(size)`。
2. 如果 `size == 0`，按最小对齐大小处理。
3. 如果 `size > MAX_BYTES`，直接走系统分配。
4. 根据 `SizeClass::getIndex(size)` 找到 size class。
5. `ThreadCache` 优先从当前线程本地 `freeList_[index]` pop 一个块。
6. 如果本地为空，向 `CentralCache` 批量获取。
7. 如果 CentralCache 对应 size class 为空，向 `PageCache` 申请 span 并切分成小块。
8. 返回一个块给用户，其余块留在缓存链表中等待复用。

## deallocate 完整链路

1. 用户调用 `MemoryPool::deallocate(ptr, size)`。
2. 大对象直接归还给系统。
3. 小对象根据 size class 找到对应 index。
4. `ThreadCache` 将释放块头插到本地 `freeList_[index]`。
5. 更新本地 free list 计数。
6. 如果本地缓存超过阈值，将部分链表切分并归还给 `CentralCache`。
7. CentralCache 后续可继续向其他线程分发这些空闲块，必要时再与 PageCache 交互。

## 高频面试题与回答

**为什么内存池比 malloc/free 快？**

内存池把通用分配器的复杂路径前移到批量申请阶段，常见小对象分配只需要从 free list pop 一个节点。它减少系统调用、锁竞争、元数据维护和碎片整理成本。

**size class 的作用是什么？**

size class 将任意小对象尺寸归一到有限几个固定大小类别，便于复用固定大小块，也让 free list 管理更简单。代价是少量内部碎片。

**ThreadCache 为什么能减少锁竞争？**

多数分配和释放只访问线程本地链表，不需要抢全局锁。只有本地缓存不足或过多时才访问 CentralCache。

**CentralCache 和 PageCache 分别解决什么问题？**

CentralCache 解决多个线程之间的空闲块共享和再分配问题；PageCache 负责向系统申请大块页级内存，并把大块内存切给上层缓存。

**大对象为什么不走内存池？**

大对象尺寸差异大，进入小对象池会造成严重内部碎片，也容易占用本应用于高频小对象复用的缓存空间。直接走系统分配更简单、边界更清晰。

**freeListSize_ bug 是怎么发现和修复的？**

调试时发现 `freeListSize_` 和真实本地 free list 长度不一致。原因是空链表也会先递减，导致 `size_t` 下溢；从 CentralCache 获取时还把返回给用户的块计入了本地缓存。修复方式是只在真实 pop 成功后递减，并从留在本地的第二个节点开始统计 batch。

**和 PyTorch CUDA caching allocator / vLLM KV Cache block 管理有什么类比？**

它们都强调资源复用和分层管理：按大小或块规格分桶，局部缓存优先，集中资源池负责跨 worker 调度，底层管理大块内存。内存池里的 ThreadCache / CentralCache / PageCache 可以类比推理系统里的 worker-local buffer、共享 block pool 和底层大块显存/内存管理器。

## AI Infra 映射话术

在推理服务中，每个 worker 会频繁创建 request context、token metadata、scheduler node、临时通信 buffer 或 KV Cache block 句柄。如果每次都直接向系统或 CUDA runtime 申请释放，延迟和锁竞争会很明显。这个 C++ 内存池项目体现的思路是：小对象按 size class 复用，worker 本地优先分配，跨 worker 通过共享池协调，底层按页或大块统一申请。这和高性能推理引擎中管理请求生命周期、KV Cache block、临时 buffer 的思想是相通的。
