/**
 * @file src/yyjson/uvcpp_json_doc.cpp
 * @brief `uvcpp_json_doc.h` 的实现（yyjson 后端）。
 */

#include <yyjson/uvcpp_json_doc.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

#include <uvcpp/uvcpp_json_scan.h>

// yyjson 是**私有**依赖：`<yyjson.h>` 出现在这里、以及 CMake 给本模块的私有
// include 路径里，公开头一个 yyjson 类型都不出现（见头文件「yyjson 是私有依赖」
// 一节）。`extern "C"` 必须写明 —— yyjson 是 C 库，C++ 里不加会按 C++ 链接名找
// 符号，链接期才发现。
extern "C" {
#include <yyjson.h>
}

namespace uvcpp {

#if UVCPP_YYJSON_ENABLE

namespace {

// =========================================================================
// 迭代器尺寸：公开头里的那个字节数组必须装得下后端的迭代器
// =========================================================================
//
// 这几条 `static_assert` 是本模块"必须能看见 yyjson 类型"的少数几处之一，也正是
// 把 yyjson 类型挡在公开头之外的全部代价。yyjson 哪天把迭代器加宽，这里当场红，
// 而不是等到某次栈上覆写（那种错在 ASAN 下才看得见，生产里是静默的）。
static_assert(sizeof(yyjson_obj_iter) <= sizeof(void*) * 6,
              "uvcpp_json_object_iter 的存储装不下 yyjson_obj_iter");
static_assert(sizeof(yyjson_arr_iter) <= sizeof(void*) * 6,
              "uvcpp_json_array_iter 的存储装不下 yyjson_arr_iter");
static_assert(alignof(yyjson_obj_iter) <= alignof(std::max_align_t),
              "uvcpp_json_object_iter 的存储对齐不够");
static_assert(alignof(yyjson_arr_iter) <= alignof(std::max_align_t),
              "uvcpp_json_array_iter 的存储对齐不够");

}  // namespace

// =========================================================================
// 带计数的分配器
// =========================================================================
//
// 为什么要自己记：`memory_bytes()` 是要**断言**的（"reset 之后回到 0"这句话没有
// 数字就只是形容词），而 yyjson 的 `free` 回调**不告诉你这次释放了多少字节**
// （签名是 `void (*)(void *ctx, void *ptr)`）。所以每块分配前面加一个定长的头把
// 尺寸记下来，释放时按头里的数字扣。
//
// 头占 `sizeof(size_t) * 2`（LP64 上 16 字节），malloc 返回的指针对 16 字节对齐
// ⇒ 跳过 16 字节之后仍然 16 字节对齐，yyjson 那边看到的对齐与直接 malloc 相同。
// 这不只是对齐问题：yyjson 会在分配出来的块上做**对齐访问**（见它 `yyjson.h` 里
// `YYJSON_DISABLE_UNALIGNED_MEMORY_ACCESS` 那段的说明）。
//
// **这个类型必须在 `uvcpp` 命名空间的作用域上**，不能放进上面那个匿名命名空间：
// 头文件里 `uvcpp_json_doc` 的私有成员写的是 `struct alloc_stats* stats_;`，它
// 指名的是 `uvcpp::alloc_stats`；匿名命名空间里的同名类型是**另一个**类型。那种
// 错在头文件里一个字都看不出来（两处都写着 `alloc_stats`），只在 .cpp 里报一句
// "无法把 `alloc_stats*` 转成 `alloc_stats*`"。
struct alloc_stats {
  size_t live;   ///< 当前活着的字节数（含垫片）
  size_t peak;   ///< live 的峰值
  size_t calls;  ///< malloc/realloc 被调用的次数（free 不算）
};

namespace {

const size_t k_alloc_pad = sizeof(size_t) * 2;

void* alc_malloc(void* ctx, size_t size) {
  alloc_stats* st = static_cast<alloc_stats*>(ctx);
  void* base = std::malloc(size + k_alloc_pad);
  if (base == nullptr) return nullptr;  // yyjson 见到 nullptr 会报 MEMORY_ALLOCATION
  *static_cast<size_t*>(base) = size;
  st->live += size + k_alloc_pad;
  ++st->calls;
  if (st->live > st->peak) st->peak = st->live;
  return static_cast<char*>(base) + k_alloc_pad;
}

void* alc_realloc(void* ctx, void* ptr, size_t old_size, size_t size) {
  alloc_stats* st = static_cast<alloc_stats*>(ctx);
  if (ptr == nullptr) return alc_malloc(ctx, size);

  char* base = static_cast<char*>(ptr) - k_alloc_pad;
  // 用头里记的尺寸、不用 `old_size`：两者应当相等（yyjson 自己的记账），
  // 而"应当"不是可以拿来扣字节的依据 —— 头是唯一真相。
  const size_t was = *reinterpret_cast<size_t*>(base);
  (void)old_size;
  void* nb = std::realloc(base, size + k_alloc_pad);
  if (nb == nullptr) return nullptr;
  *static_cast<size_t*>(nb) = size;
  st->live = st->live - was + size;
  ++st->calls;
  if (st->live > st->peak) st->peak = st->live;
  return static_cast<char*>(nb) + k_alloc_pad;
}

void alc_free(void* ctx, void* ptr) {
  if (ptr == nullptr) return;
  alloc_stats* st = static_cast<alloc_stats*>(ctx);
  char* base = static_cast<char*>(ptr) - k_alloc_pad;
  st->live -= *reinterpret_cast<size_t*>(base);
  std::free(base);
}

yyjson_alc make_alc(alloc_stats* st) {
  yyjson_alc alc;
  alc.malloc = &alc_malloc;
  alc.realloc = &alc_realloc;
  alc.free = &alc_free;
  alc.ctx = st;
  return alc;
}

// 后端的失败码 → 本模块的状态。
//
// 只有"分配失败"需要单独认出来（它与 nlohmann 路径的 bad_alloc 对应）；
// 其余一律 SYNTAX —— 包括 yyjson 对纯空白输入的 `EMPTY_CONTENT`：**空白不是
// "空输入"**，nlohmann 路径对 `"   "` 报的是 SYNTAX（EMPTY 那条只认
// `len == 0`），两个后端必须给出同一个答案，所以这里不能照抄后端的措辞。
json_doc_status map_read_code(int code) {
  if (code == YYJSON_READ_ERROR_MEMORY_ALLOCATION) {
    return json_doc_status::NO_MEMORY;
  }
  return json_doc_status::SYNTAX;
}

// 读标志：`ALLOW_BOM` 是**为了与 nlohmann 对齐**才开的，不是图方便。
// nlohmann 的 `parse` 无条件跳过开头的 UTF-8 BOM（`skip_bom`），而 yyjson 默认
// 把它当非法输入（"UTF-8 byte order mark (BOM) is not supported"）。
// BOM 在 RFC 8259 §8.1 里是"实现可以忽略"的东西，两边都算合规 —— 但"同一份
// 输入两个后端给出不同状态"是**我们自己的**契约不允许的（见头文件那张对照表），
// 所以这一位必须开。用例里有一条专门喂 BOM。
const yyjson_read_flag k_read_flags = YYJSON_READ_ALLOW_BOM;

}  // namespace

// =========================================================================
// 状态名
// =========================================================================

const char* json_doc_status_name(json_doc_status status) {
  switch (status) {
    case json_doc_status::OK:        return "OK";
    case json_doc_status::EMPTY:     return "EMPTY";
    case json_doc_status::SYNTAX:    return "SYNTAX";
    case json_doc_status::TOO_DEEP:  return "TOO_DEEP";
    case json_doc_status::TOO_LARGE: return "TOO_LARGE";
    case json_doc_status::NO_MEMORY: return "NO_MEMORY";
  }
  return "?";
}

json_doc_options::json_doc_options(size_t depth, size_t bytes)
    : max_depth(depth), max_bytes(bytes) {}

// =========================================================================
// uvcpp_json_doc
// =========================================================================

uvcpp_json_doc::uvcpp_json_doc()
    : doc_(nullptr),
      stats_(nullptr),
      last_error_(""),
      last_error_pos_(0) {}

uvcpp_json_doc::~uvcpp_json_doc() { reset(); }

// 移动 = 指针搬家。**分配器计数块必须跟着走**：yyjson 的文档里存着
// `alc.ctx = stats_`，如果计数块留在源对象里、随源对象析构而消失，那份文档
// 之后的每一次 free 都会写一块已经不属于它的内存。
uvcpp_json_doc::uvcpp_json_doc(uvcpp_json_doc&& other) noexcept
    : doc_(other.doc_),
      stats_(other.stats_),
      last_error_(other.last_error_),
      last_error_pos_(other.last_error_pos_) {
  other.doc_ = nullptr;
  other.stats_ = nullptr;
  other.last_error_ = "";
  other.last_error_pos_ = 0;
}

uvcpp_json_doc& uvcpp_json_doc::operator=(uvcpp_json_doc&& other) noexcept {
  if (this != &other) {
    reset();
    doc_ = other.doc_;
    stats_ = other.stats_;
    last_error_ = other.last_error_;
    last_error_pos_ = other.last_error_pos_;
    other.doc_ = nullptr;
    other.stats_ = nullptr;
    other.last_error_ = "";
    other.last_error_pos_ = 0;
  }
  return *this;
}

void uvcpp_json_doc::reset() {
  if (doc_ != nullptr) {
    // 顺序是硬的：文档用的是 `stats_` 那块内存里的计数，所以**先**释放文档、
    // **再**释放计数块。反过来的话，`yyjson_doc_free` 的每一次 free 都在写
    // 已经还给堆的内存。
    yyjson_doc_free(static_cast<yyjson_doc*>(doc_));
    delete stats_;
    doc_ = nullptr;
    stats_ = nullptr;
  }
  last_error_ = "";
  last_error_pos_ = 0;
}

bool uvcpp_json_doc::valid() const { return doc_ != nullptr; }

uvcpp_json_value uvcpp_json_doc::root() const {
  if (doc_ == nullptr) return uvcpp_json_value();
  return uvcpp_json_value(
      yyjson_doc_get_root(static_cast<yyjson_doc*>(doc_)));
}

size_t uvcpp_json_doc::memory_bytes() const {
  return stats_ != nullptr ? stats_->live : 0;
}

size_t uvcpp_json_doc::peak_bytes() const {
  return stats_ != nullptr ? stats_->peak : 0;
}

size_t uvcpp_json_doc::allocation_count() const {
  return stats_ != nullptr ? stats_->calls : 0;
}

const char* uvcpp_json_doc::last_error() const { return last_error_; }

size_t uvcpp_json_doc::last_error_position() const { return last_error_pos_; }

json_doc_status uvcpp_json_doc::parse(const char* data, size_t len) {
  return parse(data, len, json_doc_options());
}

json_doc_status uvcpp_json_doc::parse(const std::string& text) {
  return parse(text.data(), text.size(), json_doc_options());
}

json_doc_status uvcpp_json_doc::parse(const std::string& text,
                                      const json_doc_options& opts) {
  return parse(text.data(), text.size(), opts);
}

json_doc_status uvcpp_json_doc::parse(const char* data, size_t len,
                                      const json_doc_options& opts) {
  // 三道闸门的顺序与 nlohmann 路径**逐条相同**（`uvcpp_json_parse` 里那三句），
  // 因为状态是它们判的，不是解析器判的。顺序换了，"同一份输入两个后端同一个
  // 状态"这句话就不成立了：一份 9 MiB 又超深的输入在两边会被判成不同的错。
  if (data == nullptr || len == 0) {
    last_error_ = "输入为空";
    last_error_pos_ = 0;
    return json_doc_status::EMPTY;
  }
  if (len > opts.max_bytes) {
    last_error_ = "超过 max_bytes";
    last_error_pos_ = 0;
    return json_doc_status::TOO_LARGE;
  }
  if (uvcpp_json_max_nesting(data, len) > opts.max_depth) {
    last_error_ = "超过 max_depth";
    last_error_pos_ = 0;
    return json_doc_status::TOO_DEEP;
  }

  // 这次尝试用**自己的**计数块：解析失败时它随临时文档一起消失，本对象留下的
  // 还是上一次成功那份（`memory_bytes()` 因此永远描述"当前这份 DOM"）。
  alloc_stats* stats = new (std::nothrow) alloc_stats();
  if (stats == nullptr) {
    last_error_ = "分配失败（计数块）";
    last_error_pos_ = 0;
    return json_doc_status::NO_MEMORY;
  }
  stats->live = 0;
  stats->peak = 0;
  stats->calls = 0;

  yyjson_alc alc = make_alc(stats);
  yyjson_read_err err;
  std::memset(&err, 0, sizeof(err));

  // `yyjson_read_opts` 要 `char*`，但我们**没开** `YYJSON_READ_INSITU`，所以它
  // 对输入只读（就地解析才是它改写输入的唯一条件，见 yyjson.h 里那个标志的
  // 说明）。`const_cast` 因此是安全的，也是这个 C API 的唯一用法。
  yyjson_doc* parsed = yyjson_read_opts(const_cast<char*>(data), len,
                                        k_read_flags, &alc, &err);
  if (parsed == nullptr) {
    last_error_ = (err.msg != nullptr) ? err.msg : "解析失败";
    last_error_pos_ = err.pos;
    delete stats;
    // 失败不改动本对象：旧的文档、旧的值视图、旧的 `memory_bytes()` 全部照旧。
    return map_read_code(static_cast<int>(err.code));
  }

  reset();
  doc_ = parsed;
  stats_ = stats;
  last_error_ = "";
  last_error_pos_ = 0;
  return json_doc_status::OK;
}

// =========================================================================
// uvcpp_json_value
// =========================================================================

// 为什么**没有**一个 `value -> yyjson_val*` 的公开取用通道：那会漏出 yyjson 类型，
// 而本模块对外的承诺是"换掉 yyjson 不动使用者的代码"。`uvcpp_json_value` 的私有
// 句柄 `val_` 只在本文件里被读，经头文件里那几条 `friend` 声明（`uvcpp_json_doc`
// 与两个迭代器）。所以下面每个成员函数的第一件事都是把它当作
// `const yyjson_val*` 拿起来用。

uvcpp_json_value::uvcpp_json_value() : val_(nullptr) {}

uvcpp_json_value::uvcpp_json_value(const void* handle) : val_(handle) {}

bool uvcpp_json_value::valid() const { return val_ != nullptr; }

const char* uvcpp_json_value::type_name() const {
  const yyjson_val* v = static_cast<const yyjson_val*>(val_);
  if (v == nullptr) return "invalid";
  switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_NULL: return "null";
    case YYJSON_TYPE_BOOL: return "bool";
    case YYJSON_TYPE_NUM:
      // 整数（有符号/无符号）都叫 "int"：本模块的 `is_int()` 也是这个口径。
      return (yyjson_get_subtype(v) == YYJSON_SUBTYPE_REAL) ? "double" : "int";
    case YYJSON_TYPE_STR: return "string";
    case YYJSON_TYPE_ARR: return "array";
    case YYJSON_TYPE_OBJ: return "object";
    default: return "invalid";
  }
}

