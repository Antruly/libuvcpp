/**
 * @file tests/functional/buf_share_handle_func.cpp
 * @brief 变体表**按句柄**存/取（#17 第 ④ 步）之后必须成立的那几条契约。
 *
 * 第 ④ 步把压缩变体表里那份字节从「自己持有的一份拷贝」换成了「与响应共用的
 * 一个句柄」（`std::shared_ptr<const std::string>`）。于是**同一块字节**上同时
 * 挂着两个持有者：表里那一条，和正在往外发的那个响应。这个文件钉的就是这件事
 * 带来的三条契约 —— 它们都不是"性能"，是**正确性**：
 *
 *  1. **命中那条路上，响应体与表里那条就是同一份字节**（指针相同、持有者数对得
 *     上）。这条是 ④ 的全部意义所在：所谓"共享"如果背后又拷了一份，那仍然是
 *     拷贝，热的压缩响应就还是白付一次整份体的拷贝。判据用**指针相同**，不用
 *     "内容相等" —— 后者对"到底拷没拷"零信息量。
 *
 *  2. **谁写响应体，谁自己拿到一份私有的**，表里那条逐字节不变。`clear()` 紧接
 *     `clone()` 是响应对象复用的**正常**序列（不是错误），所以这条不能靠断言、
 *     只能靠 `uvcpp_buf` 的静默物化。它是 ④ 最容易踩的地方：表与响应现在是同一
 *     个 string，写的人要是拿到了可写指针，坏掉的是**之后每一次命中** —— 而且
 *     是"偶尔发错字节"这种最难查的坏法。防线是两样：表这边是
 *     `shared_ptr<const std::string>`（拿不到可写指针），写这边是物化。
 *     所以这里既要钉"表没被写坏"，也要钉"物化确实发生了、而且只发生一次"
 *     （`share_discard_count()` 恰好 +1）。
 *
 *  3. **谁先走都不影响另一份。** 淘汰（字节/条数上限）随时可能发生，而"表里那
 *     一条被淘汰时响应还没发完"在 ④ 之前**不可能**出问题 —— 那时两边各持一份
 *     拷贝。现在两者共用同一块字节，所以这一条是句柄**必须**提供的东西：它靠
 *     引用计数把字节撑住，谁先松手都不动内容。反过来（响应先走、表里还在）同理。
 *
 * 全是单线程的纯缓冲语义，起不了连接也不需要 —— 契约本身就活在 `uvcpp_buf`
 * 这一层，跑真连接只会把同一件事测得更慢。
 */
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "uvcpp/uvcpp_buf.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

// =========================================================================
// G1：命中那条路 —— 表与响应是**同一份字节**
// =========================================================================
void case_same_bytes() {
  std::shared_ptr<const std::string> made =
      std::make_shared<const std::string>("gzip-payload");
  const char* p = made->data();

  // 模拟 ④ 的两条路：压出来之后响应先接住句柄，表里存的是**同一个**句柄。
  uvcpp_buf resp;
  resp.share(made);
  uvcpp_buf table;
  table.share(made);

  check(made.use_count() == 3, "G1: 三个持有者 —— 本地 1 + 表 1 + 响应 1");
  check(resp.get_const_data() == p, "G1: 响应指的就是那份字节（不是拷来的）");
  check(table.get_const_data() == p, "G1: 表里指的也是同一份（指针相同才是没拷）");
  check(resp.get_const_data() == table.get_const_data(),
        "G1: 两边一模一样 —— ④ 省掉的就是这两边之间那次拷贝");

  // `shared_ref()` 递出来的必须是**同一个**句柄（引用计数上去），而不是一份拷贝。
  {
    std::shared_ptr<const std::string> back = resp.shared_ref();
    check(back == made, "G1: shared_ref() 递回来的是同一个句柄");
    check(made.use_count() == 4, "G1: 多了一个持有者（是引用计数，不是新的一份字节）");
  }

  check(resp.is_shared() && table.is_shared(), "G1: 两边都是共享视图");
  check(resp.size() == made->size() && table.size() == made->size(),
        "G1: 长度都取自那份串");
  check(resp.to_string() == "gzip-payload", "G1: 读出来就是那份压缩产物");
}

