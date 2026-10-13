/**
 * @file src/yyjson/uvcpp_json_doc.cpp
 * @brief `uvcpp_json_doc.h` 的实现（yyjson 后端）。
 */

#include <yyjson/uvcpp_json_doc.h>

#include <cstdlib>
#include <cstring>
#include <new>

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
// 桥：DOM → uvcpp_json_writer
// =========================================================================

namespace {

void write_value(const yyjson_val* v, uvcpp_json_writer& w) {
  if (v == nullptr) {
    w.null_value();  // 没有这个值 ⇒ JSON 的 null（不是失败）
    return;
  }
  switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_NULL:
      w.null_value();
      break;
    case YYJSON_TYPE_BOOL:
      w.value(yyjson_get_bool(v));  // 它本来就返回 bool，不用再跟 0 比一次
      break;
    case YYJSON_TYPE_NUM:
      // 三条分支顺序不能换：无符号那支必须排在浮点之前，否则
      // `18446744073709551615` 会先被当成 real 取一次（丢精度）。
      if (yyjson_is_uint(v)) {
        w.value(static_cast<unsigned long long>(yyjson_get_uint(v)));
      } else if (yyjson_is_sint(v)) {
        w.value(static_cast<long long>(yyjson_get_sint(v)));
      } else {
        w.value(yyjson_get_real(v));
      }
      break;
    case YYJSON_TYPE_STR:
      // 二进制安全：长度从 DOM 来，非法 UTF-8 原样写出（解析器那边已经拒掉了
      // 非法 UTF-8，所以这条路走不到"写出非法字节"）。
      w.value(yyjson_get_str(v), yyjson_get_len(v));
      break;
    case YYJSON_TYPE_ARR: {
      w.array_begin();
      yyjson_arr_iter it;
      yyjson_arr_iter_init(const_cast<yyjson_val*>(v), &it);
      while (yyjson_val* e = yyjson_arr_iter_next(&it)) write_value(e, w);
      w.array_end();
      break;
    }
    case YYJSON_TYPE_OBJ: {
      w.object_begin();
      yyjson_obj_iter it;
      yyjson_obj_iter_init(const_cast<yyjson_val*>(v), &it);
      while (yyjson_val* k = yyjson_obj_iter_next(&it)) {
        w.key(yyjson_get_str(k), yyjson_get_len(k));
        write_value(yyjson_obj_iter_get_val(k), w);
      }
      w.object_end();
      break;
    }
    default:
      // 到不了（NONE 只出现在 `yyjson_doc_get_root` 为空的场合，那条在
      // `uvcpp_json_write` 的入口就挡住了）。真到了也不能少一个值 ——
      // 少一个值会让产出静默地变成另一份 JSON。
      w.null_value();
      break;
  }
}

}  // namespace

json_write_status uvcpp_json_write(const uvcpp_json_value& v,
                                   uvcpp_json_writer& w) {
  const yyjson_val* val = static_cast<const yyjson_val*>(v.val_);
  if (val == nullptr) {
    // 无效视图写成 `null`。理由与 `uvcpp_json_writer::value((const char*)nullptr)`
    // 相同：JSON 里"没有这个值"有确切表示，不该让整份响应构造失败。
    w.null_value();
    return w.status();
  }
  write_value(val, w);
  return w.status();
}

#endif  // UVCPP_YYJSON_ENABLE

}  // namespace uvcpp
