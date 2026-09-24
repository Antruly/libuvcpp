# 应用层 JSON 构造器指南

`uvcpp_json_writer`（`src/uvcpp/uvcpp_json_writer.h`）是**把几个字段拼成一段 JSON** 的那一层。
它是**核心模块**：永远在编、零依赖、不吃任何 `UVCPP_ENABLE_*` 开关，也不需要 nlohmann。

这一页讲的是"怎么用它"，以及"它的边界在哪"。它不是 JSON **解析**指南 —— 解析与 DOM
在 `webapp/uvcpp_web_json.h`（nlohmann 后端），见 [`webapp-guide.md`](webapp-guide.md)。

## 目录

1. [为什么还要一个构造器](#1-为什么还要一个构造器)
2. [编译期条件与包含](#2-编译期条件与包含)
3. [五分钟上手](#3-五分钟上手)
4. [转义契约](#4-转义契约)
5. [逗号、括号与 `finish()`](#5-逗号括号与-finish)
6. [失败是粘性的](#6-失败是粘性的)
7. [数字](#7-数字)
8. [上限与缓冲](#8-上限与缓冲)
9. [接到响应上](#9-接到响应上)
10. [线程与所有权](#10-线程与所有权)
11. [这份代码的判据](#11-这份代码的判据)
12. [没做的（如实列出）](#12-没做的如实列出)

---

## 1. 为什么还要一个构造器

本仓的 JSON 支持原来只有两处，两处都不适合"服务端拼几个字段发出去"：

| 这一处 | 做什么 | 为什么不够 |
|---|---|---|
| `webapp/uvcpp_web_json.h` | 解析 + DOM 序列化（nlohmann） | 要先建一棵树再 `dump()`；而且整个挂在 webapp 上，**webapp 默认关掉**（`CMakeLists.txt`），还会拉一个几十 MB 的 nlohmann 进来 |
| 手写字符串 | `resp.json_str("{\"size\":" + std::to_string(n) + "}")` | 字段名是硬编码时"看着对"；一旦值是**运行时来的字符串**，同一句话就漏掉转义，产出**能被注入的 JSON** |

第二行不是假想：它在仓里真的存在（`src/webapp/uvcpp_web_app.h`，静态文件路由那一段），
今天是对的，因为那个值是个整形的大小。**本模块要解决的就是"下一个人往里塞一个用户发的
字符串"这件事**：转义、逗号、括号配对三件容易写错的事交给库，使用者只管按顺序描述结构。

形状是**流式直写**：没有中间表示，按调用顺序把字节写进缓冲。

## 2. 编译期条件与包含

- **没有开关。** 它是核心模块，`UVCPP_ENABLE_NET` / `_WEB` / `_WEBAPP` 一个都不影响它；
  `CMakeLists.txt:472` 那个收 `src/uvcpp/` 下源文件的 GLOB 已经把它收进去了。
- **不需要 nlohmann、不需要 OpenSSL、不需要 zlib。** 头文件只带
  `<cstddef>` 与 `<string>`，实现只多带 `<cmath> <cstdio> <cstdlib> <cstring> <new> <stdexcept>`。
- **C++11**（本仓没有 per-target 覆盖），所以没有 `std::string_view`、没有 `if constexpr`。
- 包含方式：`#include <uvcpp/uvcpp_json_writer.h>`。**它不在 `<uvcpp.h>` 里** ——
  那个聚合头只收 `uvcpp_define/version/export` 与 `handle/loop/req` 三组，本模块要单独包含。
- 导出的东西：`uvcpp_json_writer`、`json_write_status`、`json_write_status_name()`、
  `json_write_options`。

## 3. 五分钟上手

```cpp
#include <uvcpp/uvcpp_json_writer.h>

#include <string>

std::string doc_build_size(std::size_t size, const std::string& name) {
  std::string body;
  uvcpp::uvcpp_json_writer w(body);
  w.object_begin();
  w.key("size");
  w.value(static_cast<long long>(size));   // 宽度自己选，别指望隐式转换猜对
  w.member("name", name);                  // key + value 一步；转义由库做
  w.key("ids");
  w.array_begin();
  for (int i = 0; i < 3; ++i) w.value(i);
  w.array_end();
  w.object_end();
  if (w.finish() != uvcpp::json_write_status::OK) return std::string();  // 非 OK ⇒ 丢弃
  return body;
}
```

产出是 `{"size":1024,"name":"…","ids":[0,1,2]}`，**没有空格**（本模块不做美化输出）。

三条要点：

- 每个方法返回 `json_write_status`。**刻意不给链式**（`.key("a").value(1)`）：那要求返回
  `*this`，错误就只能靠"回头再查 `status()`"，而漏查一次就变成静默的坏产出。
  真要接链的只有一类，就是上面那个 `member()`。
- `member(k, v)` 是**模板**而不是三个重载（`src/uvcpp/uvcpp_json_writer.h:288`）。理由在
  头文件里：`member("k", 5)` 在 `(const char*, long long)` 与 `(const char*, bool)` 之间
  是**歧义**的（`int` 到这两个的转换同级），而 `member("k", true)` 只留整数版本又会把
  JSON 的 `true` 写成 `1`。
- `object_begin()` / `array_begin()` 之后**必须**有对应的 `end`。`finish()` 不补字节，
  它只把"这份产出到底完不完整"变成一个返回值。

## 4. 转义契约

这张表是**契约**，不是实现细节。第三、四行那两个分档住在实现里的
256 项分类表 `escape_table`（`src/uvcpp/uvcpp_json_writer.cpp:110`，用表而不是 `switch`
是因为每个字节都要判一次），搬运字节的那一段是 `append_escaped()`
（`src/uvcpp/uvcpp_json_writer.cpp:271`）：

| 输入字节 | 产出 | 说明 |
|---|---|---|
| `"` | `\"` | |
| `\` | `\\` | |
| 0x08 0x09 0x0A 0x0C 0x0D | `\b` `\t` `\n` `\f` `\r` | 五个短形态 |
| 其余 0x00–0x1F | `\u00XX` | **小写**十六进制 |
| `/` | **原样** | 见下 |
| 0x20 及以上 | 原样逐字节 | 含 0x7F（DEL）与 UTF-8 多字节 |

```cpp
#include <uvcpp/uvcpp_json_writer.h>

#include <string>

void doc_escape(std::string& out, const std::string& user_input) {
  uvcpp::uvcpp_json_writer w(out);
  w.object_begin();
  w.member("q", user_input);     // 里面的 " \ 换行 0x01 都由库转义
  w.key("path/with/slash");      // 键走同一条路；`/` 保持原样
  w.value(1);
  w.object_end();
  w.finish();
}
```

**键与值走的是同一条转义路径**（同一个函数），所以不存在"值转义了、键忘了"这个形状。

**不转义 `/` 是与 hical 的有意分歧**（hical 在 `hical:src/core/CompileTimeJson.h:51` 把 `/`
转成 `\/`）。RFC 8259 §7 **不要求**转义 `/`；转它纯粹是为了 HTML 内嵌 JSON 的
`</script>` 那类场景，而本模块的定位是 **HTTP 响应体**（由 JSON 解析器消费，不经 HTML
解析器）。真要在 HTML 里内嵌，应当在**嵌入点**处理 —— 那需要"这段 JSON 要嵌到哪"的
上下文，本模块拿不到，所以它不假装能防。

**不校验 UTF-8。** 本模块逐字节搬运，不解码也不校验：合法 UTF-8 进来就是合法 JSON，
非法字节序列进来就是非法 JSON。这一点与 nlohmann 不同（它的 `dump()` 遇到非法 UTF-8 会
抛 `type_error.316`），本模块**不抛**，所以只能原样输出。要校验请在上游做。

## 5. 逗号、括号与 `finish()`

逗号是**自动**的，规则一句话：**谁在写一个新元素，谁负责它前面那个逗号**。

- 数组：写值之前，若这个数组已经有过元素，先写 `,`。
- 对象：写**键**之前，若这个对象已经有过键值对，先写 `,`。
- 关容器（`object_end` / `array_end`）**不写任何逗号**。

于是"多余逗号"这种错在 API 形状上就写不出来。括号配对是另一种错：`object_end()` 要求
栈顶**就是**对象（写的是 `[` 就是 `MISUSE`），`array_end()` 对称。

`finish()` 检查两件事：**栈已空**（所有容器都关了）与**至少写过一个根值**（不是空的）。
它不检查"你写的是不是一个有意义的 JSON 文档" —— 它只报 `UNCLOSED` 与 `MISUSE` 两种。

## 6. 失败是粘性的

**所有方法都不抛异常**（本仓的铁律：异常不得穿透 libuv 回调）。第一次失败之后，后续
调用**直接返回那个错误、不再动缓冲**（`src/uvcpp/uvcpp_json_writer.cpp:243` 的 `fail()`）。
挡住后续调用的是两处：`key()` / `object_end()` / `array_end()` 开口第一句就是
`if (status_ != OK) return status_;`，写值的那条路走 `check_value()`
（`src/uvcpp/uvcpp_json_writer.cpp:320`）的同一句。所以错误路径的代码不必层层检查。

```cpp
#include <uvcpp/uvcpp_json_writer.h>

#include <string>

bool doc_status(std::string& out) {
  uvcpp::uvcpp_json_writer w(out);
  uvcpp::json_write_status rc = w.object_begin();
  rc = w.value(1);                 // 对象里没先写键 ⇒ MISUSE
  if (rc != uvcpp::json_write_status::OK) return false;
  rc = w.value(2);                 // 粘性：这里拿到的还是 MISUSE，缓冲没动
  return rc == uvcpp::json_write_status::OK && w.ok();
}
```

**一旦返回非 OK，`str()` 里的内容就可能是个半截 JSON**（比如逗号已经写出去，才发现这个
位置不该有值）。这是流式构造器的固有代价：要么先建树（那正是本模块要避开的），要么允许
失败时丢弃缓冲。所以规矩是：**非 OK 就丢弃产出，别把它发出去**。

失败有五种（`json_write_status`，`src/uvcpp/uvcpp_json_writer.h:119`），
`json_write_status_name()` 可以把它们打成可读串：

| 值 | 什么时候 |
|---|---|
| `MISUSE` | 调用次序不合法：值没配键、键没配值、容器不配对、根值写了两次 |
| `TOO_DEEP` | 嵌套超过 `max_depth`（默认 64） |
| `TOO_LARGE` | 产出超过 `max_bytes`（默认 8 MiB） |
| `UNCLOSED` | `finish()` 时还有容器没关 |
| `NO_MEMORY` | 缓冲扩容失败（`std::bad_alloc`，以及其它异常一并归到这里） |

## 7. 数字

```cpp
#include <uvcpp/uvcpp_json_writer.h>

void doc_numbers(uvcpp::uvcpp_json_writer& w) {
  w.member("i32", static_cast<int>(-7));
  w.member("u64", static_cast<unsigned long long>(18446744073709551615ull));
  w.member("d", 0.1);                        // 最短可往返形态 ⇒ "0.1"
  w.member("f", static_cast<float>(0.1f));   // 按 float 的精度算往返
  w.key("n");
  w.null_value();                            // 值位上的 null
}
```

- **整数有八个宽度的重载**（`short` 到 `unsigned long long`），按你给的类型原样写十进制。
  自己选宽度：`size_t` 在 Linux 是 `unsigned long`、在 Windows 是 `unsigned long long`，
  两边都对，但**跨平台比对的 JSON 里最好显式用 `long long`**。
- **浮点先试"最短可往返"形态**（`format_fp()`，`src/uvcpp/uvcpp_json_writer.cpp:181`）：
  `%.15g`（`float` 是 `%.6g`）→ 用 `strtod` 读回来比对，不等再上 `%.17g`（`float` 是
  `%.9g`）。所以 `0.1` 写出来就是 `0.1`，而 π 会写成 17 位 —— 15 位回不到同一个 `double`。
- **`NaN` 与 `±Inf` 产出 `null`**：JSON 没有这三个字面量（与 nlohmann 的 `dump()` 同）。
  如果你需要"这两个值不一样"，得在调用方自己编码。
- **没有 `long double`**：`value(long double)` 不存在（在 MSVC 上它和 `double` 同宽，
  在别处不同宽，给一个"看着能用、跨平台精度不同"的重载比不给更坏）。

## 8. 上限与缓冲

- `max_depth` **默认 64**（`src/uvcpp/uvcpp_json_writer.h:137`），并被**夹到
  `[0, k_max_depth]`**（`k_max_depth` 也是 64，`src/uvcpp/uvcpp_json_writer.h:157`）。
  状态栈是**定长数组**（不分配是这个类的设计前提），所以夹到 64 这件事没有返回值可报
  —— 把 `max_depth` 设成 128 得到的仍然是一个能用的 64 层构造器。
  `max_depth = 0` 时只能写一个标量根值，`object_begin()` 会报 `TOO_DEEP`。
- `max_bytes` 默认 **8 MiB**，数的是**本 writer 自己写进去的字节数**，不是缓冲的总长度。
  判定在 `append_raw()`（`src/uvcpp/uvcpp_json_writer.cpp:250`）里，写法是
  `n > max_bytes_ - written_` —— **减法形态**，回绕不了。
- 构造时给一个 `std::string&` 就是**直写调用方的缓冲**：产出**追加**到它现有的内容之后，
  不读、不清空、不要求它是空的。`written()` 与 `max_bytes` 都只算自己写的那一段。

```cpp
#include <uvcpp/uvcpp_json_writer.h>

#include <cstddef>
#include <string>

std::size_t doc_reuse(std::string& scratch) {
  scratch += "prefix:";                   // 已有内容不会被清掉
  uvcpp::uvcpp_json_writer w(scratch);
  w.value(1);
  w.clear();                              // 只截回构造时的长度，前缀还在
  w.object_begin();
  w.member("k", 1);
  w.object_end();
  w.finish();
  return w.written();                     // 9：只数自己写的，不含 "prefix:"
}
```

`clear()`（`src/uvcpp/uvcpp_json_writer.cpp:601`）把缓冲**截回构造时的长度**而不是清空
整个串 —— 把前缀放在缓冲里是合法用法，`clear()` 不该顺手吃掉它。它是为"一个线程上一份
可复用的缓冲"设计的：省掉每次请求一次分配，而不是每次请求重建一个 writer。

`clear()` 也会把错误清掉、栈清空、`written()` 归零，所以失败之后可以原地重来。

## 9. 接到响应上

响应侧**不需要新 API**：产出是"已经序列化好的 JSON 串"，`json_str()` 就是给它准备的。

```cpp
#include <uvcpp/uvcpp_json_writer.h>
#include <webapp/uvcpp_web_response.h>

#include <cstddef>
#include <string>

void doc_into_response(uvcpp::uvcpp_web_response& resp, std::size_t size) {
  std::string body;
  uvcpp::uvcpp_json_writer w(body);
  w.object_begin();
  w.member("size", static_cast<long long>(size));
  w.object_end();
  if (w.finish() != uvcpp::json_write_status::OK) {
    resp.status(500).text("json build failed");
    return;
  }
  resp.json_str(w.str());     // 已序列化好 ⇒ 走 json_str，不走 json()
}
```

上面那段为什么有两个 `include`：**`webapp/uvcpp_web_response.h` 不含本模块的头**（两者的
依赖方向是"应用层用核心模块"，不是反过来），所以用到 writer 的翻译单元要自己包含它。
这也是本模块"不吃 webapp 门禁"的另一面 —— 顺序永远是核心模块在下面。

**为什么不做"零拷贝把那串交给 body"的入口**：`json_str()` 收 `const std::string&`
（`src/webapp/uvcpp_web_response.h:346`），一份小 JSON 的拷贝在这个尺度上量不出来，而多一个
入口就多一处能在失败路径上漏掉检查的地方。真在意那一次拷贝，用**外部缓冲**（§8）：把
每个连接自己那份 `std::string` 攒下来复用，省的是分配不是拷贝。

反过来，**nlohmann 那侧照样能用**：本模块负责产出，`uvcpp_json_parse()` 负责读回来
（两边的 `json_status` 是两个枚举，别混）。

```cpp
#include <webapp/uvcpp_web_json.h>

bool doc_round_trip(const std::string& text, long long& out_size) {
  uvcpp::uvcpp_json j;
  if (uvcpp::uvcpp_json_parse(text, j) != uvcpp::json_status::OK) return false;
  if (!j.is_object() || !j.contains("size")) return false;
  if (!j["size"].is_number_integer()) return false;
  out_size = j["size"].get<long long>();
  return true;
}
```

## 10. 线程与所有权

- **一个 writer 只属于一个线程**，不是在写中间共享。跨线程共享要调用方自己加锁，
  本模块不提供任何同步。
- 外部缓冲模式下 **`out` 必须比 writer 活得久**。本类不可拷贝、不可移动，
  所以这个前提不容易被意外破坏。
- 构造器**不分配**（状态栈是定长的成员数组）：唯一的分配来自产出缓冲的扩容，
  或者使用者选的那个缓冲。

## 11. 这份代码的判据

| 用例 | 它判什么 |
|---|---|
| `tests/functional/json_writer_func.cpp` | 逐字节判产出本身：结构/逗号、转义表逐项、数字的每个宽度、MISUSE 的每种形状、深度与大小上限、外部缓冲与 `clear()` |
| `tests/functional/web_app_json_writer_func.cpp` | 拿 **nlohmann 当独立见证**：产出能不能被另一个实现读回来，且**逐字节相等** |

第二条那一页里最要紧的是它的**对照臂**：手写的坏 JSON（值缺失、尾随逗号、括号不配对）
必须被解析器**拒**；以及"字符串里一个**裸**换行会被拒、而本模块写出的转义形态会被收"
—— 这两条一起，才说明这一层的转义是**承重**的，而不是装饰。（只判"解析成功"的话，
一个对什么都说 OK 的解析器能让它全绿。）

两页都跑过**变异核查**（改一处实现、用例必须红、再还原）：核心那条 9 个变异、
往返那条 5 个，逐个都把对应的那组判据跑红。

**诚实缺口**：转义的**跨度批量**（一段干净字节整段追加，而不是逐字节追加）只有
"产出逐字节正确"这一个见证，**效率没有独立读数** —— 它是个实现选择，不是契约。

## 12. 没做的（如实列出）

- **不解析。** 解析、DOM 查询、`dump()` 都在 nlohmann 那侧（webapp 模块）。
- **不校验 UTF-8**（§4）。
- **不转义 `/`**（§4，与 hical 的有意分歧）。
- **不给"裸字节值"**（`value_raw` 之类）：那等于把转义的责任又还给调用方，而转义正是
  本模块存在的理由。要拼已经序列化好的片段，请在**外面**拼，拼接点由你负责。
- **不给链式**（`member()` 除外，它是一个合并形态而不是链）。
- **不做美化输出**（没有缩进开关）。要人读的 JSON 请用别处。
- **失败之后产出不作保证**，也不支持"失败后继续"（§6）。
- **不认识任何"对象/结构体"**：本页只讲"手写字段"，把结构体整个写成 JSON 是**反射那一层**
  的事（它接在本模块的 writer 上，产出这一侧因此仍然是"无 DOM"的）—— 那一层有
  自己的一页：[`json-reflect-guide.md`](json-reflect-guide.md)。