bool uvcpp_json_value::is_null() const {
  return valid() && yyjson_is_null(static_cast<const yyjson_val*>(val_));
}

bool uvcpp_json_value::is_bool() const {
  return valid() && yyjson_is_bool(static_cast<const yyjson_val*>(val_));
}

bool uvcpp_json_value::is_int() const {
  return valid() && yyjson_is_int(static_cast<const yyjson_val*>(val_));
}

bool uvcpp_json_value::is_double() const {
  return valid() && yyjson_is_real(static_cast<const yyjson_val*>(val_));
}

bool uvcpp_json_value::is_number() const {
  return valid() && yyjson_is_num(static_cast<const yyjson_val*>(val_));
}

bool uvcpp_json_value::is_string() const {
  return valid() && yyjson_is_str(static_cast<const yyjson_val*>(val_));
}

bool uvcpp_json_value::is_object() const {
  return valid() && yyjson_is_obj(static_cast<const yyjson_val*>(val_));
}

bool uvcpp_json_value::is_array() const {
  return valid() && yyjson_is_arr(static_cast<const yyjson_val*>(val_));
}

bool uvcpp_json_value::as_bool(bool def) const {
  if (!is_bool()) return def;
  return yyjson_get_bool(static_cast<const yyjson_val*>(val_));
}

