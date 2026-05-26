# C++ High Performance Memory Pool

## 项目简介

这是一个面向高频小对象分配场景的 C++ 高性能内存池项目。项目目标是减少频繁 `malloc` / `new` / `free` / `delete` 带来的分配开销、内存碎片和延迟抖动，并通过分层缓存结构提升多线程场景下的对象复用效率。

当前代码按 `v1`、`v2`、`v3` 三个版本组织，逐步从基础 fixed-size free list 内存池演进到类似 tcmalloc 思路的 `ThreadCache` / `CentralCache` / `PageCache` 三层缓存架构。

## 项目来源与二次优化说明

本项目基于公开项目 [youngyangyang04/memory-pool](https://github.com/youngyangyang04/memory-pool.git) 进行学习、复现和二次实现/优化，不包装成完全原创项目。

我在该参考项目基础上完成了：

- 本地仓库迁移与 GitHub 项目整理。
- Windows + WSL 环境下的 CMake 构建和测试验证。
- v2 `ThreadCache` 分配路径的手动调试、问题定位和 bug 修复。
- 项目命名空间统一重构为 `Avery_memoryPool`。
- README、迁移说明、调试笔记和面试表达文档整理。
- 后续将继续补充 benchmark、延迟分布、碎片率统计和架构图。

## 版本结构

- `v1`：基础内存池。核心结构包括 `HashBucket`、`MemoryPool`、`Slot` 和 free list，适合理解 fixed-size block 复用、对齐和简单对象池接口。
- `v2`：引入 `ThreadCache` / `CentralCache` / `PageCache` 三层缓存架构。每个线程优先访问本地 free list，本地不足时向中心缓存批量获取，中心缓存不足时向页缓存申请 span。
- `v3`：在 v2 基础上进一步调整批量获取策略，例如按 size class 控制 batch size，并优化 ThreadCache 与 CentralCache 之间的批量交互。

## 核心设计

- **size class 分桶**：将小对象请求按 8 字节对齐映射到固定大小类别，降低任意尺寸分配带来的碎片和管理复杂度。
- **内存对齐**：通过 `roundUp` 和 `getIndex` 保证对象落在对应 size class，减少未对齐访问和内部管理混乱。
- **free list**：释放后的小块以内嵌 next 指针串成链表，后续分配可 O(1) 复用。
- **ThreadCache**：线程本地缓存，优先满足当前线程的小对象分配，减少全局锁竞争。
- **CentralCache**：跨线程共享缓存，负责在多个 ThreadCache 之间调度空闲块，并在本地缓存过多时回收。
- **PageCache**：底层页级内存管理器，负责向系统申请较大 span，并将 span 切分给 CentralCache。
- **mmap / 页级内存申请**：v2/v3 的 PageCache 使用 `mmap` 申请页级内存，降低频繁小块系统调用。
- **大对象直接走系统分配**：超过 `MAX_BYTES` 的请求不进入小对象内存池，直接使用系统分配，避免大对象占用小块缓存体系。

## v2 调试中发现的问题

在 v2 `ThreadCache` 调试过程中发现 `freeListSize_[index]` 可能不准确：

1. `ThreadCache::allocate(size_t size)` 曾在确认 `freeList_[index]` 是否为空之前就执行 `freeListSize_[index]--`。当本地 free list 原本为空时，`size_t` 会从 0 下溢为极大的无符号整数。
2. `ThreadCache::fetchFromCentralCache(size_t index)` 曾从 `start` 开始统计 batch 数量，但 `start` 本身会作为本次分配结果返回给用户，不应该计入线程本地 free list。
3. 这些问题会影响 `shouldReturnToCentralCache(index)` 的判断，使 ThreadCache 的归还策略失真。

本次修复后，`freeListSize_[index]` 只统计当前 ThreadCache 本地 free list 中真实存在的空闲块数量：只有成功从本地链表 pop 节点时才递减；从 CentralCache 获取批量节点时，只从留在线程本地链表中的第二个节点开始计数。

## 构建和运行方式

### v1

`v1` 可以在 Windows/MSYS2、WSL 或 Linux 下构建。示例命令：

```bash
cd v1
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build .
./MemoryPoolProject
```

如果生成的可执行文件名称不同，可以在 `build` 目录下查找可执行文件并运行。

### v2

`v2` 推荐在 WSL/Linux 下构建运行：

```bash
cd v2
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build .
./unit_test
./perf_test
```

### v3

`v3` 推荐在 WSL/Linux 下构建运行：

```bash
cd v3
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build .
./unit_test
./perf_test
```

## 测试结果

本次回归结果：

- `v1` Debug 构建通过，并成功运行 `MemoryPoolProject`。
- `v2 unit_test` 通过。
- `v2 perf_test` 可运行。
- `v3 unit_test` 通过。
- `v3 perf_test` 可运行。

性能数据会随机器、编译器、负载和 WSL 状态变化。后续会补充稳定 benchmark 表格，目前不在 README 中虚构固定性能数字。

## 与 AI Infra / 深度学习底层架构的关系

这个项目虽然是 C++ 内存池，但它对应的是 AI Infra 中非常常见的底层资源复用思想：

- `ThreadCache` 类似每个推理 worker、本地执行线程或 tokenizer worker 的本地 buffer pool，优先无锁/少锁复用本地资源。
- `CentralCache` 类似多 worker 共享的资源池，用于在线程本地缓存不足或过量时做跨线程调度。
- `PageCache` 类似底层大块内存管理器，负责向系统申请大块连续资源，再切分给上层。
- size class 和 free list 思想可以类比 request context、scheduler node、token metadata、临时通信 buffer、KV Cache block 的复用。
- 三层缓存体现了高性能系统中的内存管理、缓存层次设计、锁竞争优化、批量分配和性能测试能力。

在 PyTorch CUDA caching allocator、vLLM KV Cache block manager、推理服务 request buffer pool 等系统中，都能看到类似的“按大小分桶、批量申请、局部复用、集中回收”的设计影子。

## 面试可讲点

- 为什么内存池比 `malloc/free` 快？
- size class 的作用是什么，为什么要按 8 字节对齐？
- free list 如何做到 O(1) 分配和释放？
- ThreadCache 为什么能减少锁竞争？
- CentralCache 和 PageCache 分别解决什么问题？
- 为什么大对象不走小对象内存池？
- v2 `freeListSize_` bug 是怎么发现、复现和修复的？
- `size_t` 下溢为什么会影响缓存归还策略？
- 这个项目和 PyTorch CUDA caching allocator / vLLM KV Cache block 管理有什么类比关系？

## 后续优化计划

- 补充更完整的 benchmark 场景，包括单线程、多线程、混合 size class 和高并发释放。
- 增加 P50 / P95 / P99 分配延迟统计。
- 增加内存占用、内部碎片率和 span 复用率统计。
- 进一步完善 v3 的批量获取和归还策略。
- 添加更多调试笔记、架构图和 AI Infra 映射说明。