// =========================================================================
// G2：写响应体 —— 表里那条**逐字节不变**（④ 最容易踩的地方）
//
// 分三种写法，因为它们的代价**不一样**，"该记几次账"也跟着不一样：
//   a) 直接写共享视图（`clone_data` 一步到位）：共享的那几个字节必须先被拷成
//      自有的才谈得上改写 —— 一次真金白银的物化，计数 +1；
//   b) `clear()` 之后再写（**响应对象复用的正常序列**）：clear 不需要写，只是
//      把引用放掉（`free_own()`），新内容随后从别处来 —— 一个字节都没白拷，
//      所以计数**不动**；
//   c) 往共享视图上**追加**（"有人往响应体里塞了个 banner"的形状）：原件必须先
//      拷出来才能接在后面，所以和 a 一样 +1。
// 三种写法都必须让表里那条纹丝不动 —— 那才是 ④ 的契约。（b 那条"不记账"不是
// 抠字眼：判据写成 +1 的话，这条用例就变成在钉一个**错的**账。）
// =========================================================================
void case_write_response_keeps_table() {
  const std::string payload = "gzip-payload";

  // ---- a) 直接写：一次真物化 ----
  {
    std::shared_ptr<const std::string> made =
        std::make_shared<const std::string>(payload);
    const char* p = made->data();

    uvcpp_buf table;
    table.share(made);
    uvcpp_buf resp;
    resp.share(made);

    const uint64_t before = uvcpp_buf::share_discard_count();
    resp.clone_data("replaced", 8);

    check(*made == payload, "G2a: 写响应没有动到那份串");
    check(table.to_string() == payload, "G2a: 表里那一条逐字节不变");
    check(table.get_const_data() == p, "G2a: 表里那条还在原来那块字节上");
    check(resp.get_const_data() != p, "G2a: 响应拿到了自己私有的一块");
    check(resp.to_string() == "replaced", "G2a: 响应读到的是新内容");
    check(!resp.is_shared(), "G2a: 写完不再是共享视图");
    check(made.use_count() == 2, "G2a: 物化时放掉了响应那份引用（只剩本地 + 表）");
    check(uvcpp_buf::share_discard_count() == before + 1,
          "G2a: 一次真物化被记成一次「共享又丢」");
  }

  // ---- b) clear() 之后再写：不记账（没有白拷） ----
  {
    std::shared_ptr<const std::string> made =
        std::make_shared<const std::string>(payload);
    const char* p = made->data();

    uvcpp_buf table;
    table.share(made);
    uvcpp_buf resp;
    resp.share(made);

    const uint64_t before = uvcpp_buf::share_discard_count();
    resp.clear();
    check(made.use_count() == 2, "G2b: clear() 放掉了响应那份引用（只剩本地 + 表）");
    resp.clone_data("replaced", 8);

    check(*made == payload, "G2b: 写响应没有动到那份串");
    check(table.to_string() == payload, "G2b: 表里那一条逐字节不变");
    check(table.get_const_data() == p, "G2b: 表里那条还在原来那块字节上");
    check(resp.to_string() == "replaced", "G2b: 响应读到的是新内容");
    check(uvcpp_buf::share_discard_count() == before,
          "G2b: clear() 不写字节，所以这一次**不该**记账（记的是白拷，这里一个字节都没拷）");
  }

  // ---- c) 追加：原件先被拷出来，然后接在后面 ----
  {
    std::shared_ptr<const std::string> made =
        std::make_shared<const std::string>(payload);
    const char* p = made->data();

    uvcpp_buf table;
    table.share(made);
    uvcpp_buf resp;
    resp.share(made);

    const uint64_t before = uvcpp_buf::share_discard_count();
    resp.append_data("!!", 2);

    check(resp.to_string() == payload + "!!", "G2c: 追加之后 = 原件 + 追加");
    check(table.to_string() == payload, "G2c: 表里那一条还是原件（追加没串过去）");
    check(table.get_const_data() == p, "G2c: 表里那条还在原来那块字节上");
    check(*made == payload, "G2c: 那份串本身也没变（它是 const，改不了）");
    check(uvcpp_buf::share_discard_count() == before + 1,
          "G2c: 追加要先拷出原件，记一次「共享又丢」");
  }
}

