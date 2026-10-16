/**
 * @file tests/functional/yyjson_build_func.cpp
 * @brief `yyjson/uvcpp_json_doc.h` 的**构造侧**测试（`uvcpp_json_builder` /
 *        `uvcpp_json_node` / `uvcpp_json_key`）。
 *
 * ## 这一条测的是什么
 *
 * 姊妹文件 `yyjson_doc_func.cpp` 测的是**解析侧**（读一份文档）。这一条测另一个
 * 方向：**从零建一棵树，再用 yyjson 自己的序列化器写出去**。模块的公开面上这两侧
 * 是并列的（两套句柄、两套 API），所以用例也是两条 —— 但**判据的口径必须逐条
 * 相同**，否则"同一份数据，解析着读"与"建完读"会给出不同答案。第 ① 组就是拿
 * "建树 → 写 → 解析 → 取值"把两边钉在一起的。
 *
 * 分组（每条判据都是可失败的断言，不是"看着对"）：
 *
 *   ① 各类型的往返：建 → 写 → 解析 → 取值，值必须原样回来
 *   ② 键与字符串值的**拷贝**语义：临时对象当键、含 `\0` 的键、空键
 *   ③ `add_copy` / `push_copy`：源文档析构之后拷进来的子树仍然正确
 *   ④ **句柄稳定性**：取一个句柄 → 往同一容器再加一万个元素 → 旧句柄仍然有效
 *   ⑤ 记账：空树 0、`reset()` 回 0、**移动之后不重复计数**（错的话 `reset` 会崩）
 *   ⑥ 键序 = **插入序**（第 1 处不同；nlohmann 是按键排序的）
 *   ⑦ 与 nlohmann 逐字节对照（键序落在两边都同意的那一批上时）
 *   ⑧ **失败路径**：不抛异常、不改动已经建好的部分、`last_error()` 说得清
 *
 * ## 为什么第 ④ 组必须有
 *
 * "往容器里追加不搬已经发出去的节点"是 yyjson 可变 DOM 的一条**结构性假设**，
 * 整个构造侧的可组合性都压在它上面（子节点是循环链表，追加只改 `next`）。它是
 * 上游的实现细节，不是文档化的保证 —— 所以只能由我们这边断言。哪天上游改了，
 * 我们要红在自己的用例上，而不是红在某个用户的内存里。
 *
 * ## 命名
 *
 * 文件名含 `yyjson` ⇒ `tests/functional/CMakeLists.txt` 里那条
 * `/[^/]*yyjson[^/]*\.cpp$` 过滤器会在 `UVCPP_ENABLE_YYJSON=OFF` 时把它摘掉
 * （关掉模块时"没测"必须表现为**用例不存在**，不是表现为通过）。名字**不带**
 * `web_app_` 前缀：本模块在核心层，不需要 webapp。
 */
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_json_scan.h>
#include <yyjson/uvcpp_json_doc.h>