long long uvcpp_json_value::as_int(long long def) const {
  if (!is_number()) return def;
  const yyjson_val* v = static_cast<const yyjson_val*>(val_);
  // 三条分支与 nlohmann 路径的 `uvcpp_json_get_int` 一一对应：无符号先转
  // （`18446744073709551615` 会截成 -1，与那边 `static_cast<long long>` 相同），
  // 有符号直取，浮点截断（`3.9` → 3，`3.0` → 3）。
  if (yyjson_is_uint(v)) {
    return static_cast<long long>(yyjson_get_uint(v));
  }
  if (yyjson_is_sint(v)) {
    return static_cast<long long>(yyjson_get_sint(v));
  }
  return static_cast<long long>(yyjson_get_real(v));
}

unsigned long long uvcpp_json_value::as_uint(unsigned long long def) const {
  if (!is_number()) return def;
  const yyjson_val* v = static_cast<const yyjson_val*>(val_);
  if (yyjson_is_uint(v)) return static_cast<unsigned long long>(yyjson_get_uint(v));
  if (yyjson_is_sint(v)) {
    return static_cast<unsigned long long>(yyjson_get_sint(v));
  }
  return static_cast<unsigned long long>(yyjson_get_real(v));
}

double uvcpp_json_value::as_double(double def) const {
  if (!is_number()) return def;
  const yyjson_val* v = static_cast<const yyjson_val*>(val_);
  if (yyjson_is_real(v)) return yyjson_get_real(v);
  if (yyjson_is_uint(v)) return static_cast<double>(yyjson_get_uint(v));
  return static_cast<double>(yyjson_get_sint(v));
}

std::string uvcpp_json_value::as_string(const std::string& def) const {
  size_t len = 0;
  const char* s = as_cstr(&len);
  if (s == nullptr) return def;
  return std::string(s, len);
}

const char* uvcpp_json_value::as_cstr(size_t* len) const {
  const yyjson_val* v = static_cast<const yyjson_val*>(val_);
  if (v == nullptr || !yyjson_is_str(v)) return nullptr;
  if (len != nullptr) *len = yyjson_get_len(v);
  return yyjson_get_str(v);
}

size_t uvcpp_json_value::size() const {
  const yyjson_val* v = static_cast<const yyjson_val*>(val_);
  if (v == nullptr) return 0;
  if (yyjson_is_arr(v)) return yyjson_arr_size(v);
  if (yyjson_is_obj(v)) return yyjson_obj_size(v);
  return 0;
}

uvcpp_json_value uvcpp_json_value::get(const char* key) const {
  if (key == nullptr) return uvcpp_json_value();
  const yyjson_val* v = static_cast<const yyjson_val*>(val_);
  if (v == nullptr || !yyjson_is_obj(v)) return uvcpp_json_value();
  // 重复键取**第一个**（yyjson 的语义），nlohmann 取最后一个 —— 文件头第 2 条
  // 不同，用例把两边各自的行为都钉住了。
  return uvcpp_json_value(yyjson_obj_get(v, key));
}

uvcpp_json_value uvcpp_json_value::get(const char* key, size_t key_len) const {
  if (key == nullptr) return uvcpp_json_value();
  const yyjson_val* v = static_cast<const yyjson_val*>(val_);
  if (v == nullptr || !yyjson_is_obj(v)) return uvcpp_json_value();
  return uvcpp_json_value(yyjson_obj_getn(v, key, key_len));
}

uvcpp_json_value uvcpp_json_value::at(size_t index) const {
  const yyjson_val* v = static_cast<const yyjson_val*>(val_);
  if (v == nullptr || !yyjson_is_arr(v)) return uvcpp_json_value();
  return uvcpp_json_value(yyjson_arr_get(v, index));
}

// =========================================================================
// 迭代器
// =========================================================================

uvcpp_json_object_iter::uvcpp_json_object_iter(const uvcpp_json_value& obj) {
  std::memset(storage_, 0, sizeof(storage_));
  yyjson_obj_iter_init(static_cast<yyjson_val*>(
                           const_cast<void*>(obj.val_)),
                       reinterpret_cast<yyjson_obj_iter*>(storage_));
}

bool uvcpp_json_object_iter::next(const char** key, size_t* key_len,
                                  uvcpp_json_value& value) {
  yyjson_val* k =
      yyjson_obj_iter_next(reinterpret_cast<yyjson_obj_iter*>(storage_));
  if (k == nullptr) return false;
  if (key != nullptr) *key = yyjson_get_str(k);
  if (key_len != nullptr) *key_len = yyjson_get_len(k);
  value = uvcpp_json_value(yyjson_obj_iter_get_val(k));
  return true;
}

bool uvcpp_json_object_iter::next(std::string& key, uvcpp_json_value& value) {
  const char* k = nullptr;
  size_t klen = 0;
  if (!next(&k, &klen, value)) return false;
  key.assign(k, klen);
  return true;
}

uvcpp_json_array_iter::uvcpp_json_array_iter(const uvcpp_json_value& arr) {
  std::memset(storage_, 0, sizeof(storage_));
  yyjson_arr_iter_init(static_cast<yyjson_val*>(
                           const_cast<void*>(arr.val_)),
                       reinterpret_cast<yyjson_arr_iter*>(storage_));
}

bool uvcpp_json_array_iter::next(uvcpp_json_value& value) {
  yyjson_val* v =
      yyjson_arr_iter_next(reinterpret_cast<yyjson_arr_iter*>(storage_));
  if (v == nullptr) return false;
  value = uvcpp_json_value(v);
  return true;
}

// =========================================================================
// 与 nlohmann 路径同名的入口
// =========================================================================

json_doc_status uvcpp_json_parse(const char* data, size_t len,
                                 uvcpp_json_doc& out) {
  return out.parse(data, len);
}

json_doc_status uvcpp_json_parse(const char* data, size_t len,
                                 uvcpp_json_doc& out,
                                 const json_doc_options& opts) {
  return out.parse(data, len, opts);
}

