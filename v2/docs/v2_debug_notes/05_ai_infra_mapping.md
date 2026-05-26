# v2 内存池与 AI Infra 映射

## 架构映射

| v2 组件 | AI Infra / 推理服务中的类比 | 价值 |
| --- | --- | --- |
| ThreadCache | worker-local allocator、请求执行线程的本地对象池 | 减少锁竞争，降低小对象分配延迟 |
| CentralCache | 进程级共享 arena / central freelist | 在线程之间平衡内存，避免每个线程无限增长 |
| PageCache | 页级 arena、OS mmap 层、runtime memory manager | 批量向系统申请，摊薄系统调用成本 |
| size class | tensor metadata、RPC buffer、调度节点等固定尺寸对象类别 | 减少碎片，提高复用概率 |
| span | 大块连续页 | 给上层切分小块，方便批量管理 |

## 为什么 ThreadCache 是 v2 的重点

v1 如果只有一个简单内存池或全局 free list，多线程推理时每次分配/释放都可能争抢同一把锁。v2 引入 ThreadCache 后，常见路径变为：

```text
请求线程
  -> 从本地 freeList 弹出对象
  -> 使用对象
  -> 释放回本地 freeList
```

只要本地命中，就不需要进入 CentralCache 和 PageCache。这个变化对高并发服务很关键，因为推理链路中有大量短生命周期对象。

## 和大模型推理链路的对应关系

| 推理服务场景 | 内存池对应路径 |
| --- | --- |
| HTTP/gRPC 请求进入 | 分配请求上下文、小 buffer、队列节点 |
| tokenizer / detokenizer | 分配临时 vector、字符串片段、token 元数据 |
| batch scheduler | 分配任务节点、优先级队列节点、状态对象 |
| KV cache 管理 | 小对象元数据走 size class，大块显存/页缓存走更大粒度 allocator |
| 日志和 tracing | 高频创建 span/event 对象，适合 ThreadCache 快路径 |
| 请求结束 | 大量对象回到本地 freeList，下一批请求复用 |

## 调试变量在系统设计里的意义

| 调试变量 | 系统含义 | 如果错误会怎样 |
| --- | --- | --- |
| `freeList_[index]` | 本线程可立即复用的对象池 | 空链表会导致频繁走全局路径 |
| `freeListSize_[index]` | 本地缓存水位 | 错误水位会让回收策略失真 |
| `batchNum` | 从中心缓存补货数量 | 过小没有批量收益，过大可能占用过多内存 |
| `returnNum` | 回收到中心缓存的数量 | 错误会导致内存滞留或频繁锁竞争 |
| PageCache `numPages` | 系统级申请粒度 | 粒度太小系统调用多，太大内存浪费 |
| CentralCache 链表长度 | 全局可复用容量 | 影响多线程间内存再分配效率 |

## 可写进简历的点

可以把这个项目描述成：

| 简历点 | 表述建议 |
| --- | --- |
| 三级缓存架构 | “实现/分析了 ThreadCache、CentralCache、PageCache 三级缓存结构，降低多线程小对象分配的锁竞争和系统调用开销” |
| size class | “基于 8 字节对齐和 size class 管理小对象，提升内存复用效率” |
| 调试能力 | “使用 gdb/源码级调试追踪 allocate/deallocate 全链路，定位 size_t 下溢导致的 freeList 计数异常” |
| 并发意识 | “分析了线程本地缓存与中心缓存之间的批量迁移策略、锁粒度和回收阈值” |
| AI Infra 关联 | “将 C++ 内存池优化映射到推理服务中的 worker-local allocator、请求生命周期对象复用和 P99 延迟优化” |

## 后续优化方向

| 方向 | 说明 |
| --- | --- |
| 修复 ThreadCache 计数 | 先保证 `freeListSize_` 永远等于真实链长 |
| 明确批量 fetch 语义 | 让 CentralCache 真正批量返回，或改名为 fetchOne |
| 增加 debug 校验 | Debug 模式下遍历链表校验 `freeListSize_` |
| 增加定向单测 | 针对 1024/index 127、空链表 allocate、多次 allocate/deallocate 写测试 |
| 降低 false sharing | 高频计数和链表头可以考虑 cache line 对齐 |
| 统计指标 | 暴露 local hit、central miss、page alloc、return count 等指标，方便压测分析 |

## 和真实推理服务的距离

当前 v2 已经有很好的教学骨架，但距离线上 allocator 还差几类能力：

| 能力 | 线上意义 |
| --- | --- |
| 严格计数和一致性校验 | 防止长时间运行后水位失真 |
| 完整批量迁移策略 | 平衡吞吐和内存占用 |
| 更细的并发安全设计 | 避免全局锁和 false sharing 成为瓶颈 |
| 监控与压测指标 | 线上定位 P99 抖动必须有 allocator 维度数据 |
| 与大块内存/显存管理区分 | 推理系统同时存在小对象元数据和大块 tensor/KV cache |

## 总结

v2 的核心价值不是“替代 malloc”，而是展示现代高并发系统里 allocator 的分层思想：本地快路径、中心协调层、页级后端。这个思想和大模型推理服务高度相关，因为推理系统的性能问题经常来自大量小对象的分配释放、线程竞争和内存水位管理。
