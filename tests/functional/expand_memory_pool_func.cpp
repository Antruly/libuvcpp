/**
 * @file tests/functional/expand_memory_pool_func.cpp
 * @brief 内存分配回归测试（UVCPP_ENABLE_MEMORY_POOL 开/关都跑）
 *
 * 覆盖一个曾经让整个进程死循环的缺陷：central_cache::pop() 的 span 链表
 * 遍历写成「一旦离开表头就回到表头重来」，不是终止条件。4KB 以上的 size
 * class 每个 span 只放得下一个块，于是链表一旦长出第二个 span，遍历就会在
 * 「表头 -> 第二个 -> 表头」之间空转，永远走不到 nullptr —— 实测 8KB 的
 * HTTP 响应会让进程挂死。
 *
 * 注意触发条件是「链表里同时存在两个 span」，而这只在**第三个**同类块
 * 同时在手时才会出现（前两个块各占一个 span，都要走了才轮得到第三个）。
 * 所以 Test 1 必须并发持有 4 块 —— 只并发 2 块时 bug 并不触发。
 *
 * 这里只走公开接口（uvcpp_buf / uvcpp_alloc），所以内存池关闭时同样有效：
 * 那时用的是 malloc/realloc，逻辑上就该通过。
 */
#include <iostream>
#include <string>
#include <cstring>
#include <cstdio>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>
#include <uvcpp/uvcpp_alloc.h>

using namespace uvcpp;

// =========================================================================
// Test 1: 同一 size class 同时持有多个大块（回归：曾死循环）
//
// 4KB 以上每个 span 只容纳一个块，所以每多要一块就要多开一个 span。链表长到
// 第三个 span 时，才会出现「表头已空、链表里还有下一个」这个曾经死循环的
// 状态 —— 两块还不够，必须至少三块同时在手。
// =========================================================================
static bool test_many_large_blocks() {
  const size_t sizes[] = {8192, 16384, 65536, 131072, 262144};
  const size_t kCount = 4;  // 必须 > 2

  for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); ++si) {
    const size_t sz = sizes[si];
    const unsigned char tag = static_cast<unsigned char>(0xA0 + si);

    uvcpp_buf bufs[kCount];
    for (size_t i = 0; i < kCount; ++i) {
      bufs[i].resize(sz);
      if (bufs[i].size() != sz) {
        fprintf(stderr, "  [dbg] size=%zu idx=%zu got=%zu\n", sz, i,
                bufs[i].size());
        return false;
      }
      // 每块写不同的字节，用来验证块之间不重叠
      std::memset(bufs[i].get_data(), tag + (int)i, bufs[i].size());
    }

    // 全部写完再统一校验：若两个块重叠，后写的会破坏先写的
    for (size_t i = 0; i < kCount; ++i) {
      const char want = static_cast<char>(tag + (int)i);
      const char* p = bufs[i].get_const_data();
      if (p[0] != want || p[sz / 2] != want || p[sz - 1] != want) {
        fprintf(stderr, "  [dbg] size=%zu idx=%zu overlapped\n", sz, i);
        return false;
      }
    }
  }
  return true;
}

// =========================================================================
// Test 2: 反复分配/释放大块，跨越多个 size class 边界
// =========================================================================
static bool test_large_block_churn() {
  // 4096 是 size class 的分界（4096 及以下一个 span 放多块，以上只放一块）
  const size_t sizes[] = {4096, 5120, 8192, 12288, 16384, 32768, 65536};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    for (int round = 0; round < 3; ++round) {
      uvcpp_buf x;
      uvcpp_buf y;
      x.resize(sizes[i]);
      y.resize(sizes[i]);
      if (x.size() != sizes[i] || y.size() != sizes[i]) return false;
      x.get_data()[0] = 'x';
      y.get_data()[0] = 'y';
      if (x.get_const_data()[0] != 'x' || y.get_const_data()[0] != 'y') {
        return false;
      }
    }
  }
  return true;
}

// =========================================================================
// Test 3: 缓冲区分次增长（客户端收大响应的真实路径）
//
// uvcpp_buf::resize 每次按新长度重新分配，这条路径给 HTTP 客户端的
// body_buf_、HTTP 服务端的响应缓冲反复使用。
// =========================================================================
static bool test_buf_growth() {
  uvcpp_buf buf;
  size_t total = 0;
  // 逐步长到 64KB，跨越 4KB 边界很多次
  for (size_t chunk = 1024; chunk <= 8192; chunk += 1024) {
    for (int i = 0; i < 4; ++i) {
      buf.append_data("0123456789abcdef", 16);
      total += 16;
      if (buf.size() != total) return false;
    }
  }
  // 追加的数据必须完整保留
  if (buf.size() != total) return false;
  if (buf.get_const_data()[0] != '0') return false;
  if (buf.get_const_data()[total - 1] != 'f') return false;
  return true;
}

