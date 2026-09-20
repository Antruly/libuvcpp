# 内存池与分配器指南

`src/expand/` 是四个**互相独立**的分配器，不是一个模块的四个部分：

| 类型 | 头 | 定位 |
|---|---|---|
| `uvcpp_memory_pool_enterprise` | `src/expand/uvcpp_page_heap.h:162` | **主分配器**（TCMalloc 风格，线程缓存 → 中央 span → 页） |
| `uvcpp_memory_pool` | `src/expand/uvcpp_memory_pool.h:788` | 另一套独立的内存池（7 档块 + MPSC 全局队列 + 线程缓存） |
| `uvcpp_memory_pool_span` | `src/expand/uvcpp_memory_pool_span.h:59` | 实验性 span 分配器，头注释自标 WIP |
| `uvcpp_page_allocator` | `src/expand/uvcpp_page_allocator.h:27` | 裸页分配（`mmap` / `VirtualAlloc`），上面几个的地基 |

**只有第一个接进了库。** 全仓 `uvcpp_enterprise_alloc` / `uvcpp_memory_pool_enterprise`
的调用者只有 `src/uvcpp/uvcpp_alloc.h` 一处（含自身 `.cpp`），而
`uvcpp_memory_pool` 与 `uvcpp_memory_pool_span` 在自己的文件之外**零调用者** ——
它们是给使用者自己拿去的，库内没有任何东西用它们。

- 打开方式：`-DUVCPP_BUILD_EXPAND=ON`（默认 `OFF`，`CMakeLists.txt:16`）
- 包含方式：直接用时 `<expand/uvcpp_memory_pool.h>`；**跟随全库分配策略**用
  `<uvcpp/uvcpp_alloc.h>`，那是唯一被库内使用的入口
- 打开后 `<uvcpp/uvcpp_alloc.h>` 里所有 `uvcpp_alloc*` 都改道到
  `uvcpp_enterprise_alloc`（`src/uvcpp/uvcpp_alloc.h:19-21`、`:25-87`）

> 本指南里的签名、默认值、行为都对着当前源码核过。凡是"这一层没做"、或者
> **头注释与实现不一致**的地方都明确标出来 —— 后者是这份文档里最值得看的部分。
> 这一模块的注释与实现不符之处比前七个模块加起来还多。

---

## 目录