// =========================================================================
// G3：淘汰先发生（表里那条先走）—— 正在发的响应还得是完好的
//
// ④ 之前这一条**不可能**出问题（两边各持一份拷贝）；换句柄之后它成了
// 引用计数必须撑住的东西。判据里连"指针没变"一起钉：物化也算拷贝，这里
// 一次都不该有。
// =========================================================================
void case_evict_before_response() {
  std::shared_ptr<const std::string> made =
      std::make_shared<const std::string>("evicted-target");
  const char* p = made->data();

  uvcpp_buf table;
  table.share(made);
  uvcpp_buf resp;
  resp.share(made);

  made.reset();  // 真实现里外头没人持着 —— 只剩表和响应
  {
    std::shared_ptr<const std::string> both = table.shared_ref();
    check(both.use_count() == 3,
          "G3: 外部引用放掉后只剩表 + 响应（加这里取出来这一份）");
  }

  table.clear();  // 淘汰：表里那一条被 erase（这里等价于 clear）
  {
    std::shared_ptr<const std::string> only = resp.shared_ref();
    check(only.use_count() == 2, "G3: 只剩响应那一个持有者（表那份已经放了）");
  }
  check(resp.get_const_data() == p,
        "G3: 淘汰之后响应还在原来那块字节上（一次拷贝都没有）");
  check(resp.to_string() == "evicted-target",
        "G3: 淘汰表里那一条，正在发的响应仍然完好");

  resp.clear();
}

// =========================================================================
// G4：反过来 —— 响应先走，表里那条还得完好
// =========================================================================
void case_response_first_keeps_table() {
  std::shared_ptr<const std::string> made =
      std::make_shared<const std::string>("kept-by-table");
  const char* p = made->data();

  uvcpp_buf table;
  table.share(made);
  uvcpp_buf resp;
  resp.share(made);

  made.reset();
  resp.clear();  // 这条响应发完（或被复用）了

  check(table.get_const_data() == p,
        "G4: 响应走了之后表里那条还在同一块字节上");
  check(table.to_string() == "kept-by-table", "G4: 表里那一条逐字节完好");
  {
    std::shared_ptr<const std::string> only = table.shared_ref();
    check(only.use_count() == 2, "G4: 只剩表那一个持有者");
  }
}

// =========================================================================
// G5：空句柄/空手柄的退化
//
// ④ 的实现里，压缩产出长度为 0 时会 `share()` 一个**空的**共享串。它必须与
// `clear()` 完全一致（空缓冲、不是共享视图）—— 否则 `is_shared()` 为真而
// 长度为 0 的"空共享"会在别处（比如"命中就把句柄递出去"）变成另一种东西。
// =========================================================================
void case_empty_share() {
  {
    std::shared_ptr<const std::string> empty =
        std::make_shared<const std::string>();
    uvcpp_buf b;
    b.clone_data("old-bytes", 9);
    b.share(empty);
    check(!b.is_shared(), "G5: 空的共享串 = 不是共享视图");
    check(b.size() == 0 && b.get_const_data() == nullptr,
          "G5: 空的共享串 = 与 clear() 一致");
  }
  {
    std::shared_ptr<const std::string> null_handle;
    uvcpp_buf b;
    b.clone_data("old-bytes", 9);
    b.share(null_handle);
    check(!b.is_shared() && b.size() == 0,
          "G5: 空手柄也是 clear()（旧的那份要放掉）");
  }
}

}  // namespace

int main() {
  // 计数器是进程级的：这里只关心"某一段里恰好 +1"，先清零。
  uvcpp_buf::reset_share_discard_count();

  case_same_bytes();
  case_write_response_keeps_table();
  case_evict_before_response();
  case_response_first_keeps_table();
  case_empty_share();

  if (g_failures == 0) {
    std::cout << "[buf_share_handle] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[buf_share_handle] FAIL (" << g_failures << " checks failed)"
            << std::endl;
  return 2;
}