json_doc_status uvcpp_json_parse(const std::string& s, uvcpp_json_doc& out) {
  return out.parse(s);
}

std::string uvcpp_json_get_string(const uvcpp_json_value& obj, const char* key,
                                  const std::string& def) {
  const uvcpp_json_value v = obj.get(key);
  if (!v.is_string()) return def;
  return v.as_string(def);
}

bool uvcpp_json_get_bool(const uvcpp_json_value& obj, const char* key, bool def) {
  const uvcpp_json_value v = obj.get(key);
  if (!v.is_bool()) return def;
  return v.as_bool(def);
}

bool uvcpp_json_get_int(const uvcpp_json_value& obj, const char* key,
                        long long& out) {
  const uvcpp_json_value v = obj.get(key);
  if (!v.is_number()) return false;
  // 与 nlohmann 路径同形：类型不符返回 false **且不写 out**（写 0 会让调用方
  // 分不清"取到了 0"和"没取到"）。
  out = v.as_int();
  return true;
}

bool uvcpp_json_get_double(const uvcpp_json_value& obj, const char* key,
                           double& out) {
  const uvcpp_json_value v = obj.get(key);
  if (!v.is_number()) return false;
  out = v.as_double();
  return true;
}

// =========================================================================
// 写回：DOM → JSON 文本（yyjson 自己的序列化器）
// =========================================================================

namespace {

// 写回的三个标志。为什么是这三个，见头文件那条声明上的表；一句话：
//   * `LOWERCASE_HEX` —— yyjson 默认写**大写**的 `\u001F`，老写器与 nlohmann
//     都是小写。上游头里那句 "Only effective when ESCAPE_UNICODE is also set"
//     是**误导**：控制字符那条 `\uXXXX` 一律查 `get_hex_table_with_flag`，
//     与 ESCAPE_UNICODE 无关（`yyjson.c` 的 `write_str`，实测也确认了）。
//   * `INF_AND_NAN_AS_NULL` —— `uvcpp_json_writer` 对 NaN/Inf 的既有行为。
//   * `ALLOW_INVALID_UNICODE` —— 二进制安全：非法 UTF-8 原样透传（老写器也是），
//     否则 yyjson 换成 `U+FFFD`，而 nlohmann 的 `dump()` 在这条路上直接抛。
// **不开** ESCAPE_UNICODE / ESCAPE_SLASHES / PRETTY / NEWLINE_AT_END。
const yyjson_write_flag k_write_flags = static_cast<yyjson_write_flag>(
    YYJSON_WRITE_LOWERCASE_HEX | YYJSON_WRITE_INF_AND_NAN_AS_NULL |
    YYJSON_WRITE_ALLOW_INVALID_UNICODE);

// 快路的起点与上限。起点取 `out` 现有的容量，是有意的：同一形状的响应从第二次
// 起缓冲就够大了，于是稳态**零 malloc**。
//
// **缓冲必须比产出大，而且不是大一两个字节** —— 这一点是量出来的，别照"够装下就行"
// 的直觉改：写器把它的 `yyjson_mut_write_ctx` 栈放在**缓冲尾部**（`ctx = end`、往下
// 长），而且每写一段之前先按**最坏情形**保留空间（字符串那里是 `str_len * 6 + 16`，
// 见 `yyjson.c` 的 `incr_len`）。所以
// 「缓冲长度 == 产出长度」**一定**失败（实测 2 692 B 的产出要 > 2 820 B 才写得下）。
// 早先这里只在失败时转去分配式那条路，于是快路**永远走不到**、每次都白写一遍大半段
// 再重来 —— 量具上就是每调用 13 次多余 `malloc`。现在按头文件写的那样**翻倍重试**。
//
// `k_max_buf` 之上不走快路：按最坏情形多占一份内存不值当，改走按需分配那条路。
const size_t k_min_buf = 4096;
const size_t k_max_buf = 64u * 1024u * 1024u;

json_write_status map_write_code(yyjson_write_code code) {
  switch (code) {
    case YYJSON_WRITE_SUCCESS:
      return json_write_status::OK;
    case YYJSON_WRITE_ERROR_MEMORY_ALLOCATION:
      return json_write_status::NO_MEMORY;
    case YYJSON_WRITE_ERROR_DEPTH:
      // 到不了：`YYJSON_WRITER_DEPTH_LIMIT` 是编译期的 0（这一版 yyjson 的写器
      // 用显式栈、不递归）。留着这条是为了"yyjson 哪天把它打开"时不静默走 default。
      return json_write_status::TOO_DEEP;
    default:
      // INVALID_PARAMETER（空缓冲 / 空根）、INVALID_VALUE_TYPE，以及我们这条路
      // 上到不了的 FILE_OPEN / FILE_WRITE / NAN_OR_INF / INVALID_STRING。
      return json_write_status::MISUSE;
  }
}

// 两个后端（不可变 / 可变）各有一对写函数，签名只差句柄类型。这两个转接把差异
// 收在一处，下面那条共同实现于是只用写一遍。
size_t call_write_buf(const void* val, char* buf, size_t n,
                      yyjson_write_err* err, bool mut_side) {
  if (mut_side) {
    return yyjson_mut_val_write_buf(
        buf, n, static_cast<const yyjson_mut_val*>(val), k_write_flags, err);
  }
  return yyjson_val_write_buf(buf, n, static_cast<const yyjson_val*>(val),
                              k_write_flags, err);
}

char* call_write_opts(const void* val, const yyjson_alc* alc, size_t* len,
                      yyjson_write_err* err, bool mut_side) {
  if (mut_side) {
    return yyjson_mut_val_write_opts(static_cast<const yyjson_mut_val*>(val),
                                     k_write_flags, alc, len, err);
  }
  return yyjson_val_write_opts(static_cast<const yyjson_val*>(val),
                               k_write_flags, alc, len, err);
}

json_write_status write_dom_impl(const void* handle, std::string& out,
                                 bool mut_side) {
  // 契约：调用前的内容丢掉、失败时是空的。所以 `clear()` 先来。
  out.clear();

  if (handle == nullptr) {
    // 无效视图 / 无效节点写成 `null`。这一条**不经过 yyjson** —— 它是 4 个固定
    // 字节，不是"序列化"。理由与 `uvcpp_json_writer::value((const char*)nullptr)`
    // 相同：JSON 里"没有这个值"有确切表示，不该让整份响应构造失败。
    out.append("null", 4);
    return json_write_status::OK;
  }

  // 快路：直接写进 `out` 已有的缓冲；装不下就把缓冲翻倍再来一次。
  // 成功的 `resize(len)` 把翻倍多出来的容量留在 `out` 里 ⇒ 同一形状的响应从第二次
  // 起一遍过、零分配（头文件那条性能契约就是这一条）。
  size_t n = out.capacity();
  if (n < k_min_buf) n = k_min_buf;
  while (n <= k_max_buf) {
    out.resize(n);
    yyjson_write_err err;
    std::memset(&err, 0, sizeof(err));
    const size_t len = call_write_buf(handle, &out[0], n, &err, mut_side);
    if (len > 0) {
      out.resize(len);  // 不含结尾 NUL（yyjson 的 `*dat_len` 就是这么算的）
      return json_write_status::OK;
    }
    if (err.code != YYJSON_WRITE_ERROR_MEMORY_ALLOCATION) {
      // 不是"装不下"，是真的错了（空缓冲、空根……）。
      out.clear();
      return map_write_code(err.code);
    }
    // 落到这里只有一种解释：**缓冲不够**。缓冲写法用的是 `YYJSON_NULL_ALC`
    // （`malloc` / `realloc` 恒返回 NULL），所以 `alc.realloc` 那一步是唯一能
    // 走到 MEMORY_ALLOCATION 的地方 ⇒ "太小"是这条路上这个错误码的全部含义。
    if (n > k_max_buf / 2) break;  // 再翻就过上限了，交给下面那条路
    n *= 2;
  }

  // 慢路：按需分配（用本模块自己的计数分配器），再搬进 `out`。
  // 任何尺寸都能成功；代价是大文档多一次拷贝。
  alloc_stats st;
  st.live = 0;
  st.peak = 0;
  st.calls = 0;
  yyjson_alc alc = make_alc(&st);
  size_t len = 0;
  yyjson_write_err err;
  std::memset(&err, 0, sizeof(err));
  char* p = call_write_opts(handle, &alc, &len, &err, mut_side);
  if (p == nullptr) {
    out.clear();
    return map_write_code(err.code);
  }
  out.assign(p, len);
  alc_free(&st, p);
  return json_write_status::OK;
}

// 真身里 `out` 长大那几步（`resize` / `assign` / `append`）**可能抛** —— 本模块
// 一律不抛异常（调用点在 libuv 回调里，异常穿到 C 栈就是进程级的麻烦），所以在
// 这道壳里收成状态码。成功的路上这条 `try` 一分钱不花。
json_write_status write_dom(const void* handle, std::string& out,
                            bool mut_side) {
  try {
    return write_dom_impl(handle, out, mut_side);
  } catch (const std::bad_alloc&) {
    out.clear();  // 抛出时 `out` 里可能是半截产出，按契约清空
    return json_write_status::NO_MEMORY;
  } catch (const std::length_error&) {
    // 只有一种可能：产出超过 `std::string` 的长度上限（`k_max_buf` 那条路管不到
    // 的尺寸）。归 `MISUSE`：调用方给了一棵写不进字符串的树。
    out.clear();
    return json_write_status::MISUSE;
  }
}

}  // namespace

