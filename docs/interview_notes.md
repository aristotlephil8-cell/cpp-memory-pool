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
## v1 性能面试补充

**面试官问：为什么你的内存池不一定比 malloc 快？**

回答：因为 malloc/free 本身已经高度优化，很多现代 allocator 对小对象也有线程缓存和快速路径。内存池只有在高频小对象、对象大小稳定、复用充分、锁竞争可控的场景下才更容易体现优势。v1 还包含 HashBucket 分桶、atomic/CAS、block 分配锁、placement new 和显式析构等额外成本，如果 benchmark 规模太小或者场景不匹配，内存池不一定领先。

**面试官问：如何设计 benchmark 才公平？**

回答：需要区分 Debug 和 Release，性能结论主要看 Release；计时用 `std::chrono::steady_clock`，多轮运行取平均；核心循环里不能做 I/O；要避免编译器优化掉分配结果；测试场景要覆盖固定大小小对象、批量分配再批量释放、重复复用、多线程、小对象混合大小和大对象绕过。这样才能判断内存池到底在哪些场景有效，而不是用一个很小的 demo 循环得出片面结论。

**面试官问：v1 的瓶颈在哪里？**

回答：v1 的主要瓶颈是每次分配释放都要经过 HashBucket size class 映射，free list 使用 atomic/CAS，即使单线程也有原子操作成本；多线程下同一 size class 的 free list 还可能产生 CAS 重试和 ABA 风险。block 扩容时还有 mutex 保护，虽然不是常态路径，但首次分配和扩容阶段会影响测试结果。

**面试官问：如何优化 v1？**

回答：可以先拆分单线程和多线程版本。单线程版本用普通指针 free list，去掉 atomic/CAS 成本；多线程版本可以引入 ThreadCache，把每个线程的高频分配留在本地，减少共享 free list 竞争。还可以调大 block size，优化 size class index 计算，补充延迟分位数和碎片率统计，再根据数据决定是否继续演进到 v2/v3 的分层缓存结构。

**面试官问：这个项目和 AI Infra 有什么关系？**

回答：AI Infra 里也有大量短生命周期对象和 buffer，例如 request context、scheduler node、token metadata、临时通信 buffer、KV Cache block handle。内存池体现的是高频资源复用和分层缓存思想：小对象按 size class 管理，本地优先复用，竞争严重时引入更细粒度缓存层。这个思路可以迁移到推理服务的请求生命周期管理、KV Cache block 复用和 worker-local buffer pool 设计中。

## v2 性能面试补充

**面试官问：v2 相比 v1 最大变化是什么？**

回答：v2 从 v1 的基础 free list 内存池升级为 ThreadCache / CentralCache / PageCache 三层缓存。v1 更像按 size class 分桶的共享对象池，v2 则把高频小对象分配优先放在线程本地，只有本地缓存不足或过多时才访问中心缓存和页缓存。

**面试官问：ThreadCache 为什么能减少锁竞争？**

回答：ThreadCache 是 `thread_local` 的，每个线程有自己的 free list。只要本地 free list 命中，分配释放不需要访问 CentralCache 的锁，因此可以减少多线程下的全局竞争。

**面试官问：CentralCache 为什么要批量给 ThreadCache 补货？**

回答：批量补货可以摊薄中心锁和 PageCache 访问成本。如果每次 ThreadCache miss 都只拿一个块，那么每次本地耗尽都要重新进入 CentralCache，三层缓存的优势会被削弱。当前 v2 代码在这点上还有优化空间。

**面试官问：PageCache 负责什么？**

回答：PageCache 负责页级大块内存管理。CentralCache 没有对应 size class 的空闲块时，会向 PageCache 申请 span；PageCache 再通过 `mmap` 向系统申请页，并把 span 切给上层。

**面试官问：为什么现代 malloc/free 有时仍然比你的内存池快？**

回答：现代 allocator 本身已经有线程缓存、size class、arena 和 fast path。自研内存池如果 batch 策略、锁粒度、归还策略或元数据管理不够成熟，在某些场景仍可能输给 malloc/free。

**面试官问：如何设计公平 benchmark？**

回答：要区分 Debug/Release，使用 `steady_clock`，多轮平均，避免核心循环 I/O，写入分配到的内存避免优化，分别比较 memory pool、malloc/free 和 new/delete，并覆盖固定小对象、batch 分配释放、多线程同 size class、多线程 mixed size、refill、大对象 bypass 和长时间压力。

**面试官问：v2 的瓶颈在哪里？**

回答：当前 v2 的瓶颈主要在 CentralCache 和归还路径：`fetchRange` 没有真正批量返回多个块给 ThreadCache；`returnToCentralCache` 的固定阈值比较粗；大批量同 size class 释放会暴露稳定性风险；PageCache 的 SpanTracker 也有固定容量和线性扫描问题。

**面试官问：如果优化 v2，你会先优化哪里？**

回答：我会先修复归还 CentralCache 的稳定性，再实现真正的 ThreadCache 批量 refill，然后把批量大小和归还阈值按 size class 自适应。之后再优化 SpanTracker 索引，并补充 P95/P99 延迟和内存占用指标。

**面试官问：这个三层缓存结构和 TCMalloc 有什么关系？**

回答：它借鉴了 TCMalloc 的核心思想：线程本地缓存处理高频小对象，中心缓存负责跨线程共享，页级缓存负责向系统申请和管理大块内存。当前项目是学习版实现，很多策略还比工业级 TCMalloc 简化。

**面试官问：这个项目和 AI Infra / 推理服务中的 buffer pool 有什么关系？**

回答：推理服务里有大量 request context、token metadata、scheduler node、通信 buffer 和 KV Cache block 句柄，这些对象生命周期短、分配频繁。ThreadCache 类似 worker-local buffer pool，CentralCache 类似多 worker 共享池，PageCache 类似底层大块内存管理器。
