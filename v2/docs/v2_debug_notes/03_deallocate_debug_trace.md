# deallocate 调试路径

## 调用链

```text
MemoryPool::deallocate(ptr, size)
  -> ThreadCache::deallocate(ptr, size)
     -> size > MAX_BYTES 时 free(ptr)
     -> index = SizeClass::getIndex(size)
     -> *reinterpret_cast<void**>(ptr) = freeList_[index]
     -> freeList_[index] = ptr
     -> freeListSize_[index]++
     -> shouldReturnToCentralCache(index)
        -> freeListSize_[index] > 256
     -> returnToCentralCache(freeList_[index], size)
        -> batchNum = freeListSize_[index]
        -> keepNum = max(batchNum / 4, 1)
        -> returnNum = batchNum - keepNum
        -> 链表切分
        -> CentralCache::returnRange(nextNode, returnNum * alignedSize, index)
```

## 关键源码点

| 文件 | 行号 | 观察点 |
| --- | ---: | --- |
| `src/ThreadCache.cpp` | 47 | 释放时重新计算 `index` |
| `src/ThreadCache.cpp` | 50-51 | 头插法写 next 指针 |
| `src/ThreadCache.cpp` | 54 | `freeListSize_[index]++` |
| `src/ThreadCache.cpp` | 64-68 | 是否超过阈值 256 |
| `src/ThreadCache.cpp` | 107-112 | 使用 `freeListSize_[index]` 推导 `batchNum/keepNum/returnNum` |
| `src/ThreadCache.cpp` | 118-144 | 切断链表并归还 CentralCache |
| `src/CentralCache.cpp` | 145-164 | CentralCache 接收链表并头插 |

## deallocate 关键变量表

| 变量 | 来源 | 观察含义 |
| --- | --- | --- |
| `ptr` | 用户传回的块 | 应被头插到 ThreadCache 本地链表 |
| `size` | 用户传回大小 | 用来重新计算 size class |
| `index` | `SizeClass::getIndex(size)` | 必须与 allocate 时一致 |
| `freeList_[index]` | 本地链表头 | 释放前是旧 head，释放后等于 `ptr` |
| `freeListSize_[index]` | 本地计数 | 释放后应加 1，但若此前已下溢则意义失真 |
| `start` | `returnToCentralCache(freeList_[index], size)` | 归还前本地链表头 |
| `batchNum` | `freeListSize_[index]` | 当前代码直接信任计数，而不是实际遍历链表 |
| `current/splitNode` | 切分链表 | 找到保留部分的尾节点 |
| `returnNum` | `batchNum - keepNum` | 作为返回 CentralCache 的数量依据 |

## 释放路径观察

| 检查项 | 结果 |
| --- | --- |
| deallocate 是否头插回 ThreadCache | 是，第 50-51 行就是标准头插 |
| `freeListSize_[index]` 是否加一 | 是，第 54 行 |
| 超过阈值是否归还 CentralCache | 是，阈值为 256 |
| `returnToCentralCache` 的数量统计是否可靠 | 不可靠。它完全信任 `freeListSize_[index]`，而该计数可能已经下溢或包含错误块数 |
| 归还链表是否按真实链长校正 | 只在遍历保留段时发现 `nullptr` 会局部修正，但 `batchNum` 本身仍可能严重错误 |

## 典型释放推演：1024 字节

```text
ThreadCache::deallocate(ptr, 1024)
  index = 127
  *ptr = freeList_[127]
  freeList_[127] = ptr
  freeListSize_[127]++
  if (freeListSize_[127] > 256):
      returnToCentralCache(freeList_[127], 1024)
```

如果 `freeListSize_[127]` 正常，单次释放后从 0 到 1，不会归还 CentralCache。

如果此前发生下溢并残留为巨大值，单次释放会继续保持巨大值或绕回，`shouldReturnToCentralCache(127)` 可能立即为 true。随后 `returnToCentralCache()` 会把这个巨大值当作 `batchNum`，导致 `keepNum/returnNum` 全部失真。

## returnToCentralCache 的实际风险

`returnToCentralCache()` 通过链表 next 指针寻找第 `keepNum` 个节点。但是 `keepNum` 来自计数，不来自真实链长：

| 情况 | 影响 |
| --- | --- |
| `freeListSize_` 大于真实链长 | 遍历时提前遇到 `nullptr`，只做部分修正；`freeListSize_` 可能被设置为一个仍然不可信的 `keepNum` |
| `freeListSize_` 小于真实链长 | 可能归还不足，ThreadCache 持有过多缓存 |
| `freeListSize_` 为 `SIZE_MAX` | `keepNum` 约为 `SIZE_MAX / 4`，循环理论上巨大；实际会在链表短时遇到 `nullptr`，但路径非常危险 |