json_write_status uvcpp_json_write(const uvcpp_json_value& v,
                                   std::string& out) {
  return write_dom(v.val_, out, false);
}

json_write_status uvcpp_json_write(const uvcpp_json_node& v, std::string& out) {
  return write_dom(v.val_, out, true);
}

// =========================================================================
// 构造侧：uvcpp_json_key
// =========================================================================

uvcpp_json_key::uvcpp_json_key(const char* s)
    : ptr(s), len(s != nullptr ? std::strlen(s) : 0) {}

uvcpp_json_key::uvcpp_json_key(const char* s, size_t n) : ptr(s), len(n) {}

uvcpp_json_key::uvcpp_json_key(const std::string& s)
    : ptr(s.data()), len(s.size()) {}

// =========================================================================
// 构造侧：uvcpp_json_node
// =========================================================================
//
// 与 `uvcpp_json_value` 那一节逐条同形 —— 同名的判定、同名的默认值语义，只是
// 句柄与后端函数换成可变的那一套（`yyjson_mut_*`）。故意**不合并**成模板：合并
// 的话公开头里就得出现一个带 yyjson 类型的模板参数，那正是本模块挡在外面的事。
//
// 判定的口径与不可变那侧必须**逐条相同**，否则同一份数据"解析着读"与"建完读"
// 会给出不同答案。用例第 ① 组就是拿"建树 → 写 → 解析 → 取值"把两边钉在一起的。

uvcpp_json_node::uvcpp_json_node() : val_(nullptr) {}

uvcpp_json_node::uvcpp_json_node(const void* handle) : val_(handle) {}

bool uvcpp_json_node::valid() const { return val_ != nullptr; }

const char* uvcpp_json_node::type_name() const {
  const yyjson_mut_val* v = static_cast<const yyjson_mut_val*>(val_);
  if (v == nullptr) return "invalid";
  switch (yyjson_mut_get_type(v)) {
    case YYJSON_TYPE_NULL: return "null";
    case YYJSON_TYPE_BOOL: return "bool";
    case YYJSON_TYPE_NUM:
      return (yyjson_mut_get_subtype(v) == YYJSON_SUBTYPE_REAL) ? "double" : "int";
    case YYJSON_TYPE_STR: return "string";
    case YYJSON_TYPE_ARR: return "array";
    case YYJSON_TYPE_OBJ: return "object";
    default: return "invalid";
  }
}

bool uvcpp_json_node::is_null() const {
  return valid() && yyjson_mut_is_null(static_cast<const yyjson_mut_val*>(val_));
}

bool uvcpp_json_node::is_bool() const {
  return valid() && yyjson_mut_is_bool(static_cast<const yyjson_mut_val*>(val_));
}

bool uvcpp_json_node::is_int() const {
  return valid() && yyjson_mut_is_int(static_cast<const yyjson_mut_val*>(val_));
}

bool uvcpp_json_node::is_double() const {
  return valid() && yyjson_mut_is_real(static_cast<const yyjson_mut_val*>(val_));
}

bool uvcpp_json_node::is_number() const {
  return valid() && yyjson_mut_is_num(static_cast<const yyjson_mut_val*>(val_));
}

bool uvcpp_json_node::is_string() const {
  return valid() && yyjson_mut_is_str(static_cast<const yyjson_mut_val*>(val_));
}

bool uvcpp_json_node::is_object() const {
  return valid() && yyjson_mut_is_obj(static_cast<const yyjson_mut_val*>(val_));
}

bool uvcpp_json_node::is_array() const {
  return valid() && yyjson_mut_is_arr(static_cast<const yyjson_mut_val*>(val_));
}

bool uvcpp_json_node::as_bool(bool def) const {
  if (!is_bool()) return def;
  return yyjson_mut_get_bool(static_cast<const yyjson_mut_val*>(val_));
}

long long uvcpp_json_node::as_int(long long def) const {
  if (!is_number()) return def;
  const yyjson_mut_val* v = static_cast<const yyjson_mut_val*>(val_);
  // 三条分支与不可变那侧一一对应（顺序也一样：无符号必须先认）。
  if (yyjson_mut_is_uint(v)) {
    return static_cast<long long>(yyjson_mut_get_uint(v));
  }
  if (yyjson_mut_is_sint(v)) {
    return static_cast<long long>(yyjson_mut_get_sint(v));
  }
  return static_cast<long long>(yyjson_mut_get_real(v));
}

unsigned long long uvcpp_json_node::as_uint(unsigned long long def) const {
  if (!is_number()) return def;
  const yyjson_mut_val* v = static_cast<const yyjson_mut_val*>(val_);
  if (yyjson_mut_is_uint(v)) {
    return static_cast<unsigned long long>(yyjson_mut_get_uint(v));
  }
  if (yyjson_mut_is_sint(v)) {
    return static_cast<unsigned long long>(yyjson_mut_get_sint(v));
  }
  return static_cast<unsigned long long>(yyjson_mut_get_real(v));
}

double uvcpp_json_node::as_double(double def) const {
  if (!is_number()) return def;
  const yyjson_mut_val* v = static_cast<const yyjson_mut_val*>(val_);
  if (yyjson_mut_is_real(v)) return yyjson_mut_get_real(v);
  if (yyjson_mut_is_uint(v)) return static_cast<double>(yyjson_mut_get_uint(v));
  return static_cast<double>(yyjson_mut_get_sint(v));
}