// =========================================================================
// Test 4: 直接走 uvcpp_alloc_bytes 的裸分配
// =========================================================================
static bool test_raw_alloc() {
  void* p1 = uvcpp_alloc_bytes(8192);
  void* p2 = uvcpp_alloc_bytes(8192);
  if (p1 == nullptr || p2 == nullptr) return false;
  if (p1 == p2) return false;

  std::memset(p1, 0x11, 8192);
  std::memset(p2, 0x22, 8192);

  bool ok = (static_cast<unsigned char*>(p1)[0] == 0x11) &&
            (static_cast<unsigned char*>(p2)[0] == 0x22) &&
            (static_cast<unsigned char*>(p1)[8191] == 0x11) &&
            (static_cast<unsigned char*>(p2)[8191] == 0x22);

  uvcpp_free_bytes(p1);
  uvcpp_free_bytes(p2);

  // 释放后再分配，应当能复用
  void* p3 = uvcpp_alloc_bytes(8192);
  if (p3 == nullptr) return false;
  uvcpp_free_bytes(p3);
  return ok;
}

// =========================================================================
// Test 5: 缓冲区分次倍增（回归：旧实现 resize 时按「新大小」从旧块复制）
//
// 旧 uvcpp_realloc_bytes 写的是 memcpy(新, 旧, 新大小)，而旧块只有旧大小那么
// 大 —— 每次增长都从旧块后面读越界。增长跨过 span 映射边界时直接访问违例：
// 实测客户端收 128KB 响应崩在 uvcpp_buf::resize 的 memcpy 上。
// 这里必须逐字节校验旧内容被完整搬过来（不是只校验长度）。
// =========================================================================
static bool test_buf_doubling_growth() {
  uvcpp_buf buf;
  buf.resize(65536);
  if (buf.size() != 65536) return false;
  std::memset(buf.get_data(), 0x5A, buf.size());

  // 65536 -> 131072：旧实现会从 64KB 块的后面再读 64KB
  buf.resize(131072);
  if (buf.size() != 131072) {
    fprintf(stderr, "  [dbg] size=%zu\n", buf.size());
    return false;
  }
  for (size_t i = 0; i < 65536; ++i) {
    if (buf.get_const_data()[i] != (char)0x5A) {
      fprintf(stderr, "  [dbg] content lost at %zu\n", i);
      return false;
    }
  }
  // 新增部分是零填充
  if (buf.get_const_data()[65536] != 0) {
    fprintf(stderr, "  [dbg] tail not zero\n");
    return false;
  }

  // 继续倍增到 1MB，跨越多个 size class 与「大对象」阈值(256KB)。
  // 只有最初那 64KB 是 0x5A，其余是 resize 的零填充；每次增长都必须原样
  // 搬走这 64KB，并把新扩出来的区间清零。
  const size_t kFilled = 65536;
  size_t prev = 131072;
  for (size_t target = 262144; target <= 1048576; target *= 2) {
    buf.resize(target);
    if (buf.size() != target) {
      fprintf(stderr, "  [dbg] target=%zu size=%zu\n", target, buf.size());
      return false;
    }
    for (size_t i = 0; i < kFilled; ++i) {
      if (buf.get_const_data()[i] != (char)0x5A) {
        fprintf(stderr, "  [dbg] target=%zu content lost at %zu\n", target, i);
        return false;
      }
    }
    for (size_t i = prev; i < target; ++i) {
      if (buf.get_const_data()[i] != 0) {
        fprintf(stderr, "  [dbg] target=%zu new region not zeroed at %zu\n",
                target, i);
        return false;
      }
    }
    prev = target;
  }
  return true;
}

int main() {
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"many_large_blocks", test_many_large_blocks},
    {"large_block_churn", test_large_block_churn},
    {"buf_growth", test_buf_growth},
    {"buf_doubling_growth", test_buf_doubling_growth},
    {"raw_alloc", test_raw_alloc},
  };
  for (const auto& t : tests) {
    std::cout << "[expand_memory_pool] " << t.name << std::endl;
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << std::endl;
    ok = r && ok;
  }
  std::cout << "[expand_memory_pool] " << (ok ? "ALL PASS" : "FAIL") << std::endl;
  return ok ? 0 : 2;
}