// nlohmann 只在 webapp 编进来时才存在（那才是 fetch 它的条件）。
#if UVCPP_WEBAPP_ENABLE
#include <webapp/uvcpp_web_json.h>
#endif

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq_s(const std::string& got, const std::string& want,
                const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_eq_ll(long long got, long long want, const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_eq_u(size_t got, size_t want, const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_eq_d(double got, double want, const std::string& what) {
  if (!(got == want)) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_st(json_write_status got, json_write_status want,
              const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what
              << "\n         期望: " << json_write_status_name(want)
              << "\n         实际: " << json_write_status_name(got) << std::endl;
    ++g_failures;
  }
}

void check_st(json_doc_status got, json_doc_status want,
              const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what
              << "\n         期望: " << json_doc_status_name(want)
              << "\n         实际: " << json_doc_status_name(got) << std::endl;
    ++g_failures;
  }
}

#if UVCPP_WEBAPP_ENABLE
void must_ok_nl(json_status st, const std::string& what) {
  if (st != json_status::OK) {
    std::cerr << "  [FAIL] " << what << "\n         期望: OK\n         实际: "
              << json_status_name(st) << std::endl;
    ++g_failures;
  }
}
#endif

/// @brief 把一个节点写出去；状态不是 OK 就当场记一笔并返回空串。
///
/// 判据不能只看状态码 —— "返回 OK 但什么都没写"同样会让后面所有比对空转成绿，
/// 所以非空那一条也在这儿断言。
std::string write_node(const uvcpp_json_node& n, const std::string& what) {
  std::string out;
  check_st(uvcpp_json_write(n, out), json_write_status::OK,
           what + "：uvcpp_json_write 的状态");
  check(!out.empty(), what + "：写出来的不是空串");
  return out;
}

// =========================================================================
// 第 ① 组：各类型的往返（建 → 写 → 解析 → 取值）
// =========================================================================

void group_roundtrip() {
  uvcpp_json_builder b;
  uvcpp_json_node root = b.make_object();
  check(root.valid(), "1.1 游离的空对象是有效的");
  check(root.is_object(), "1.1 它的类型是 object");
  check(root.size() == 0, "1.1 刚建出来是空的");
  check(b.set_root(root), "1.1 挂成根");
  check(b.root().valid(), "1.1 root() 拿得回来");

  check(b.add_null(root, "n"), "1.2 add_null");
  check(b.add_bool(root, "t", true), "1.2 add_bool true");
  check(b.add_bool(root, "f", false), "1.2 add_bool false");
  check(b.add_int(root, "i", -42), "1.2 add_int");
  check(b.add_uint(root, "u", 18446744073709551615ull), "1.2 add_uint 2^64-1");
  check(b.add_double(root, "d", 2.5), "1.2 add_double");
  check(b.add_string(root, "s", "hi"), "1.2 add_string");
  check(b.add_string(root, "z", ""), "1.2 add_string 空串");
  uvcpp_json_node arr = b.add_array(root, "a");
  check(arr.valid() && arr.is_array(), "1.2 add_array 返回子数组");
  check(b.add_object(root, "o").valid(), "1.2 add_object 返回子对象");

  check(b.push_null(arr), "1.2 push_null");
  check(b.push_bool(arr, true), "1.2 push_bool");
  check(b.push_int(arr, 7), "1.2 push_int");
  check(b.push_uint(arr, 8ull), "1.2 push_uint");
  check(b.push_double(arr, 0.5), "1.2 push_double");
  check(b.push_string(arr, "x"), "1.2 push_string");
  check(b.push_array(arr).valid(), "1.2 push_array");
  check(b.push_object(arr).valid(), "1.2 push_object");
  check_eq_u(arr.size(), 8, "1.2 数组里正好 8 个元素");

  // 写出去。
  const std::string out = write_node(root, "1.3");

  // 解析回来，逐条取值对照 —— 用**另一个**后端句柄（`uvcpp_json_value`）读，
  // 所以这一条同时是"两侧口径相同"的见证。
  uvcpp_json_doc doc;
  check_st(doc.parse(out), json_doc_status::OK, "1.3 产出可再解析");
  const uvcpp_json_value r = doc.root();
  check(r.is_object(), "1.3 解回来还是对象");
  check(r.get("n").is_null(), "1.3 null 回来了");
  check(r.get("t").as_bool() == true, "1.3 true 回来了");
  check(r.get("f").as_bool(true) == false, "1.3 false 回来了");
  check_eq_ll(r.get("i").as_int(), -42, "1.3 int 回来了");
  check(r.get("u").as_uint() == 18446744073709551615ull, "1.3 uint 回来了");
  check_eq_d(r.get("d").as_double(), 2.5, "1.3 double 回来了");
  check_eq_s(r.get("s").as_string(), "hi", "1.3 字符串回来了");
  check_eq_s(r.get("z").as_string(), "", "1.3 空串回来了（不是缺失）");
  check(r.get("a").is_array() && r.get("a").size() == 8, "1.3 数组回来了");
  check(r.get("o").is_object(), "1.3 对象回来了");

  // 直接读构造侧的同一棵树（不经过写/解析）必须给出**同一批**答案。
  check(root.get("i").as_int() == -42, "1.4 构造侧直接读 int");
  check(root.get("u").as_uint() == 18446744073709551615ull,
        "1.4 构造侧直接读 uint");
  check(root.get("d").as_double() == 2.5, "1.4 构造侧直接读 double");
  check_eq_s(root.get("s").as_string(), "hi", "1.4 构造侧直接读字符串");
  check_eq_ll(arr.at(2).as_int(), 7, "1.4 构造侧按下标读");
  check(arr.at(2).is_int(), "1.4 整数不会被读成浮点");
  check(!root.get("nope").valid(), "1.4 缺失键是无效节点");
  check(!arr.at(99).valid(), "1.4 越界下标是无效节点");
  check(!root.get("i").at(0).valid(), "1.4 对非数组取下标是无效节点");
  check(!root.get("i").get("x").valid(), "1.4 对非对象取成员是无效节点");
  check_eq_s(root.get("i").type_name(), "int", "1.4 type_name");
  check_eq_s(root.get("d").type_name(), "double", "1.4 type_name double");
  check_eq_s(root.get("a").type_name(), "array", "1.4 type_name array");
  check_eq_s(uvcpp_json_node().type_name(), "invalid", "1.4 无效节点的 type_name");

  // 类型不符时的默认值语义（与解析侧同一条）：**不转换、给默认值**。
  check_eq_ll(root.get("s").as_int(41), 41, "1.5 字符串当整数读给默认值");
  check(root.get("s").as_bool(true) == true, "1.5 字符串当布尔读给默认值");
  check_eq_s(root.get("i").as_string("dflt"), "dflt",
             "1.5 整数当字符串读给默认值");
  check(root.get("n").is_null() && root.get("n").as_int(9) == 9,
        "1.5 null 当整数读也给默认值");

  // 无符号/有符号的边界：两边用同一条规则（无符号那支必须先认）。
  uvcpp_json_builder b2;
  uvcpp_json_node r2 = b2.make_object();
  b2.set_root(r2);
  check(b2.add_uint(r2, "big", 18446744073709551615ull), "1.6 add_uint 2^64-1");
  check(r2.get("big").is_int(), "1.6 无符号也归在 int 类里");
  check(!r2.get("big").is_double(), "1.6 它不是浮点");
  check_eq_s(write_node(r2, "1.6"), "{\"big\":18446744073709551615}",
             "1.6 2^64-1 写出来不丢精度");
}

// =========================================================================
// 第 ② 组：键与字符串值的**拷贝**语义
// =========================================================================

void group_key_copy() {
  uvcpp_json_builder b;
  uvcpp_json_node root = b.make_object();
  b.set_root(root);

  // 拿一个**马上要消失**的临时 `std::string` 当键。签名收的是 `const char*`，
  // 所以临时对象在这一行结束就没了 —— 键要是没拷进池，写出来就是野指针的字节。
  {
    const std::string tmp_key = "temp_key";
    check(b.add_int(root, uvcpp_json_key(tmp_key), 1), "2.1 临时 std::string 当键");
  }
  check(root.get("temp_key").as_int() == 1, "2.1 键被拷下来了");

  // 栈上的缓冲区当键，函数返回后就没了。
  {
    char buf[16];
    std::memcpy(buf, "stack_key", 9);
    check(b.add_int(root, uvcpp_json_key(buf, 9), 2), "2.1 栈上的缓冲区当键");
    std::memset(buf, 'X', sizeof(buf));  // 把源缓冲涂掉
  }
  check_eq_ll(root.get("stack_key").as_int(), 2,
              "2.1 源缓冲被涂掉之后键仍然正确（证明是拷的）");

  // 字符串**值**同一条契约。
  {
    std::string tmp_val = "temp_value";
    check(b.add_string(root, "tv", tmp_val), "2.1 临时值");
    tmp_val.assign("XXXXXXXXXX");
  }
  check_eq_s(root.get("tv").as_string(), "temp_value",
             "2.1 源字符串被改掉之后值仍然正确（证明是拷的）");

  // **含 `\0` 的键**：JSON 的键允许任意字节，所以这是合法输入，不该被当成
  // "到 NUL 为止"。`uvcpp_json_key(ptr, len)` 就是为它存在的。
  {
    const char raw[] = {'a', '\0', 'b', '\0'};
    check(b.add_int(root, uvcpp_json_key(raw, sizeof(raw)), 3), "2.2 含 \\0 的键");
    size_t klen = 0;
    const char* kp = nullptr;
    // 从构造侧找回来：用带长度的 `get`。
    uvcpp_json_node got = root.get(raw, sizeof(raw));
    check(got.valid() && got.as_int() == 3, "2.2 含 \\0 的键能被带长度地取回");
    (void)kp;
    (void)klen;
    const std::string out = write_node(root, "2.2");
    check(out.find("\"a\\u0000b\\u0000\":3") != std::string::npos,
          "2.2 含 \\0 的键写成了 \\u0000 转义（产出：" + out + "）");
  }

  // **空键**合法（`""`），与"无效键"（`nullptr`）必须分得开。
  check(b.add_int(root, "", 4), "2.3 空键是合法的");
  check_eq_ll(root.get("").as_int(), 4, "2.3 空键取回来");
  check(!b.add_int(root, uvcpp_json_key(static_cast<const char*>(nullptr)), 5),
        "2.3 nullptr 键失败");
  check(b.last_error() != nullptr && *b.last_error() != '\0',
        "2.3 失败之后 last_error() 说得清");

  // 中文键（UTF-8 多字节）：按键的**字节**走，不经任何编码转换。
  check(b.add_int(root, "中文键", 6), "2.4 UTF-8 键");
  check_eq_ll(root.get("中文键").as_int(), 6, "2.4 UTF-8 键取回来");
  check(write_node(root, "2.4").find("\"中文键\":6") != std::string::npos,
        "2.4 UTF-8 键写出来是原样的字节（不转义）");
}

// =========================================================================
// 第 ③ 组：add_copy / push_copy（两个 DOM 之间唯一的通道）
// =========================================================================

void group_copy_from_parsed() {
  uvcpp_json_builder b;
  uvcpp_json_node root = b.make_object();
  b.set_root(root);

  // 源文档在一个**内层作用域**里建、里面把子树拷出来，然后源文档析构。
  // 拷完与源再无关系（字符串进本 builder 的池）—— 这一条是那句话的判据。
  {
    uvcpp_json_doc src;
    check_st(src.parse("{\"obj\":{\"deep\":[1,{\"x\":\"yy\"},null]},"
                       "\"arr\":[true,18446744073709551615,\"s\"]}"),
             json_doc_status::OK, "3.1 源文档");
    check(b.add_copy(root, "obj", src.root().get("obj")), "3.1 add_copy 对象");
    check(b.add_copy(root, "arr", src.root().get("arr")), "3.1 add_copy 数组");
    check(b.add_copy(root, "bad", uvcpp_json_value()), "3.1 无效视图拷成 null");
  }
  // 源文档已经析构。如果 `add_copy` 是"借"而不是"拷"，这里读到的就是野内存
  // —— 会在 ASAN 下红，在普通构建下大概率是垃圾字节。
  check(root.get("obj").is_object(), "3.2 源析构后拷贝仍在");
  check_eq_ll(root.get("obj").get("deep").at(0).as_int(), 1,
              "3.2 拷贝的深层值可读");
  check_eq_s(root.get("obj").get("deep").at(1).get("x").as_string(), "yy",
             "3.2 拷贝的深层字符串可读");
  check(root.get("obj").get("deep").at(2).is_null(), "3.2 拷贝的 null 是 null");
  check(root.get("arr").is_array() && root.get("arr").size() == 3,
        "3.2 拷贝的数组长度对");
  check(root.get("arr").at(1).as_uint() == 18446744073709551615ull,
        "3.2 拷贝的 2^64-1 没丢精度");
  check(root.get("bad").is_null(), "3.2 无效视图拷成了 null");

  // 拷进来的子树**不共享**：改本树不影响别处（这里改不了源，源没了；改的是
  // 本树后续追加，验证拷来的那棵是独立的活节点、还能继续往里加）。
  check(b.add_int(root.get("obj"), "later", 99), "3.3 拷来的对象还能继续加");
  check_eq_s(write_node(root.get("obj"), "3.3"),
             "{\"deep\":[1,{\"x\":\"yy\"},null],\"later\":99}",
             "3.3 拷来的子树 + 新加的东西一起写出来");

  // 数组侧同一个通道。
  {
    uvcpp_json_doc src;
    check_st(src.parse("[{\"k\":1},[2,3]]"), json_doc_status::OK, "3.4 源数组");
    uvcpp_json_node a = b.make_array();
    b.set_root(a);
    check(b.push_copy(a, src.root().at(0)), "3.4 push_copy 对象");
    check(b.push_copy(a, src.root().at(1)), "3.4 push_copy 数组");
    check(b.push_copy(a, uvcpp_json_value()), "3.4 push_copy 无效视图");
  }
  // 注意：上面把根换成了数组 —— `set_root` 只在**还没有根**时失败，而这里
  // builder 的根已经被第一个对象占了。所以这一条同时钉住了"换不掉根"。
  check(!b.set_root(b.make_object()), "3.4 已经有根时 set_root 返回 false");
  check(b.root().is_object(), "3.4 而且根**没有**被换掉");
}

// =========================================================================
// 第 ④ 组：句柄稳定性（结构性假设）
// =========================================================================

void group_handle_stability() {
  uvcpp_json_builder b;
  uvcpp_json_node root = b.make_array();
  b.set_root(root);

  // 先塞一个对象进去，**把它的句柄留下来**。
  uvcpp_json_node first = b.push_object(root);
  check(first.valid(), "4.1 第一个元素");
  check(b.add_int(first, "k", 1), "4.1 往第一个元素里加东西");

  // 再往同一个数组追加一万个元素 —— 子节点是循环链表，追加会改很多 `next`。
  const size_t k_n = 10000;
  bool all_ok = true;
  for (size_t i = 0; i < k_n; ++i) {
    if (!b.push_int(root, static_cast<long long>(i))) all_ok = false;
  }
  check(all_ok, "4.2 一万次追加都成功");
  check_eq_u(root.size(), k_n + 1, "4.2 数组长度长对了");

  // 旧句柄仍然有效，仍然指向**同一个**节点。
  check(first.valid(), "4.3 一万次追加之后旧句柄仍有效");
  check(first.is_object(), "4.3 类型没变");
  check_eq_ll(first.get("k").as_int(), 1, "4.3 旧句柄里的值还在");
  check_eq_ll(root.at(0).get("k").as_int(), 1, "4.3 从数组走过去是同一个节点");
  check_eq_ll(root.at(k_n).as_int(), static_cast<long long>(k_n - 1),
              "4.3 最后一个元素也在");

  // 往旧句柄里再加东西，数组那边**看得见**（同一个节点，不是副本）。
  check(b.add_int(first, "later", 2), "4.3 往旧句柄里加东西");
  check_eq_ll(root.at(0).get("later").as_int(), 2,
              "4.3 数组里能看见（证明是同一个节点，不是拷贝）");

  // 写出来能解析回来、长度对 —— 不是只在这棵树里自说自话。
  const std::string out = write_node(root, "4.4");
  uvcpp_json_doc doc;
  check_st(doc.parse(out), json_doc_status::OK, "4.4 一万元素的产出可再解析");
  check_eq_u(doc.root().size(), k_n + 1, "4.4 解析回来长度仍然对");
  check_eq_ll(doc.root().at(0).get("k").as_int(), 1, "4.4 第一个元素仍然对");
}

// =========================================================================
// 第 ⑤ 组：记账（空树 0 / reset 回 0 / 移动不重复计数）
// =========================================================================

void group_accounting() {
  // 空 builder：**还没建过文档**，所以三个数字都是 0（不是"一个还没用上的
  // 文档结构"）。这条是"惰性建文档"那个设计的判据。
  {
    uvcpp_json_builder b;
    check_eq_u(b.memory_bytes(), 0, "5.1 空 builder 的 memory_bytes 是 0");
    check_eq_u(b.peak_bytes(), 0, "5.1 空 builder 的 peak_bytes 是 0");
    check_eq_u(b.allocation_count(), 0, "5.1 空 builder 的 allocation_count 是 0");
    check(b.root().valid() == false, "5.1 空 builder 没有根");
    // 空 builder 写出去 = 无效节点 = `null`，不是错误。
    check_eq_s(write_node(b.root(), "5.1"), "null", "5.1 空 builder 写出 null");
  }

  uvcpp_json_builder b;
  uvcpp_json_node root = b.make_object();
  b.set_root(root);
  uvcpp_json_node arr = b.add_array(root, "items");
  for (int i = 0; i < 100; ++i) {
    uvcpp_json_node it = b.push_object(arr);
    b.add_int(it, "id", i);
    b.add_string(it, "name", "row");
  }
  const size_t live = b.memory_bytes();
  const size_t peak = b.peak_bytes();
  const size_t calls = b.allocation_count();
  check(live > 0, "5.2 建完树之后 memory_bytes > 0");
  check(peak >= live, "5.2 peak >= live");
  // 101 个对象 + 100 个 name 字符串 + 1 个数组 + 200 个键…… 池是按 2 倍长块长的，
  // 所以是**个位数到十几**次分配，而同样的树走 nlohmann 的 std::map 是四位数。
  // 这里判的是那条量级（写成 "< 100" 而不是写死一个数：块大小的具体倍数不是契约，
  // "不是四位数"才是）。
  check(calls > 0 && calls < 100,
        "5.2 分配次数是个位数到十几（实测 " + std::to_string(calls) + "）");

  // `reset()` 之后**回到 0**。这是"没有泄漏"这句话唯一可测的形式（yyjson 的
  // free 回调不给尺寸，所以这个数字是本库拿一块 16 字节的头自己记的）。
  b.reset();
  check_eq_u(b.memory_bytes(), 0, "5.3 reset 之后 memory_bytes 回到 0");
  check_eq_u(b.peak_bytes(), 0, "5.3 reset 之后 peak_bytes 回到 0");
  check_eq_u(b.allocation_count(), 0, "5.3 reset 之后 allocation_count 回到 0");
  check(!b.root().valid(), "5.3 reset 之后没有根了");

  // reset 之后还能重新建（"就地重建"，不是一次性对象）。
  {
    uvcpp_json_node r2 = b.make_object();
    check(b.set_root(r2), "5.3 reset 之后还能建新树");
    check(b.add_int(r2, "n", 1), "5.3 新树能加东西");
    check_eq_s(write_node(r2, "5.3"), "{\"n\":1}", "5.3 新树写得出来");
    check(b.memory_bytes() > 0, "5.3 新树的计数重新开始");
  }

  // **移动**：计数块必须跟着文档走。不跟的话，移动之后每一次 free 都会写一块
  // 已经不属于它的内存 —— 症状是 `reset()` 时崩或者数字变负。
  {
    uvcpp_json_builder a;
    uvcpp_json_node ra = a.make_object();
    a.set_root(ra);
    for (int i = 0; i < 50; ++i) a.add_int(ra, "k" + std::to_string(i), i);
    const size_t a_bytes = a.memory_bytes();
    const size_t a_calls = a.allocation_count();
    check(a_bytes > 0, "5.4 移动前有字节数");

    uvcpp_json_builder mv(std::move(a));
    check_eq_u(mv.memory_bytes(), a_bytes,
               "5.4 移动之后**目标**的字节数与原来相同");
    check_eq_u(mv.allocation_count(), a_calls, "5.4 计数也跟过来了");
    check_eq_u(a.memory_bytes(), 0, "5.4 源对象变成没有文档");
    check_eq_u(a.allocation_count(), 0, "5.4 源的计数也是 0");
    check(!a.root().valid(), "5.4 源对象没有根");
    check(mv.root().valid(), "5.4 目标对象有根");
    check(mv.root().is_object() && mv.root().size() == 50,
          "5.4 目标对象的树是完整的");
    check_eq_ll(mv.root().get("k7").as_int(), 7, "5.4 树里的值没坏");
    // 仍然写得出来 —— 分配器链没断的正面见证。
    const std::string s = write_node(mv.root(), "5.4");
    check(s.size() > 10, "5.4 移动之后仍然写得出来");
    // **关键的一条**：目标析构（= reset）时不能崩、不能泄漏。源对象已经空了，
    // 如果计数块没跟着走，这里就是 double free / 野写。
    mv.reset();
    check_eq_u(mv.memory_bytes(), 0, "5.4 移动之后 reset 回 0（计数器链没断）");
    check_eq_u(mv.peak_bytes(), 0, "5.4 移动之后 peak 也回 0");
  }
  {
    // 移动**赋值**同一条。
    uvcpp_json_builder a, c;
    uvcpp_json_node ra = a.make_object();
    a.set_root(ra);
    a.add_int(ra, "x", 1);
    uvcpp_json_node rc = c.make_object();
    c.set_root(rc);
    c.add_int(rc, "y", 2);
    const size_t a_bytes = a.memory_bytes();
    c = std::move(a);  // c 原来那棵树必须被释放（不泄漏）
    check_eq_u(c.memory_bytes(), a_bytes, "5.5 移动赋值的字节数");
    check_eq_s(write_node(c.root(), "5.5"), "{\"x\":1}", "5.5 拿到的是 a 的树");
  }

  // `reserve()` 是提示：调了之后分配次数**下降**（把长块那几次提前省掉），
  // 而结果字节不变。
  {
    uvcpp_json_builder plain;
    uvcpp_json_node rp = plain.make_object();
    plain.set_root(rp);
    for (int i = 0; i < 5000; ++i) plain.add_int(rp, "k", i);
    const size_t plain_calls = plain.allocation_count();

    uvcpp_json_builder with;
    with.reserve(8000, 64 * 1024);
    uvcpp_json_node rw = with.make_object();
    with.set_root(rw);
    for (int i = 0; i < 5000; ++i) with.add_int(rw, "k", i);
    const size_t with_calls = with.allocation_count();

    check(with_calls < plain_calls,
          "5.6 reserve 之后分配次数下降（" + std::to_string(plain_calls) +
              " → " + std::to_string(with_calls) + "）");
    // 结果一样（reserve 不影响正确性）。
    check_eq_u(write_node(rw, "5.6").size(), write_node(rp, "5.6").size(),
               "5.6 reserve 不改变产出");
  }
}

// =========================================================================
// 第 ⑥ 组：键序 = **插入序**（第 1 处不同）
// =========================================================================

void group_key_order() {
  uvcpp_json_builder b;
  uvcpp_json_node root = b.make_object();
  b.set_root(root);
  // 故意按**非**字典序插入。
  check(b.add_int(root, "z", 26), "6.1 插 z");
  check(b.add_int(root, "a", 1), "6.1 插 a");
  check(b.add_int(root, "m", 13), "6.1 插 m");
  check_eq_s(write_node(root, "6.1"), "{\"z\":26,\"a\":1,\"m\":13}",
             "6.1 对象按**插入序**写出来");
  // 迭代着读也是插入序 —— 但从构造侧读要用「写出去再解析」，因为构造侧没有
  // 公开的迭代器（那是解析侧的东西）。这条与上面那条一起把"插入序"钉死。
  uvcpp_json_doc doc;
  check_st(doc.parse(write_node(root, "6.1")), json_doc_status::OK, "6.1 再解析");
  std::vector<std::string> keys;
  uvcpp_json_object_iter it(doc.root());
  const char* k = nullptr;
  size_t klen = 0;
  uvcpp_json_value v;
  while (it.next(&k, &klen, v)) keys.push_back(std::string(k, klen));
  check_eq_u(keys.size(), 3, "6.1 三个键");
  check(keys.size() == 3 && keys[0] == "z" && keys[1] == "a" && keys[2] == "m",
        "6.1 迭代出来也是插入序（z, a, m）");

  // 同名键**允许**，两个都写出来（JSON 不禁止，本模块也不假装它禁止）。
  uvcpp_json_builder d;
  uvcpp_json_node rd = d.make_object();
  d.set_root(rd);
  check(d.add_int(rd, "k", 1), "6.2 第一个 k");
  check(d.add_int(rd, "k", 2), "6.2 第二个 k 也允许");
  check_eq_u(rd.size(), 2, "6.2 两个都在（size 是 2，不是 1）");
  check_eq_s(write_node(rd, "6.2"), "{\"k\":1,\"k\":2}",
             "6.2 两个同名键都写出来");
  // 取回来是**第一个**（与解析侧 `get()` 同一条语义）。
  check_eq_ll(rd.get("k").as_int(), 1, "6.2 get() 取第一个");
}

// =========================================================================
// 第 ⑦ 组：与 nlohmann 的对照
// =========================================================================

#if UVCPP_WEBAPP_ENABLE
void group_vs_nlohmann() {
  // 7.1 逐字节：键按**升序**插入（两边就都同意顺序了），值的形态也落在两边
  // 都同意的那一批里。这一条是"我们的构造侧 + 写回"与 nlohmann 的
  // "DOM + dump()" 之间的完整对照。
  {
    uvcpp_json_builder b;
    uvcpp_json_node root = b.make_object();
    b.set_root(root);
    // 插入序 = 升序（a, d, i, n, o, s, t）。顺序**必须**这样写：第 1 处不同就在
    // 键序上，插反了这条判据量的就不是"字节一样"而是"字节不一样"了。
    uvcpp_json_node arr = b.add_array(root, "a");
    b.push_int(arr, 1);
    b.push_int(arr, 2);
    b.add_double(root, "d", 0.5);
    b.add_int(root, "i", -7);
    b.add_null(root, "n");
    uvcpp_json_node o = b.add_object(root, "o");
    b.add_int(o, "deep", 3);
    b.add_string(root, "s", "x\ny\"z\\w");
    b.add_bool(root, "t", true);
    const std::string ours = write_node(root, "7.1");

    uvcpp_json j;
    must_ok_nl(uvcpp_json_parse(ours, j), "7.1 nlohmann 解得开我们的产出");
    const std::string theirs = uvcpp_json_dump(j);
    check(!theirs.empty() && theirs != "null", "7.1 nlohmann 侧产出非空洞");
    check_eq_s(ours, theirs, "7.1 与 nlohmann dump() 逐字节相同");

    // 逐字节相同 = 再 dump 一次还是同一串（幂等），且**键序是升序**。
    check_eq_s(uvcpp_json_dump(j), theirs, "7.1 nlohmann 侧再 dump 幂等");
    check(ours.find("{\"a\":") == 0, "7.1 键序是升序（a 在最前）");
  }

  // 7.2 第 1 处不同**确实存在**：同一批内容按非字典序插入，我们的产出保留
  // 插入序，nlohmann 会重排成升序 ⇒ 两边字节不同、**值也等价**。
  {
    uvcpp_json_builder b;
    uvcpp_json_node root = b.make_object();
    b.set_root(root);
    b.add_int(root, "z", 1);
    b.add_int(root, "a", 2);
    const std::string ours = write_node(root, "7.2");
    check_eq_s(ours, "{\"z\":1,\"a\":2}", "7.2 我们保留插入序");

    uvcpp_json j1, j2;
    must_ok_nl(uvcpp_json_parse(ours, j1), "7.2 nlohmann 解我们的产出");
    check_eq_s(uvcpp_json_dump(j1), "{\"a\":2,\"z\":1}",
               "7.2 nlohmann 重排成升序（第 1 处不同确实存在）");
    must_ok_nl(uvcpp_json_parse("{\"a\":2,\"z\":1}", j2), "7.2 nlohmann 解升序写法");
    check(j1 == j2, "7.2 两种键序的**值**等价");
  }

  // 7.3 分配次数对照：同样的树，我们的构造侧是**个位数到十几**次 malloc，
  // nlohmann 的 `std::map` DOM 是**四位数**。这一条是那条量级唯一的判据
  // （装在用例里而不是只写在文档里，改坏了当场红）。
  {
    const int k_rows = 50;
    uvcpp_json_builder b;
    uvcpp_json_node root = b.make_array();  // 两边都是**裸数组**，逐字节才可比
    b.set_root(root);
    uvcpp_json json_arr = uvcpp_json::array();
    for (int i = 0; i < k_rows; ++i) {
      uvcpp_json_node it = b.push_object(root);
      // 键按升序插（第 1 处不同：nlohmann 的 std::map 是排序的）。
      b.add_int(it, "id", i);
      b.add_string(it, "name", "row");
      b.add_bool(it, "ok", true);
      uvcpp_json_node tags = b.add_array(it, "tags");
      b.push_string(tags, "a");
      b.push_string(tags, "b");

      uvcpp_json row;
      row["id"] = i;
      row["name"] = "row";
      row["ok"] = true;
      row["tags"] = uvcpp_json::array({"a", "b"});
      json_arr.push_back(std::move(row));
    }
    const size_t ours = b.allocation_count();
    const std::string a = write_node(root, "7.3");
    std::string bb;
    try {
      bb = json_arr.dump();
    } catch (const std::exception& e) {
      bb = std::string("<throw: ") + e.what() + ">";
    }
    check_eq_s(a, bb, "7.3 50 行、四个键的树与 nlohmann 逐字节相同");
    check(ours > 0 && ours < 100,
          "7.3 我们这边的分配次数是个位数到十几（实测 " +
              std::to_string(ours) + " 次，nlohmann 是四位数）");
  }
}
#endif

// =========================================================================
// 第 ⑧ 组：失败路径（不抛、不改动、说得清）
// =========================================================================

void group_failure_paths() {
  uvcpp_json_builder b;
  uvcpp_json_node root = b.make_object();
  b.set_root(root);
  check(b.add_int(root, "keep", 1), "8.0 先放一个东西进去");

  // 目标不是对象：add_* 一律失败（**不**偷偷建一个对象）。
  {
    uvcpp_json_node arr = b.add_array(root, "arr");
    check(!b.add_int(arr, "k", 1), "8.1 add_int 打数组失败");
    check(!b.add_string(arr, "k", "v"), "8.1 add_string 打数组失败");
    check(!b.add_null(arr, "k"), "8.1 add_null 打数组失败");
    check(!b.add_object(arr, "k").valid(), "8.1 add_object 打数组返回无效节点");
    check(!b.add_copy(arr, "k", uvcpp_json_value()), "8.1 add_copy 打数组失败");
    check(arr.size() == 0, "8.1 失败的挂载一个元素都没留下");
  }
  // 目标不是数组：push_* 一律失败。
  {
    uvcpp_json_node o = b.add_object(root, "o");
    check(!b.push_int(o, 1), "8.2 push_int 打对象失败");
    check(!b.push_string(o, "v"), "8.2 push_string 打对象失败");
    check(!b.push_array(o).valid(), "8.2 push_array 打对象返回无效节点");
    check(o.size() == 0, "8.2 失败的追加一个元素都没留下");
  }
  // 目标无效（默认构造的节点、或缺失键）。
  {
    check(!b.add_int(uvcpp_json_node(), "k", 1), "8.3 无效目标 add 失败");
    check(!b.push_int(uvcpp_json_node(), 1), "8.3 无效目标 push 失败");
    check(!b.add_int(root.get("nope"), "k", 1), "8.3 缺失键当目标失败");
    uvcpp_json_node n;
    check(!b.add_val(root, "k", n), "8.3 add_val 挂无效节点失败");
    check(!b.push_val(b.root().get("arr"), n), "8.3 push_val 挂无效节点失败");
  }
  // `nullptr` 字符串是**失败**，不是空串、也不是 `null`。
  {
    check(!b.add_string(root, "s", static_cast<const char*>(nullptr)),
          "8.4 add_string(nullptr) 失败");
    check(!b.add_string(root, "s", static_cast<const char*>(nullptr), 3),
          "8.4 add_string(nullptr, 3) 失败");
    check(!b.push_string(b.root().get("arr"),
                         static_cast<const char*>(nullptr)),
          "8.4 push_string(nullptr) 失败");
    check(!b.push_string(b.root().get("arr"),
                         static_cast<const char*>(nullptr), 3),
          "8.4 push_string(nullptr, 3) 失败");
    // 要 null 得明说。
    check(b.add_null(root, "s"), "8.4 要 null 就调 add_null");
  }
  // 键无效。
  {
    check(!b.add_int(root, uvcpp_json_key(static_cast<const char*>(nullptr)), 1),
          "8.5 nullptr 键失败（add_int）");
    check(!b.add_string(root, uvcpp_json_key(static_cast<const char*>(nullptr)),
                        "v"),
          "8.5 nullptr 键失败（add_string）");
  }
  // 空 `std::string` 当键是**合法**的空键，与上面的 `nullptr` 必须分得开。
  {
    const std::string empty_key;
    check(b.add_int(root, empty_key, 5), "8.5 空 std::string 是合法的空键");
    check_eq_ll(root.get("").as_int(), 5, "8.5 空键取得回来");
  }
  // **失败了也不改已经建好的部分**：树还是原来那棵（+ 本次成功的那几个）。
  {
    const std::string before = write_node(root, "8.6");
    (void)b.add_int(uvcpp_json_node(), "x", 1);
    (void)b.push_int(root, 1);  // 目标是对象，必失败
    (void)b.add_string(root, "y", static_cast<const char*>(nullptr));
    check_eq_s(write_node(root, "8.6"), before, "8.6 一串失败之后产出不变");
  }
  // `last_error()`：成功时清空，失败时说得清。
  {
    check(b.add_int(root, "ok_again", 1), "8.7 成功一次");
    check(b.last_error() != nullptr && *b.last_error() == '\0',
          "8.7 成功之后 last_error() 被清空");
    check(!b.push_int(root, 1), "8.7 故意失败一次");
    check(b.last_error() != nullptr && *b.last_error() != '\0',
          "8.7 失败之后 last_error() 非空（原话：" +
              std::string(b.last_error()) + "）");
  }
  // `set_root`：已经有根时返回 false 且**不覆盖**。
  {
    uvcpp_json_node other = b.make_object();
    check(b.add_int(other, "other", 9), "8.8 另一个游离对象");
    check(!b.set_root(other), "8.8 已经有根时 set_root 失败");
    check(b.root().get("other").valid() == false,
          "8.8 根没有被换成那个游离对象");
    check(b.root().get("keep").as_int() == 1, "8.8 根还是原来那棵");
    // 挂一个无效节点也失败。
    check(!b.set_root(uvcpp_json_node()), "8.8 set_root 无效节点失败");
  }
  // 失败**不抛异常** —— 这一条只能这样测：把一大串失败操作放在 `try` 里，
  // 捕到任何东西就是红。
  {
    bool threw = false;
    try {
      uvcpp_json_builder t;
      (void)t.add_int(uvcpp_json_node(), "k", 1);
      (void)t.push_int(uvcpp_json_node(), 1);
      (void)t.add_string(t.make_object(), "k",
                         static_cast<const char*>(nullptr));
      (void)t.set_root(uvcpp_json_node());
      t.reset();
    } catch (...) {
      threw = true;
    }
    check(!threw, "8.9 失败路径一个异常都不抛");
  }
}

// =========================================================================
// 第 ⑨ 组：写回这一侧（与构造侧一起用的那个自由函数）
// =========================================================================

void group_write_side() {
  // 无效节点 → `null`（4 个固定字节，不经过 yyjson）。
  {
    std::string out;
    check_st(uvcpp_json_write(uvcpp_json_node(), out), json_write_status::OK,
             "9.1 无效节点写出去不是错误");
    check_eq_s(out, "null", "9.1 无效节点写出 null");
  }
  // 根是标量 / 字符串 / 空容器都能直接写。
  {
    uvcpp_json_builder b;
    uvcpp_json_node arr = b.make_array();
    b.set_root(arr);
    check_eq_s(write_node(arr, "9.2"), "[]", "9.2 空数组");
    b.push_int(arr, 1);
    check_eq_s(write_node(arr, "9.2"), "[1]", "9.2 一个元素");

    uvcpp_json_builder c;
    uvcpp_json_node o = c.make_object();
    c.set_root(o);
    check_eq_s(write_node(o, "9.2"), "{}", "9.2 空对象");
    c.add_string(o, "s", "hi");
    check_eq_s(write_node(o, "9.2"), "{\"s\":\"hi\"}", "9.2 一个成员");
    // 标量当根：游离的标量挂不上去（没有 `make_*` 给标量），所以走
    // "建一个数组、取它里面的元素当根"这条路 —— 节点句柄本身就是值。
    uvcpp_json_builder d;
    uvcpp_json_node a2 = d.make_array();
    d.set_root(a2);
    d.push_string(a2, "solo");
    check_eq_s(write_node(a2.at(0), "9.2"), "\"solo\"",
               "9.2 数组里的元素也能单独当根写");
  }
  // 复用同一个 `out`：契约是"写之前清空"。
  {
    uvcpp_json_builder b;
    uvcpp_json_node r = b.make_object();
    b.set_root(r);
    b.add_int(r, "n", 1);
    std::string buf;
    check_st(uvcpp_json_write(r, buf), json_write_status::OK, "9.3 第一遍");
    check_eq_s(buf, "{\"n\":1}", "9.3 第一遍的产出");
    check_st(uvcpp_json_write(r, buf), json_write_status::OK, "9.3 第二遍");
    check_eq_s(buf, "{\"n\":1}", "9.3 复用同一个 out 的第二遍相同");
  }
  // 解析侧的语料写出来，与"再解析一次"的产出逐字节相同 —— 两侧的口径一致。
  // 注意这里用的是**另一个重载**（`uvcpp_json_write(const uvcpp_json_value&, …)`）：
  // 解析侧的句柄与构造侧的句柄是两套类型，写回有两个入口，这一条把两个入口都走一遍。
  {
    uvcpp_json_doc src;
    check_st(src.parse("{\"a\":[1,2,{\"b\":\"c\"}]}"), json_doc_status::OK,
             "9.4 源");
    std::string once;
    check_st(uvcpp_json_write(src.root(), once), json_write_status::OK,
             "9.4 解析侧写回的状态");
    check(!once.empty(), "9.4 解析侧写回非空");
    uvcpp_json_doc back;
    check_st(back.parse(once), json_doc_status::OK, "9.4 产出可再解析");
    std::string again;
    check_st(uvcpp_json_write(back.root(), again), json_write_status::OK,
             "9.4 第二遍的状态");
    check_eq_s(again, once, "9.4 写→读→写幂等");
  }
}

}  // namespace

int main() {
  std::cout << "[yyjson_build] 构造侧（uvcpp_json_builder / uvcpp_json_node）"
            << std::endl;
  group_roundtrip();
  group_key_copy();
  group_copy_from_parsed();
  group_handle_stability();
  group_accounting();
  group_key_order();
#if UVCPP_WEBAPP_ENABLE
  group_vs_nlohmann();
#else
  std::cout << "  [SKIP] 第 ⑦ 组（与 nlohmann 的对照）需要 webapp" << std::endl;
#endif
  group_failure_paths();
  group_write_side();

  if (g_failures == 0) {
    std::cout << "[yyjson_build] ALL PASS" << std::endl;
    return 0;
  }
  std::cerr << "[yyjson_build] FAIL (" << g_failures << ")" << std::endl;
  return 1;
}