std::string uvcpp_json_node::as_string(const std::string& def) const {
  size_t len = 0;
  const char* s = as_cstr(&len);
  if (s == nullptr) return def;
  return std::string(s, len);
}

const char* uvcpp_json_node::as_cstr(size_t* len) const {
  const yyjson_mut_val* v = static_cast<const yyjson_mut_val*>(val_);
  if (v == nullptr || !yyjson_mut_is_str(v)) return nullptr;
  if (len != nullptr) *len = yyjson_mut_get_len(v);
  return yyjson_mut_get_str(v);
}

size_t uvcpp_json_node::size() const {
  const yyjson_mut_val* v = static_cast<const yyjson_mut_val*>(val_);
  if (v == nullptr) return 0;
  if (yyjson_mut_is_arr(v)) return yyjson_mut_arr_size(v);
  if (yyjson_mut_is_obj(v)) return yyjson_mut_obj_size(v);
  return 0;
}

uvcpp_json_node uvcpp_json_node::get(const char* key) const {
  if (key == nullptr) return uvcpp_json_node();
  const yyjson_mut_val* v = static_cast<const yyjson_mut_val*>(val_);
  if (v == nullptr || !yyjson_mut_is_obj(v)) return uvcpp_json_node();
  // 重复键取**第一个** —— 与解析侧的 `uvcpp_json_value::get` 同一条语义。
  return uvcpp_json_node(yyjson_mut_obj_get(v, key));
}

uvcpp_json_node uvcpp_json_node::get(const char* key, size_t key_len) const {
  if (key == nullptr) return uvcpp_json_node();
  const yyjson_mut_val* v = static_cast<const yyjson_mut_val*>(val_);
  if (v == nullptr || !yyjson_mut_is_obj(v)) return uvcpp_json_node();
  return uvcpp_json_node(yyjson_mut_obj_getn(v, key, key_len));
}

uvcpp_json_node uvcpp_json_node::at(size_t index) const {
  const yyjson_mut_val* v = static_cast<const yyjson_mut_val*>(val_);
  if (v == nullptr || !yyjson_mut_is_arr(v)) return uvcpp_json_node();
  return uvcpp_json_node(yyjson_mut_arr_get(v, index));
}

// =========================================================================
// 构造侧：uvcpp_json_builder
// =========================================================================

namespace {

// 节点句柄是 `const void*`（它对外是"视图"），可构造侧要改它 —— 这一处
// `const_cast` 就是那个区分的全部代价。
yyjson_mut_val* handle_of(const void* p) {
  return const_cast<yyjson_mut_val*>(static_cast<const yyjson_mut_val*>(p));
}

// 一次挂载的预备件：文档、目标容器（**已经验过类型**）、拷好的键
// （数组侧恒为 nullptr）。
struct target {
  yyjson_mut_doc* doc;
  yyjson_mut_val* ctn;
  yyjson_mut_val* key;
};

// 就绪返回 nullptr，否则返回失败原因（人类可读，进 `last_error()`）。
const char* resolve_obj(yyjson_mut_doc* doc, const void* h,
                        const uvcpp_json_key& k, target* t) {
  t->doc = doc;
  t->key = nullptr;
  t->ctn = handle_of(h);
  if (t->ctn == nullptr || !yyjson_mut_is_obj(t->ctn)) return "目标不是对象";
  if (k.ptr == nullptr) return "键无效（const char* 是 nullptr）";
  // **键一律拷进文档自己的字符串池**（`yyjson_mut_strncpy`）。为什么不用
  // `yyjson_mut_obj_add_strncpy` 那套糖：它的键是**借**的（上游文档明写
  // "The key string is not copied"），传一个临时的 `c_str()` 就会悬空。
  // 也不用 `yyjson_mut_obj_put`：同名键那条路是线性查找，建一个 11 键的对象
  // 就是 O(n²)。`yyjson_mut_obj_add` 是公开 API、O(1) 追加、允许重复键。
  t->key = yyjson_mut_strncpy(doc, k.ptr, k.len);
  if (t->key == nullptr) return "分配失败（键）";
  return nullptr;
}

const char* resolve_arr(yyjson_mut_doc* doc, const void* h, target* t) {
  t->doc = doc;
  t->key = nullptr;
  t->ctn = handle_of(h);
  if (t->ctn == nullptr || !yyjson_mut_is_arr(t->ctn)) return "目标不是数组";
  return nullptr;
}

const char* attach(const target& t, yyjson_mut_val* val) {
  if (val == nullptr) return "分配失败（值）";
  if (t.key != nullptr) {
    if (!yyjson_mut_obj_add(t.ctn, t.key, val)) return "挂不上（目标不再是对象）";
  } else {
    if (!yyjson_mut_arr_add_val(t.ctn, val)) return "挂不上（目标不再是数组）";
  }
  return nullptr;
}

// 字符串值的共同检查。`nullptr` 是**失败**，不是空串、也不是 `null` —— 要 `null`
// 请调 `add_null()` / `push_null()`。静默把"没有这个指针"写成另一种 JSON 类型，
// 正是构造一棵响应时最难查的那类 bug。
const char* check_str(const char* s) {
  return s == nullptr ? "字符串指针是 nullptr（要 null 请用 add_null/push_null）"
                      : nullptr;
}

}  // namespace

uvcpp_json_builder::uvcpp_json_builder()
    : doc_(nullptr), stats_(nullptr), last_error_("") {}

uvcpp_json_builder::~uvcpp_json_builder() { reset(); }

// 移动 = 指针搬家。**分配器计数块必须跟着走**：yyjson 的可变文档里存着
// `alc.ctx = stats_`，计数块留在源对象里、随源对象析构而消失的话，那份文档之后
// 的每一次 free 都会写一块已经不属于它的内存。（与 `uvcpp_json_doc` 同一条理由。）
uvcpp_json_builder::uvcpp_json_builder(uvcpp_json_builder&& other) noexcept
    : doc_(other.doc_), stats_(other.stats_), last_error_(other.last_error_) {
  other.doc_ = nullptr;
  other.stats_ = nullptr;
  other.last_error_ = "";
}

uvcpp_json_builder& uvcpp_json_builder::operator=(
    uvcpp_json_builder&& other) noexcept {
  if (this != &other) {
    reset();
    doc_ = other.doc_;
    stats_ = other.stats_;
    last_error_ = other.last_error_;
    other.doc_ = nullptr;
    other.stats_ = nullptr;
    other.last_error_ = "";
  }
  return *this;
}

void uvcpp_json_builder::reset() {
  if (doc_ != nullptr) {
    // 顺序是硬的：文档用的是 `stats_` 那块内存里的计数，所以**先**释放文档、
    // **再**释放计数块（反过来每一次 free 都在写已经还给堆的内存）。
    yyjson_mut_doc_free(static_cast<yyjson_mut_doc*>(doc_));
    delete stats_;
    doc_ = nullptr;
    stats_ = nullptr;
  }
  last_error_ = "";
}

void uvcpp_json_builder::note(const char* why) {
  last_error_ = (why != nullptr) ? why : "";
}

bool uvcpp_json_builder::ensure_doc() {
  if (doc_ != nullptr) return true;
  alloc_stats* st = new (std::nothrow) alloc_stats();
  if (st == nullptr) {
    note("分配失败（计数块）");
    return false;
  }
  st->live = 0;
  st->peak = 0;
  st->calls = 0;
  yyjson_alc alc = make_alc(st);
  yyjson_mut_doc* d = yyjson_mut_doc_new(&alc);
  if (d == nullptr) {
    delete st;  // 文档没建起来，计数块跟着走（顺序与 reset() 相反，也对）
    note("分配失败（文档）");
    return false;
  }
  doc_ = d;
  stats_ = st;
  return true;
}