1. [这一层是什么](#1-这一层是什么)
2. [打开方式与包含](#2-打开方式与包含)
3. [三个分配器的分工](#3-三个分配器的分工)
4. [最小用法](#4-最小用法)
5. [块大小档位](#5-块大小档位)
6. [统计读数](#6-统计读数)
7. [线程本地缓存](#7-线程本地缓存)
8. [生命周期与关闭](#8-生命周期与关闭)
9. [头注释与实现对不上](#9-头注释与实现对不上)
10. [典型坑](#10-典型坑)
11. [没做的（如实列出）](#11-没做的如实列出)

---

## 1. 这一层是什么

expand 模块对外只暴露**分配**这一件事，没有网络、没有回调、没有异步 —— 它是全库唯一
一个纯粹的、同步的模块。它出现的位置在分配点：`uvcpp_alloc.h` 那几个 helper 是
全库分配的统一门面。

```cpp
#include <uvcpp/uvcpp_alloc.h>

struct doc_widget {
  int id;
};

void doc_alloc_facade() {
  doc_widget* w = uvcpp::uvcpp_alloc<doc_widget>();   // 已经 memset 为 0
  w->id = 7;
  uvcpp::uvcpp_free(w);

  void* raw = uvcpp::uvcpp_alloc_bytes(64);
  uvcpp::uvcpp_free_bytes(raw);
}
```

门面一共六个名字（`src/uvcpp/uvcpp_alloc.h:25-87` 一套、`:89` 之后另一套，两套签名相同）：

| 名字 | 用途 |
|---|---|
| `uvcpp_alloc<T>()` | 单个对象，**已清零** |
| `uvcpp_alloc_arry<T>(len)` | 数组，**已清零** |
| `uvcpp_alloc_bytes(sz)` | 裸字节，**已清零** |
| `uvcpp_realloc_bytes(p, old_sz, sz)` | 搬迁式重分配，**`old_sz` 必须由调用方给准** |
| `uvcpp_free_bytes(p)` | 释放（`nullptr` 安全） |
| `uvcpp_free<T>(p)` | 同上，带类型的写法 |

`uvcpp_realloc_bytes` 的第二问不是装饰：内存池路径下**旧块的真实大小无法由指针反查**
（只有调用方知道自己要过多少字节）。传大了会读越界 —— `src/uvcpp/uvcpp_alloc.h:56-59` 记着这正是
"128KB 以上的 HTTP 响应会崩溃"的成因。传小了则静默截断已搬的数据。

---

## 2. 打开方式与包含

```bash
cmake -S . -B build -DUVCPP_BUILD_EXPAND=ON
```

**选项名和宏名不是同一个**，这是这一模块第一个容易绊人的地方：

| 你传的 | 代码里看到的 |
|---|---|
| `-DUVCPP_BUILD_EXPAND=ON` | `UVCPP_ENABLE_MEMORY_POOL=1` |
| `-DUVCPP_BUILD_EXPAND=OFF`（默认） | `UVCPP_ENABLE_MEMORY_POOL=0` |

选项到宏的映射在 `CMakeLists.txt:508-514` 一处完成。用预编译包时**什么都不用传** ——
包里的 `include/uvcpp/uvcpp_config.h` 就是那次构建的值，自己再传一个冲突的值是硬 `#error`。

关掉时 `src/expand/` 整目录被 `list(FILTER ... EXCLUDE REGEX "src/expand/")` 排除
（`CMakeLists.txt:606-615`），并且**不安装头**（`CMakeLists.txt:1005-1008`）。于是
"关掉之后包含它"在两条路上表现**完全不同**：

| 你怎么构建 | `#include <expand/uvcpp_memory_pool.h>` 的结果 |
|---|---|
| 源码树自己构建 | **编得过，链接失败** —— 头还在 `src/expand/` 下，且四个头里**没有任何一个用宏守卫自己**（唯一提到这个宏的地方是 `src/expand/uvcpp_page_heap.h:8` 的一句注释），没有 `#error` 兜底；但符号一个都没编进库 |
| 用预编译包 | **编译期就报找不到头** —— `include/expand/` 根本不在包里 |

链接失败那一路报的是 `undefined reference to uvcpp::uvcpp_memory_pool::allocate(unsigned long long)`，
看上去像"某个 `.cpp` 忘了加进构建"，实际是你的开关没开。对照这个模块的四个 `#error`：
它一个都没有。

> `CMakeLists.txt:13-15` 说生成头已经消除了宏隐患、"所以默认关这件事与那个隐患不再有关系了"。
> 生成头**只随预编译包发出去**，所以在"拿源码树自己构建"这条路上这句话说过头了 ——
> 源码树的使用者没有任何东西替他兜住"忘了开开关却包含了头"。这一处**已记在案，本轮未改**。

---

## 3. 三个分配器的分工

```cpp
#include <expand/uvcpp_page_heap.h>

void doc_enterprise_basic() {
  void* p = uvcpp::uvcpp_enterprise_alloc(4096);
  if (p) {
    uvcpp::uvcpp_enterprise_free(p);
  }
}
```

`uvcpp_enterprise_alloc` / `uvcpp_enterprise_free` 是一对自由函数
（`src/expand/uvcpp_page_heap.h:336`、`:342`），内部走 `uvcpp_memory_pool_enterprise::instance()`
这个函数内静态单例（`src/expand/uvcpp_page_heap.cpp:1281-1285`），**不需要 `init()`**。这是库内
唯一在用的那套。

另一个池 `uvcpp_memory_pool` 要显式构造和 `init()`：

```cpp
#include <expand/uvcpp_memory_pool.h>

void doc_pool_basic() {
  uvcpp::memory_pool_config cfg;      // 默认档位
  cfg.align = 16;

  uvcpp::uvcpp_memory_pool pool(cfg);
  if (!pool.init()) return;           // init() 先 validate()，不合法直接 false

  void* p = pool.allocate(100);       // 100B 落在 SMALL 档
  if (p) {
    pool.deallocate(p);               // 归还进本线程的缓存，不是还给系统
  }

  pool.shutdown();
}
```

实验性的第三个：

```cpp
#include <expand/uvcpp_memory_pool_span.h>

void doc_span_basic() {
  uvcpp::uvcpp_memory_pool_span& sp = uvcpp::uvcpp_memory_pool_span::instance();
  void* p = sp.alloc(128);
  if (p) {
    sp.free_mem(p);
  }
  sp.try_merge();   // 空实现：不合并任何东西（头注释现在明说了）
}
```

**`uvcpp_memory_pool` 和 `uvcpp_memory_pool_enterprise` 是两套互不相干的内存。**
它们各自向系统要内存、各自维护块表；`allocate` 和 `alloc` 落在完全不同的实现上。
`uvcpp_enterprise_alloc()` **不是** `uvcpp_memory_pool` 的包装，把两边的读数放在一起
比会得出错误结论。它们的**线程缓存却是同一个**（见 §7）。

---

## 4. 最小用法

跟随全库策略（推荐）：

```cpp
#include <uvcpp/uvcpp_alloc.h>

void doc_use_facade() {
  // 打开池时走 uvcpp_enterprise_alloc，关掉时走 std::malloc，调用点两可
  char* buf = static_cast<char*>(uvcpp::uvcpp_alloc_bytes(4096));
  uvcpp::uvcpp_free_bytes(buf);
}
```

自己管一个池，并且管到对象：

```cpp
#include <expand/uvcpp_memory_pool.h>

struct doc_node {
  int v;
  explicit doc_node(int x) : v(x) {}
};

void doc_pool_object(uvcpp::uvcpp_memory_pool& pool) {
  std::unique_ptr<doc_node, uvcpp::pool_deleter<doc_node> > n =
      uvcpp::make_unique_from_pool<doc_node>(pool, 7);
  (void)n->v;
}
```

三个工厂函数（`src/expand/uvcpp_memory_pool.h:1069-1106`）：

| 函数 | 返回 | 说明 |
|---|---|---|
| `make_from_pool<T>(pool, args...)` | `T*` | 裸指针，失败抛 `std::bad_alloc` |
| `make_unique_from_pool<T>(pool, args...)` | `std::unique_ptr<T, pool_deleter<T>>` | 销毁时先 `~T()` 再 `deallocate` |
| `make_shared_from_pool<T>(pool, args...)` | `std::shared_ptr<T>` | **在 C++11 严格模式下编不过**，见 §9 |

第一次调用某个池的分配路径时，**记得先 `init_thread_cache()`** —— 不做的话这个线程
归还的块不会回到池里（见 §7 与 §9）。

---

## 5. 块大小档位

默认七档，每档一个上限（`src/expand/uvcpp_memory_pool.h:165-184`）：

| 档 | 默认上限 | 类型名 |
|---|---|---|
| 1 | 64 B | `MEMORY_TYPE_TINY` |
| 2 | 256 B | `MEMORY_TYPE_SMALL` |
| 3 | 1 KB | `MEDIUM_SMALL` |
| 4 | 4 KB | `MEMORY_TYPE_MEDIUM` |
| 5 | 16 KB | `MEDIUM_LARGE` |
| 6 | 64 KB | `MEMORY_TYPE_LARGE` |
| 7 | 256 KB | `EXTRA_LARGE` |
| — | 以上 | `MEMORY_TYPE_SUPER`（**不缓存**） |

```cpp
#include <expand/uvcpp_memory_pool.h>

uvcpp::memory_block_type doc_tier_of(size_t n) {
  uvcpp::memory_pool_config cfg;      // 默认档位
  return cfg.get_block_type(n);
}

size_t doc_capacity_of(uvcpp::memory_block_type t) {
  uvcpp::memory_pool_config cfg;
  return cfg.get_block_size(t);       // SUPER 档返回 0：它不缓存
}
```

档位边界是**闭区间**：`size <= tiny_block_size` 才是 TINY，所以 `allocate(64)`
落 TINY、`allocate(65)` 落 SMALL（`src/expand/uvcpp_memory_pool.h:217-241`）。

每块有 **32 字节固定头**（`pool_block_header`，`BLOCK_HEADER_SIZE = 32`，
`src/expand/uvcpp_memory_pool.h:79`）。小对象上这个开销要算进去 —— 分配 8 字节，实际占 96 字节
（64 档 + 32 头）。头里塞着 `size`、`flags`（类型 + in-use）与 `next`
（`src/expand/uvcpp_memory_pool.h:103-152`）。

改档位就是改 `memory_pool_config` 的成员，构造时传进 `pool.init(cfg)`。

---

## 6. 统计读数

```cpp
#include <expand/uvcpp_memory_pool.h>

void doc_pool_stats(uvcpp::uvcpp_memory_pool& pool) {
  uint64_t live = pool.active_allocations();
  uint64_t cached = pool.cached_blocks();
  bool leaked = pool.detect_leaks();            // 就是 live > 0

  uvcpp::memory_pool_stats s = pool.get_stats();
  double ratio = s.memory_usage_ratio();
  (void)cached; (void)leaked; (void)ratio;
  pool.print_stats();
}
```

`memory_pool_stats` 一共 11 个字段（`src/expand/uvcpp_memory_pool.h:278-296`），其中
`active_allocations`、`total_allocations`、`freed_bytes` 这些是**进程级累计**，
判增量要自己前后相减。`detect_leaks()` 不是扫描器，就是
`active_allocations() > 0`（`:862-864`）—— 拿它当泄漏判据的前提是你先知道自己本次
分配了几次。

`uvcpp_memory_pool_enterprise` 那边是另一套出参：

```cpp
#include <expand/uvcpp_page_heap.h>

void doc_enterprise_stats() {
  size_t total_allocated = 0, in_use = 0, free_spans = 0;
  uvcpp::uvcpp_memory_pool_enterprise::instance().get_stats(
      total_allocated, in_use, free_spans);
  (void)total_allocated; (void)in_use; (void)free_spans;
}
```

第三个出参**不叫它的名字**：`free_spans` 与 `total_spans` 在建/销毁 span 时同增同减，
两者是同一个数（`src/expand/uvcpp_page_heap.h:209-211` 自己写明了）。判"整段有没有归还"要看
`total_allocated`。

---

## 7. 线程本地缓存

两级缓存：**线程缓存**（无锁，7 条链，容量见 `src/expand/uvcpp_memory_pool.h:469-475`）→
**全局池**（每个档位一个 MPSC 队列，`:967-973`）→ **新建块**。`allocate()` 三条路径
依次是缓存命中、全局池、新建（`:1012-1033`）。

```cpp
#include <expand/uvcpp_memory_pool.h>

void doc_pool_thread_cache(uvcpp::uvcpp_memory_pool& pool) {
  pool.init_thread_cache();          // 不调它，缓存析构时不会归还
  void* p = pool.allocate(64);
  size_t in_cache = pool.thread_cache_size();
  pool.deallocate(p);
  pool.release_thread_cache();       // 手动把本线程缓存推回全局池
  (void)in_cache;
}
```

三个公开方法都在 `src/expand/uvcpp_memory_pool.h:879-892`：`init_thread_cache()`、
`release_thread_cache()`、`thread_cache_size()`。

**这一节最重要的一句：线程缓存是函数内静态的**
（`inline static thread_local_cache &get_thread_cache() { static thread_local thread_local_cache cache; return cache; }`，
`src/expand/uvcpp_memory_pool.h:872-875`）—— 它是**全进程一个**，不是每池一个。同一个线程里
两个池交替分配/释放，块会在同一个缓存里混起来；档位相同就**看不出来**，档位不同时
`pop()` 拿到的是别的类型的块。`deallocate` 按块头里的类型（`:1044-1045`）走，所以
不会崩，但"这个指针是哪个池给的"只有调用方知道。**别在线程里混用两个池。**

---

## 8. 生命周期与关闭

```cpp
#include <expand/uvcpp_memory_pool.h>

void doc_pool_lifecycle() {
  uvcpp::uvcpp_memory_pool pool;     // 默认构造即默认配置
  if (!pool.init()) return;

  pool.warmup();                     // 预填本线程缓存
  void* p = pool.allocate(4096);
  pool.reset();                      // 只清计数，不还内存
  pool.deallocate(p);
  pool.shutdown();
}
```

| 方法 | 做什么 |
|---|---|
| `init()` / `init(cfg)` | 建 7 个全局队列、校验配置；重复调用是幂等的（`src/expand/uvcpp_memory_pool.cpp:549-551` 直接 `return true`） |
| `warmup()` | 预填本线程缓存 |
| `reset()` | **只清计数**，与 `reset_stats()` 逐行相同 |
| `shutdown()` | 关队列、换出节点、调 `dealloc_func_` |
| `is_initialized()` | 读一个原子标志（`src/expand/uvcpp_memory_pool.h:815-817`） |

**`reset()` 不还内存。** 它的实现（`src/expand/uvcpp_memory_pool.cpp:245-262`）就是 15 个
`.store(0)`，和 `reset_stats()`（`:264-281`）**逐行相同** —— 两个名字一个行为。
真正归还内存的是 `shutdown()`，以及线程缓存析构那条路（需 §7 的回调设好）。

**销毁一个池最少 7 ms。** 7 个 `mpsc_queue` 每个的析构都调 `shutdown()`
（`src/expand/uvcpp_memory_pool.h:327-329`），而 `shutdown()` 里有一句**无条件**的
`sleep_for(1ms)`（`:405`）。别在热路径上反复建销池 —— 这是固定开销，不是偶发。

---

## 9. 头注释与实现对不上

这一节是这个模块最该读的部分。逐条都对着源码核过。

### `reset()` 与 `reset_stats()` 是同一个函数

`src/expand/uvcpp_memory_pool.cpp:245-262` 与 `:264-281` 两段函数体**逐行相同**，都是把 8 个
`stats_` 字段 + 7 个 `global_*_count_` 置 0。名字承诺 `reset()` 重置池，实际只清读数。

### `allocate_aligned()` 不按 `align` 对齐地址

```cpp
// doc-snippet: fragment — 照着 uvcpp_memory_pool.h 抄的函数体，用来说明 align 只改了 size
inline void* uvcpp_memory_pool::allocate_aligned(size_t size, size_t align) {
    if (size == 0) return nullptr;
    size_t aligned_size = (size + align - 1) & ~(align - 1);
    return allocate(aligned_size);
}
```

`src/expand/uvcpp_memory_pool.h:1035-1039`。`align` **只用来把 `size` 向上取整**，返回值来自
`allocate()`，地址是否对齐取决于块头偏移，**与 `align` 无关**。要真对齐得自己再
`std::align` 一次，或者用 `uvcpp_page_allocator`（它的页天然对齐）。

### `push()` 返回 `true` 时，SUPER 档的块被丢掉

`push()` 的返回值有两种含义，第二种是「丢掉了」—— 头注释里已写明
（`src/expand/uvcpp_memory_pool.h:671-677`）。函数体第一件事是：

```cpp
// doc-snippet: fragment — uvcpp_memory_pool.h 里 push() 的前几行摘录
memory_block_type type = block->get_type();
block->set_in_use(false);
block->next = nullptr;

size_t idx = static_cast<size_t>(type);
if (idx >= 7) return true;
```

（`src/expand/uvcpp_memory_pool.h:681-686`）—— `idx >= 7` 就是 SUPER 档，**返回 true 却没放进
任何链表**（`next` 刚被清成 `nullptr`，块从此无人引用）。而 `deallocate()` 只在
`push()` 返回 `false` 时才走 `push_to_global_pool`（`:1052-1055`）⇒
**超过 256 KB 的块在 `deallocate` 时被静默丢掉**。

要紧的是**同一时刻它还被记成"已释放"**：`deallocate()` 随后无条件调
`update_dealloc_stats()`（`src/expand/uvcpp_memory_pool.h:1057`），把 `active_allocations`
减一、`freed_bytes` 加上这块大小、`total_deallocations` 加一（`:951-953`）。所以
`stats()` 与 `detect_leaks()` **都显示正常** —— 这块内存既没还回池、也再找不回来，而账面
上它是干净的。查这类问题不能信计数器。

### 线程缓存的"析构自动归还"有个前提

`thread_local_cache` 析构时**会**调 `release_all()`（`src/expand/uvcpp_memory_pool.h:496-497`），
而它的第一句就是：

```cpp
// doc-snippet: fragment — release_all() 的首句摘录，单行
if (!pool_ptr_ || !release_func_) return;
```

（`src/expand/uvcpp_memory_pool.h:544-545`）默认构造的 `thread_local_cache` 两个成员都是
`nullptr`（`:482`、`:486`），**所以没调过 `init_thread_cache()` 就什么都不还** ——
`release_all()` 连链表头和计数都不清就直接返回了。这是个静默泄漏，不是延迟归还。
头注释两处（`:428-431`、`:496-497`）现在都写明了这个前提。

### `memory_usage_ratio()` 会无符号下溢

```cpp
// doc-snippet: fragment — memory_usage_ratio() 的函数体摘录
double memory_usage_ratio() const {
    if (allocated_bytes == 0) return 0.0;
    return static_cast<double>(allocated_bytes - freed_bytes)
         / static_cast<double>(allocated_bytes);
}
```

（`src/expand/uvcpp_memory_pool.h:291-295`）两个操作数都是 `uint64_t`。而这两个计数器**口径不同**：

- `allocated_bytes` 只在**新建块**时加（`src/expand/uvcpp_memory_pool.cpp:305`、`:338` 两处）
- `freed_bytes` 在**每次** `deallocate` 时加（`src/expand/uvcpp_memory_pool.h:950-954`）

缓存命中的分配不进 `allocated_bytes`，但它的释放照样进 `freed_bytes`。自由块一多，
`freed > allocated`，`uint64_t` 减法**回绕成天文数字**，比值算出来是个荒谬的大数
（或 `inf`）。这个读数是"分配了多少 / 又还了多少"的混合口径，不是使用率。

### `make_shared_from_pool` 用的是 C++14 语法，而构建声明 C++11

```cpp
// doc-snippet: fragment — make_shared_from_pool() 里那条 return 的摘录
return std::shared_ptr<T>(ptr, [pool = &pool](T* p) {
    p->~T();
    pool->deallocate(p);
});
```

（`src/expand/uvcpp_memory_pool.h:1102`）lambda 初始化捕获是 C++14 特性，而
`CMakeLists.txt:85-86` 写的是 `set(CMAKE_CXX_STANDARD 11)` +
`CMAKE_CXX_STANDARD_REQUIRED ON`。实测：

| 编译方式 | 结果 |
|---|---|
| `g++ -std=c++11` | `warning: lambda capture initializers only available with '-std=c++14'`，rc=0 |
| `g++ -std=c++11 -pedantic-errors` | **error**，rc=1 |
| MSVC `/std:c++14` | 通过 |

库自己编得过（GCC 默认给的是 `gnu++11`，只警告；MSVC 那侧 CMake 把 C++11 映射成
`/std:c++14`），所以一直没被发现。**开了 `-pedantic-errors` / `-Werror` 的消费者会
编不过**，而你自己的代码只是包含这个头、并不调用这个模板也可能中招 —— GCC 报错
落在头文件里，不是你的调用点。

### 这一模块里**诚实**的头注释

对照上面几条，下面这些注释是准确的、值得信：

- `src/expand/uvcpp_memory_pool_span.h:3` 自标 `WIP/Experimental`
- `get_pressure_level()` 写明了"恒为 `none`，而这个 `none` 不表示没有压力"
  （`src/expand/uvcpp_page_heap.h:226-233`）
- `trigger_gc()` 写明是空操作、`g_span_free_list` 全仓没有写入点（`:235-241`）
- `set_pressure_callback()` 写明"存下来但永不调用"（`:248-255`）
- `enable_huge_page()` 写明 Windows 分支忽略这个标志（`:257-265`）
- `get_stats()` 的 `free_spans` 写明它与 `total_spans` 是同一个数（`:209-212`）

---

## 10. 典型坑

**没调 `init_thread_cache()` 就是泄漏，不是延迟归还。** 见 §9。这个坑最阴的地方是
"没调"这件事**不会报错** —— `allocate` / `deallocate` 一切正常，只有进程退出时那些块
不回到池里。

**SUPER 档的自由块直接消失。** 见 §9。默认档位下 `> 256 KB` 就走这条路
（`extra_large_block_size` 默认 `262144`，`src/expand/uvcpp_memory_pool.h:184`）。想避开就把
`extra_large_block_size` 调到你的最大对象之上 —— 但那一档也就此不再缓存了。

**线程缓存是全进程共享的**，所以同一个线程里混用两个池会串。见 §7。

**`allocate()` 不检查状态。** `uvcpp_memory_pool::allocate()`（`:992-1013`）里没有
`is_initialized()` 这一句，也没有 `shutdown()` 之后的守卫。忘了 `init()` 不会报错，
按默认配置照常分配；`shutdown()` 之后再分配同样不报错。`is_initialized()` 得你自己问。

**每块 32 字节的头。** 见 §5。小对象场景先算这笔账。

**销毁一个池最少 7 ms**，7 个队列各一句 `sleep_for(1ms)`。见 §8。

**`uvcpp_memory_pool` 与 `uvcpp_memory_pool_enterprise` 的读数不可比。** 见 §3。

**两个池的线程缓存是同一个。** §7 说了一次，这里再说一次：它由函数内静态决定，
不是你构造了几个池。

---

## 11. 没做的（如实列出）

- **`max_total_memory` 是空壳。** 结构体里有这个字段、注释写着"0 = 无限制"
  （`src/expand/uvcpp_memory_pool.h:188`），但**全仓没有一处读它** —— 那一行就是它唯一的出现。
  设了不会生效，也不会报警告。
- **压力检测整套是保留接口。** `get_pressure_level()` 恒返回 `none`，
  `trigger_gc()` 是空函数，`set_pressure_callback()` 存下的指针永不调用。
  三者头注释都自己标注了 `reserved interface`（`src/expand/uvcpp_page_heap.h:226-255`）——
  **别把它们当可用的旋钮**。
- **大页只在 Linux 上真的试。** `enable_huge_page(true)` 给 `mmap` 加 `MAP_HUGETLB`，
  还需系统预先预留 hugetlb 页，否则 `mmap` 直接失败；Windows 分支的 `VirtualAlloc`
  不带 `MEM_LARGE_PAGES`，这个标志被忽略。返回 `true` 只表示"标志记下了"
  （`src/expand/uvcpp_page_heap.h:257-265`）。
- **NUMA 只有 `set_numa_node()` / `get_numa_node()` 两个存取器**，没有任何分配路径
  读它（`:277`、`src/expand/uvcpp_page_heap.cpp:1276-1279`）。
- **`uvcpp_memory_pool_span` 是实验品**，`try_merge()` 是空函数
 （`src/expand/uvcpp_memory_pool_span.cpp:145-148`）。除 `alloc` / `free_mem` 之外的能力都没实现。
- **`allocate_aligned()` 不做地址对齐。** 见 §9。
- **没有跨池检查。** 块里没有归属标记，把 A 池的指针交给 B 池的 `deallocate`
  不会当场发现；档位不同时行为不可预期。

---

相关文档：[低层指南](./lowlevel-guide.md)（`uvcpp_alloc.h` 所在的 `uvcpp/` 那一层）、
[构建指南](./build-guide.md)（`UVCPP_BUILD_EXPAND` 与其余开关的全表）、
[性能测试](./benchmark.md)（池开关对读数的影响）、
[RELEASE.md](../RELEASE.md)（默认关这件事的来龙去脉）。