void uvcpp_json_builder::reserve(size_t values, size_t string_bytes) {
  if (!ensure_doc()) return;
  yyjson_mut_doc* d = static_cast<yyjson_mut_doc*>(doc_);
  // 两个 setter 只改**以后**分配的块的尺寸（起点分别是 16 个节点 / 256 字节，
  // 之后按 2 倍长）。所以这是"提示"，不是"保证"—— 调不调都正确，调了把长块那
  // 几次 malloc 提前省掉。返回 false 只有"尺寸离谱到会溢出"这一种原因。
  if (values > 0 && !yyjson_mut_doc_set_val_pool_size(d, values)) {
    note("reserve: values 太大");
    return;
  }
  if (string_bytes > 0 && !yyjson_mut_doc_set_str_pool_size(d, string_bytes)) {
    note("reserve: string_bytes 太大");
    return;
  }
  note(nullptr);
}

size_t uvcpp_json_builder::memory_bytes() const {
  return stats_ != nullptr ? stats_->live : 0;
}

size_t uvcpp_json_builder::peak_bytes() const {
  return stats_ != nullptr ? stats_->peak : 0;
}

size_t uvcpp_json_builder::allocation_count() const {
  return stats_ != nullptr ? stats_->calls : 0;
}

const char* uvcpp_json_builder::last_error() const { return last_error_; }

uvcpp_json_node uvcpp_json_builder::make_object() {
  if (!ensure_doc()) return uvcpp_json_node();
  yyjson_mut_val* v = yyjson_mut_obj(static_cast<yyjson_mut_doc*>(doc_));
  if (v == nullptr) {
    note("分配失败（对象）");
    return uvcpp_json_node();
  }
  note(nullptr);
  return uvcpp_json_node(v);
}

uvcpp_json_node uvcpp_json_builder::make_array() {
  if (!ensure_doc()) return uvcpp_json_node();
  yyjson_mut_val* v = yyjson_mut_arr(static_cast<yyjson_mut_doc*>(doc_));
  if (v == nullptr) {
    note("分配失败（数组）");
    return uvcpp_json_node();
  }
  note(nullptr);
  return uvcpp_json_node(v);
}

bool uvcpp_json_builder::set_root(uvcpp_json_node n) {
  if (!ensure_doc()) return false;
  yyjson_mut_doc* d = static_cast<yyjson_mut_doc*>(doc_);
  if (!n.valid()) {
    note("set_root: 节点无效");
    return false;
  }
  if (yyjson_mut_doc_get_root(d) != nullptr) {
    note("set_root: 已经有根了（要换先 reset）");
    return false;
  }
  yyjson_mut_doc_set_root(d, handle_of(n.val_));
  note(nullptr);
  return true;
}

uvcpp_json_node uvcpp_json_builder::root() const {
  if (doc_ == nullptr) return uvcpp_json_node();
  // 返回的是**非 const** 的指针：`root()` 只读地取出来，但拿到它的调用方要能
  // 继续往里加东西（"先挂根、再把根当对象填"是最常见的写法）。
  yyjson_mut_val* r =
      yyjson_mut_doc_get_root(static_cast<yyjson_mut_doc*>(doc_));
  return uvcpp_json_node(r);
}

// ── 对象侧 ─────────────────────────────────────────────────────────────
//
// 每个函数都是同一个形状：确保文档在 → 解析目标 → 造值 → 挂上 → 记原因。
// 26 个成员函数全是这五行，所以下面只给几个代表性的写注释。

bool uvcpp_json_builder::add_null(uvcpp_json_node obj, uvcpp_json_key key) {
  if (!ensure_doc()) return false;
  target t;
  const char* why =
      resolve_obj(static_cast<yyjson_mut_doc*>(doc_), obj.val_, key, &t);
  if (why == nullptr) why = attach(t, yyjson_mut_null(t.doc));
  note(why);
  return why == nullptr;
}

bool uvcpp_json_builder::add_bool(uvcpp_json_node obj, uvcpp_json_key key,
                                  bool v) {
  if (!ensure_doc()) return false;
  target t;
  const char* why =
      resolve_obj(static_cast<yyjson_mut_doc*>(doc_), obj.val_, key, &t);
  if (why == nullptr) why = attach(t, yyjson_mut_bool(t.doc, v));
  note(why);
  return why == nullptr;
}

bool uvcpp_json_builder::add_int(uvcpp_json_node obj, uvcpp_json_key key,
                                 long long v) {
  if (!ensure_doc()) return false;
  target t;
  const char* why =
      resolve_obj(static_cast<yyjson_mut_doc*>(doc_), obj.val_, key, &t);
  if (why == nullptr) {
    why = attach(t, yyjson_mut_sint(t.doc, static_cast<int64_t>(v)));
  }
  note(why);
  return why == nullptr;
}

bool uvcpp_json_builder::add_uint(uvcpp_json_node obj, uvcpp_json_key key,
                                  unsigned long long v) {
  if (!ensure_doc()) return false;
  target t;
  const char* why =
      resolve_obj(static_cast<yyjson_mut_doc*>(doc_), obj.val_, key, &t);
  if (why == nullptr) {
    why = attach(t, yyjson_mut_uint(t.doc, static_cast<uint64_t>(v)));
  }
  note(why);
  return why == nullptr;
}

bool uvcpp_json_builder::add_double(uvcpp_json_node obj, uvcpp_json_key key,
                                    double v) {
  if (!ensure_doc()) return false;
  target t;
  const char* why =
      resolve_obj(static_cast<yyjson_mut_doc*>(doc_), obj.val_, key, &t);
  if (why == nullptr) why = attach(t, yyjson_mut_real(t.doc, v));
  note(why);
  return why == nullptr;
}

bool uvcpp_json_builder::add_string(uvcpp_json_node obj, uvcpp_json_key key,
                                    const char* s) {
  const char* bad = check_str(s);
  if (bad != nullptr) {
    note(bad);
    return false;
  }
  return add_string(obj, key, s, std::strlen(s));
}

bool uvcpp_json_builder::add_string(uvcpp_json_node obj, uvcpp_json_key key,
                                    const char* s, size_t len) {
  const char* bad = check_str(s);
  if (bad != nullptr) {
    note(bad);
    return false;
  }
  if (!ensure_doc()) return false;
  target t;
  const char* why =
      resolve_obj(static_cast<yyjson_mut_doc*>(doc_), obj.val_, key, &t);
  // 字符串**值**也是拷进池的（`yyjson_mut_strncpy`），所以调用方可以传一个马上
  // 就消失的缓冲区 —— 与键同一条契约。
  if (why == nullptr) why = attach(t, yyjson_mut_strncpy(t.doc, s, len));
  note(why);
  return why == nullptr;
}

bool uvcpp_json_builder::add_string(uvcpp_json_node obj, uvcpp_json_key key,
                                    const std::string& s) {
  return add_string(obj, key, s.data(), s.size());
}

uvcpp_json_node uvcpp_json_builder::add_object(uvcpp_json_node obj,
                                               uvcpp_json_key key) {
  if (!ensure_doc()) return uvcpp_json_node();
  target t;
  const char* why =
      resolve_obj(static_cast<yyjson_mut_doc*>(doc_), obj.val_, key, &t);
  yyjson_mut_val* child = nullptr;
  if (why == nullptr) {
    child = yyjson_mut_obj(t.doc);
    why = attach(t, child);
  }
  note(why);
  return (why == nullptr) ? uvcpp_json_node(child) : uvcpp_json_node();
}

uvcpp_json_node uvcpp_json_builder::add_array(uvcpp_json_node obj,
                                              uvcpp_json_key key) {
  if (!ensure_doc()) return uvcpp_json_node();
  target t;
  const char* why =
      resolve_obj(static_cast<yyjson_mut_doc*>(doc_), obj.val_, key, &t);
  yyjson_mut_val* child = nullptr;
  if (why == nullptr) {
    child = yyjson_mut_arr(t.doc);
    why = attach(t, child);
  }
  note(why);
  return (why == nullptr) ? uvcpp_json_node(child) : uvcpp_json_node();
}

bool uvcpp_json_builder::add_val(uvcpp_json_node obj, uvcpp_json_key key,
                                 uvcpp_json_node v) {
  if (!ensure_doc()) return false;
  target t;
  const char* why =
      resolve_obj(static_cast<yyjson_mut_doc*>(doc_), obj.val_, key, &t);
  if (why == nullptr) why = attach(t, handle_of(v.val_));
  note(why);
  return why == nullptr;
}

bool uvcpp_json_builder::add_copy(uvcpp_json_node obj, uvcpp_json_key key,
                                  const uvcpp_json_value& src) {
  if (!ensure_doc()) return false;
  target t;
  const char* why =
      resolve_obj(static_cast<yyjson_mut_doc*>(doc_), obj.val_, key, &t);
  if (why == nullptr) {
    // `yyjson_val_mut_copy` 是**迭代**的（一遍连续数组扫描 + 重连 `next`，
    // 见 yyjson.c）—— 深拷十万层也不会爆栈，与本模块"两边都不递归"一致。
    // （另一个方向的 `yyjson_mut_val_mut_copy` 是**递归**的，所以我们不提供
    //  构造侧 → 构造侧的拷贝，见头文件里那段说明。）
    const yyjson_val* sv = static_cast<const yyjson_val*>(src.val_);
    why = attach(t, sv == nullptr ? yyjson_mut_null(t.doc)
                                  : yyjson_val_mut_copy(t.doc, sv));
  }
  note(why);
  return why == nullptr;
}

// ── 数组侧 ─────────────────────────────────────────────────────────────

bool uvcpp_json_builder::push_null(uvcpp_json_node arr) {
  if (!ensure_doc()) return false;
  target t;
  const char* why = resolve_arr(static_cast<yyjson_mut_doc*>(doc_), arr.val_, &t);
  if (why == nullptr) why = attach(t, yyjson_mut_null(t.doc));
  note(why);
  return why == nullptr;
}

bool uvcpp_json_builder::push_bool(uvcpp_json_node arr, bool v) {
  if (!ensure_doc()) return false;
  target t;
  const char* why = resolve_arr(static_cast<yyjson_mut_doc*>(doc_), arr.val_, &t);
  if (why == nullptr) why = attach(t, yyjson_mut_bool(t.doc, v));
  note(why);
  return why == nullptr;
}

bool uvcpp_json_builder::push_int(uvcpp_json_node arr, long long v) {
  if (!ensure_doc()) return false;
  target t;
  const char* why = resolve_arr(static_cast<yyjson_mut_doc*>(doc_), arr.val_, &t);
  if (why == nullptr) {
    why = attach(t, yyjson_mut_sint(t.doc, static_cast<int64_t>(v)));
  }
  note(why);
  return why == nullptr;
}

bool uvcpp_json_builder::push_uint(uvcpp_json_node arr, unsigned long long v) {
  if (!ensure_doc()) return false;
  target t;
  const char* why = resolve_arr(static_cast<yyjson_mut_doc*>(doc_), arr.val_, &t);
  if (why == nullptr) {
    why = attach(t, yyjson_mut_uint(t.doc, static_cast<uint64_t>(v)));
  }
  note(why);
  return why == nullptr;
}

bool uvcpp_json_builder::push_double(uvcpp_json_node arr, double v) {
  if (!ensure_doc()) return false;
  target t;
  const char* why = resolve_arr(static_cast<yyjson_mut_doc*>(doc_), arr.val_, &t);
  if (why == nullptr) why = attach(t, yyjson_mut_real(t.doc, v));
  note(why);
  return why == nullptr;
}

bool uvcpp_json_builder::push_string(uvcpp_json_node arr, const char* s) {
  const char* bad = check_str(s);
  if (bad != nullptr) {
    note(bad);
    return false;
  }
  return push_string(arr, s, std::strlen(s));
}

bool uvcpp_json_builder::push_string(uvcpp_json_node arr, const char* s,
                                     size_t len) {
  const char* bad = check_str(s);
  if (bad != nullptr) {
    note(bad);
    return false;
  }
  if (!ensure_doc()) return false;
  target t;
  const char* why = resolve_arr(static_cast<yyjson_mut_doc*>(doc_), arr.val_, &t);
  if (why == nullptr) why = attach(t, yyjson_mut_strncpy(t.doc, s, len));
  note(why);
  return why == nullptr;
}

bool uvcpp_json_builder::push_string(uvcpp_json_node arr,
                                     const std::string& s) {
  return push_string(arr, s.data(), s.size());
}

uvcpp_json_node uvcpp_json_builder::push_object(uvcpp_json_node arr) {
  if (!ensure_doc()) return uvcpp_json_node();
  target t;
  const char* why = resolve_arr(static_cast<yyjson_mut_doc*>(doc_), arr.val_, &t);
  yyjson_mut_val* child = nullptr;
  if (why == nullptr) {
    child = yyjson_mut_obj(t.doc);
    why = attach(t, child);
  }
  note(why);
  return (why == nullptr) ? uvcpp_json_node(child) : uvcpp_json_node();
}

uvcpp_json_node uvcpp_json_builder::push_array(uvcpp_json_node arr) {
  if (!ensure_doc()) return uvcpp_json_node();
  target t;
  const char* why = resolve_arr(static_cast<yyjson_mut_doc*>(doc_), arr.val_, &t);
  yyjson_mut_val* child = nullptr;
  if (why == nullptr) {
    child = yyjson_mut_arr(t.doc);
    why = attach(t, child);
  }
  note(why);
  return (why == nullptr) ? uvcpp_json_node(child) : uvcpp_json_node();
}

bool uvcpp_json_builder::push_val(uvcpp_json_node arr, uvcpp_json_node v) {
  if (!ensure_doc()) return false;
  target t;
  const char* why = resolve_arr(static_cast<yyjson_mut_doc*>(doc_), arr.val_, &t);
  if (why == nullptr) why = attach(t, handle_of(v.val_));
  note(why);
  return why == nullptr;
}

bool uvcpp_json_builder::push_copy(uvcpp_json_node arr,
                                   const uvcpp_json_value& src) {
  if (!ensure_doc()) return false;
  target t;
  const char* why = resolve_arr(static_cast<yyjson_mut_doc*>(doc_), arr.val_, &t);
  if (why == nullptr) {
    const yyjson_val* sv = static_cast<const yyjson_val*>(src.val_);
    why = attach(t, sv == nullptr ? yyjson_mut_null(t.doc)
                                  : yyjson_val_mut_copy(t.doc, sv));
  }
  note(why);
  return why == nullptr;
}

#endif  // UVCPP_YYJSON_ENABLE

}  // namespace uvcpp
