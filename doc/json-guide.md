# 应用层 JSON 构造器指南

`uvcpp_json_writer`（`src/uvcpp/uvcpp_json_writer.h`）是**把几个字段拼成一段 JSON** 的那一层。
它是**核心模块**：永远在编、零依赖、不吃任何 `UVCPP_ENABLE_*` 开关，也不需要 nlohmann。

这一页讲的是"怎么用它"，以及"它的边界在哪"。它不是 JSON **解析**指南 —— 解析与 DOM
在 `webapp/uvcpp_web_json.h`（nlohmann 后端），见 [`webapp-guide.md`](webapp-guide.md)；
本页 §13 讲**第二个**可选的解析后端（yyjson），它默认关着（**官方预编译包自 1.7.0
起开着**，见 §13.1）。

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
13. [第二个解析后端：yyjson（可选）](#13-第二个解析后端yyjson可选)

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
  `CMakeLists.txt:1512-1512` 那个收 `src/uvcpp/` 下源文件的 GLOB 已经把它收进去了。
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
（`src/uvcpp/uvcpp_json_writer.cpp:307`）：

**这张表只有一个消费者，但有两条搬运路径**：`append_escaped()` 走慢路（逐段追加，缓冲
快满时用），`escape_into()`（同文件 `src/uvcpp/uvcpp_json_writer.cpp:151`）走快路 ——
它把整段写进一块栈缓冲、再由调用方一次追加。两条路**共用这张表和下面那五个短形态、
小写十六进制常量**，所以选哪条只影响追加几次，不影响产出：同一个输入在两版上逐字节相同
（`tests/functional/json_writer_func.cpp` 的用例 2.13 / 2.14 就是钉这条的）。

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
调用**直接返回那个错误、不再动缓冲**（`src/uvcpp/uvcpp_json_writer.cpp:279` 的 `fail()`）。
挡住后续调用的是两处：`key()` / `object_end()` / `array_end()` 开口第一句就是
`if (status_ != OK) return status_;`，写值的那条路走 `check_value()`
（`src/uvcpp/uvcpp_json_writer.cpp:356`）的同一句。所以错误路径的代码不必层层检查。

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
- **浮点先试"最短可往返"形态**（`format_fp()`，`src/uvcpp/uvcpp_json_writer.cpp:217`）：
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
  判定在 `append_raw()`（`src/uvcpp/uvcpp_json_writer.cpp:286`）里，写法是
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

`clear()`（`src/uvcpp/uvcpp_json_writer.cpp:667`）把缓冲**截回构造时的长度**而不是清空
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

---

## 13. 第二个解析后端：yyjson（可选）

本页 §1–§12 讲的全是**产出**那一侧（`uvcpp_json_writer`，核心模块、零依赖、永远在编）。
**读入**那一侧在这之前只有一条路：`webapp/uvcpp_web_json.h` 的 nlohmann 后端 ——
header-only、语义完整、也**慢**：每个值占一个 `std::map` / `std::vector` 节点，一份
几 KB 的文档解析下来是成百上千次 `operator new`。

`src/yyjson/` 给同一件事提供**第二个后端**：`uvcpp_json_doc` 拿 [yyjson](https://github.com/ibireme/yyjson)
解析，然后可以

* 用与 nlohmann 路径**同名**的访问器取值（`uvcpp_json_get_string/bool/int/double`），
* 用同一套"查不到就给默认值"的语义遍历（`uvcpp_json_object_iter` / `at()`），
* 用**同一个**输出器写回去（`uvcpp_json_write` 把 DOM 喂给本页的 `uvcpp_json_writer`）。

也就是说：想换后端的人，改的是**变量的类型**，不是成片的调用点。

**这不是替换。** `using uvcpp_json = nlohmann::json` 一个字没动，`uvcpp_json_parse` /
`uvcpp_json_dump` / `uvcpp_json_get_*` 的行为一个字没改（nlohmann 那侧的重载仍在）。
两个后端可以同时出现在一个工程里，甚至一个函数里 —— `uvcpp_json_get_int` 有**两组**
重载，第一参数是 `uvcpp_json` 还是 `uvcpp_json_value` 就决定了走哪个后端。

### 13.1 怎么开

| | |
|---|---|
| CMake 选项 | `-DUVCPP_ENABLE_YYJSON=ON`，**默认 OFF**（但**发布包自 1.7.0 起开着**，见 §13.7） |
| 包含 | `#include <yyjson/uvcpp_json_doc.h>` |
| 依赖 | yyjson（单头 + 单 `.c`），**私有**：`<yyjson.h>` 只出现在本模块的 `.cpp` 里 |
| 关掉时 | `src/yyjson/` **整个不参与编译**，这个头里的声明也整段消失

最后一行要说清楚：关掉时 include 它**不是"降级"，是"这个模块不在库里"**（与 `webapp/`、
`wsdl/`、`quic/` 同形，报的是未声明的类型而不是"这个函数返回了什么默认值"）。判据是
**默认配置不受影响** —— 见 §13.5。

一处容易想反的地方，顺便记清楚：`cmake --install` 的规则挂在 `UVCPP_ENABLE_YYJSON` 上
（关掉时不装这个头），但**发布包是 `tests/tools/package_release.py` 按模块目录整体拷头
做出来的、不看开关** —— 所以一个把 yyjson 关掉的包里照样能找到
`include/yyjson/uvcpp_json_doc.h`，只是一个空的壳（宏为 0，里面一个声明都没有）。
真正拦住你的是宏，不是"包里没有这个文件"。（**官方预编译包自 1.7.0 起不关它**：
六条腿都传 `-DUVCPP_ENABLE_YYJSON=ON`，包里那个头的宏是 1。上面这句讲的是机制，
不是今天发布包的状态。）

`-DUVCPP_ENABLE_YYJSON=ON` 这个组合**在 CI 里真的有三格**（Ubuntu / macOS / MSVC）
—— 自 1.7.0 起发布腿也开着它，所以这三格是发布配置在 push/PR 阶段的替身，而"发布了
但没人编过"这句话对 yyjson **已经不再成立**（1.6.x 的包才是那个状态）。三格的门禁串、
装包表、以及为什么 macOS 那一格在而 `wsdl` 没有 macOS 格，见
[`ci-guide.md`](ci-guide.md) §5 的 `yyjson` 那一节。

### 13.2 五分钟上手

```cpp
#include <yyjson/uvcpp_json_doc.h>

#include <string>

// 把一份请求体解出来，取两个字段，再用本库自己的 writer 写回去。
std::string doc_roundtrip(const std::string& body, bool* ok) {
  uvcpp::uvcpp_json_doc doc;
  if (doc.parse(body) != uvcpp::json_doc_status::OK) {   // 失败时 doc 不被改动
    *ok = false;
    return std::string();
  }

  const uvcpp::uvcpp_json_value root = doc.root();
  long long size = 0;
  const bool has_size = uvcpp::uvcpp_json_get_int(root, "size", size);
  const std::string name =
      uvcpp::uvcpp_json_get_string(root, "name", "unnamed");  // 取不到给默认值

  std::string out;
  uvcpp::uvcpp_json_writer w(out);
  const uvcpp::json_write_status ws = uvcpp::uvcpp_json_write(root, w);

  *ok = has_size && (ws == uvcpp::json_write_status::OK);
  return out;
}
```

三处与 nlohmann 路径**不同名**的东西，都是刻意的：

- `uvcpp_json_doc` 是**拥有**整棵 DOM 的那个对象（不可拷贝、可移动，移动后源对象
  `valid() == false`，与 `std::unique_ptr` 同形）。nlohmann 那边 DOM 就是 `uvcpp_json` 本身。
- `uvcpp_json_value` 是**视图**：文档 `reset()` / 重新 `parse()` / 析构之后，之前拿到的
  值全部悬空（与 `std::string_view` 同形）。
- `json_doc_options` 与 webapp 的 `json_parse_options` 是**各自独立的类型**（本模块不依赖
  webapp，所以共用不了那一个），但**默认值相同**：深度 64、8 MiB。

### 13.3 与 nlohmann 路径**如实**存在的四处不同

四处都是**设计选择**、不是待修的差异。写在这里是为了让"换后端"这件事可预期：

| # | 这一处 | 本后端 | nlohmann 路径 |
|---|---|---|---|
| 1 | 成员顺序 | **文档里的顺序**（yyjson 按读入次序存） | 按键**排序**（对象类型是 `std::map`），`dump()` 出来也是排序的 |
| 2 | 重复键 | 取**第一个**（`obj_get` 语义） | 取**最后一个**（`std::map` 插入覆盖） |
| 3 | 数字写回去的形态 | `1.0` 写成 `1`；17 位有效数字的 double 写 17 位 | `dump()` 走自己的最短往返算法，写 `1.0` |
| 4 | 输出那一侧的递归 | `uvcpp_json_write` **递归**，深度由解析时的 `max_depth` 兜住 | `dump()` 也是递归的（同一条约束，只是默认深度不同） |

四条各自的判据：

1. 键序**不是数据**：`uvcpp_json_get_*` 与 `get(key)` 都不受它影响。要逐字节比两份输出，
   就得让源文档的键**本来就是有序的**（用例就是这么写的：五份语料的键全部升序排）。
2. 两边都**接受**重复键（都报 OK），只是取值不同。RFC 8259 里重复键"行为不可移植"，
   所以这不是谁对谁错；用例把**两边各自的行为都钉住**，而不是假装它们一样。
3. 这是 `uvcpp_json_writer` 早就定下的行为（见 §7），**值相同、字节可能不同**。本模块不改它，
   也不为它背锅；要让数字形态也一致，走 nlohmann 那一半。
4. 深度两边都用 `<uvcpp/uvcpp_json_scan.h>` 预扫描，所以 `TOO_DEEP` 是**同一条线**；
   但把 `max_depth` 设到几千以上、又拿 `uvcpp_json_write` 去写那份文档，栈是你自己的。

### 13.4 状态与 nlohmann 路径的对照

`json_doc_status` 是 `json_status` 前五项的同义物（一样的判定顺序、一样的边界）：

| 本后端 | nlohmann 路径 | 判定 |
|---|---|---|
| `EMPTY` | `EMPTY` | `data == nullptr \|\| len == 0` |
| `TOO_LARGE` | `TOO_LARGE` | `len > max_bytes` |
| `TOO_DEEP` | `TOO_DEEP` | `uvcpp_json_max_nesting() > max_depth` |
| `SYNTAX` | `SYNTAX` | 其余一切解析失败（含纯空白输入 —— 它不是"空"，是"不是 JSON"） |
| `NO_MEMORY` | （`bad_alloc`） | 分配失败：本后端**不抛异常**，报这个 |

不在这张表里的三个（`MISMATCH`/`MISSING`/`UNKNOWN`）来自 webapp 的**反射读入**，不属于解析。

### 13.5 判据

| 用例 | 它判什么 |
|---|---|
| `tests/functional/yyjson_doc_func.cpp` | 下面这一整列 |

* **与 nlohmann 路径状态逐条一致**：一张 25 行的表（16 条坏输入 + 边界：空、纯空白、超长、
  超深、BOM、非法 UTF-8、重复键、64 与 65 层、又长又深、又深又坏、`max_depth = 0`），
  每条**先与手写期望值比**，再（webapp 开着时）**与 nlohmann 逐条比**。判"两边一样"而不判
  "其中一边是什么"，是特意的：一个对什么都说 OK 的解析器让两边都错也能全绿。
* **逐字节相同**：五份语料（转义、控制字符与 UTF-8、整数两端、浮点、数组）的键全部升序排，
  于是 `uvcpp_json_write` 的产出与 nlohmann `dump()` **逐字节相等**。这半边另有一条
  **反空洞**断言：先确认 nlohmann 侧产出**非空且不是 `null`**，否则两边一起坏也判绿。
* **数字语义相同**：把两边输出都交给 nlohmann 再比 `==`，覆盖 `1.0` / `0.0` / `-0.0` /
  17 位 double / denormal / 超 uint64 的大整数；另有一条专门钉住第 3 条不同是**真的**
  （`{"v":1.0}`：本后端 `{"v":1}`、nlohmann `{"v":1.0}`）。
* **`memory_bytes()` 的可测形式**：空文档是 0、`reset()` 后回到 0、反复失败（200 次）后
  文档**逐字节不变**（与一份新解析的同文文档比）、移动之后不重复计数。
* **解析器不递归**：十万层的输入，把上限抬到它之上就真解析（而不是爆栈）。这一条是
  "本后端能报 TOO_DEEP"之外的另一半 —— 深度是**策略**，栈是**物理**，两者都要有见证。

**吞吐与常驻内存不在门禁里**，它们是这一节的读数（§13.6）。压测数字进用例就是造一条
会随机变红的门禁。

### 13.6 量出来的两个数

量具在**仓外**（`/tmp/yyexp/bench.cpp`，不进仓库）：200 次预热 + 5 轮 × 20,000 次取最好，
`taskset -c 0-3`。语料是一份 `{"items":[{id,name,category,price,quantity,active,tags[]}…],"total":n}`
（键按升序写，好让两边的产出可比），三档规模。

**解析吞吐**（ns/次，越小越好）。两个口径都给：**库入口**是
`uvcpp_json_parse(s, doc)` 对 `doc.parse(s)`，**纯解析**是 `nlohmann::json::parse` 对
`doc.parse(s)` —— 前者多含 webapp 那层的门（EMPTY/TOO_LARGE/TOO_DEEP 预扫描 +
`is_discarded()`），所以两个数不一样，两个都写出来免得把"入口的开销"记成"解析器的差距"。

| 语料 | 库入口 | 纯解析 | 小文档（220 B，一次请求体那种） |
|---|---|---|---|
| 1,147 B / 10 项 | 21,070 → 1,569（**13.4×**） | 15,184 → 1,569（9.7×） | 2,510 → 226（11.1×） |
| 5,727 B / 50 项 | 114,634 → 7,478（**15.3×**） | 79,311 → 7,478（10.6×） | 2,502 → 228（11.0×） |
| 22,973 B / 200 项 | 451,279 → 30,863（**14.6×**） | 315,150 → 30,863（10.2×） | 2,487 → 216（11.5×） |

**DOM 常驻内存**（同一份文档解析完，峰值）：

| 语料 | nlohmann | yyjson | 倍数 | 分配次数 |
|---|---|---|---|---|
| 1,147 B | 8,312 B（7.25× 输入） | 4,367 B（3.81× 输入） | **1.9×** | 318 → 2 |
| 5,727 B | 40,632 B（7.09× 输入） | 21,155 B（3.69× 输入） | **1.9×** | 1,481 → 2 |
| 22,973 B | 161,832 B（7.04× 输入） | 84,385 B（3.67× 输入） | **1.9×** | 5,835 → 2 |

**这两列不是同一把尺子的读数**，必须写出来：nlohmann 那一列来自量具里全局覆盖的
`operator new`（数的是**申请**字节），yyjson 那一列来自后端的 `memory_bytes()`（数的是
**它自己的分配器实测**）—— 因为 yyjson 走的是 `std::malloc`，全局 `operator new` 覆盖
**看不到它**（这是量具的一处真实盲区，值得单独记）。两个口径都是"这次解析向系统要了多少
字节"，量级可比，但**不是逐字节同源**。要一把真正统一的尺子，得给 yyjson 也包一层
`operator new` —— 那要求它用 C++ 的分配路径，代价比这个数大。

### 13.7 成本与诚实缺口（如实列出）

- **多一个第三方依赖**（yyjson 0.13.0）。只在 ON 时进构建，OFF 时**一个目标文件都不编**；
  取源码用的是 `FetchContent_Populate` 而不是 `MakeAvailable`，所以 yyjson 自己那套
  `install()` 规则**不会**被登记（上游 `YYJSON_INSTALL` 默认是 **ON**）。
- **默认 OFF，但它在 CI 与发布腿上都真的被编**：`yyjson` 三格（Ubuntu / macOS / MSVC）
  的 `flags` = `web` 加一个 `-DUVCPP_ENABLE_YYJSON=ON`，而**六条发布腿自 1.7.0 起
  也传同一个 `-D`** —— 所以官方预编译包里 `UVCPP_YYJSON_ENABLE` 是 1，模块真的在库里
  （1.6.x 及以前不是：那时包里只有一个空壳头，这三格是它唯一的编译处）。三格仍然不是
  可选的：发布腿只在打标签那天跑，push/PR 阶段靠的就是它们。四道门禁串与"为什么是这
  三格"写在 [`ci-guide.md`](ci-guide.md) §5 的 `yyjson` 那一节；macOS 那一格在的理由是
  **它用 libc++**，而写回逐字节那条正好是跨 libstdc++/libc++ 会分叉的那类断言。
- `check_doc_snippets.py` 里本页那几条片段曾经对**发布包**判 `[跳·默认关]`
  （`DEFAULT_OFF` 里的 `UVCPP_YYJSON_ENABLE`）。1.7.0 起发布包开着 yyjson，**那条豁免
  同批删掉了** —— 现在本页片段拿官方包跑是真编真判的（§13.7 末条那个手工验证就是
  用一棵 `-DUVCPP_ENABLE_YYJSON=ON` 的树装出来的前缀做的）。
- 上面**手工验过一次**：本机 `-DUVCPP_ENABLE_YYJSON=ON` 的树 `cmake --install` 到
  一个临时前缀（`include/yyjson/uvcpp_json_doc.h` 与 `include/uvcpp/uvcpp_json_scan.h`
  都在），拿该前缀跑 `python3 tests/tools/check_doc_snippets.py --pkg <前缀>` ——
  **本页这几条片段（含 §13.2 那条）判绿**。同一棵树上的
  `test_yyjson_doc_func` 也是 ALL PASS（全量 93/93）；四个 CI 门禁串也按 `yyjson`
  那一格的原命令行在本机复现过。**豁免删掉之后又按发布包的形状验了一遍**
  （`package_release.py --platform linux-x64` 出的那棵树，包内
  `UVCPP_YYJSON_ENABLE` 是 1）：`check_doc_snippets.py --pkg …` ⇒ 本页
  **编过 8/8 条**、全库 `编过 102/202 条`、`rc=0`。反向对照也做过：拿一棵
  yyjson 关着的旧包跑同一条命令，本页第 402 行那条片段变成 `[跳]`、整条**退 3**
  —— 这正是三个 config-contract 档必须跟着传 `-DUVCPP_ENABLE_YYJSON=ON` 的原因。
- **对照那一半需要 webapp**：nlohmann 是 webapp 的依赖，`UVCPP_BUILD_WEBAPP=OFF` 的树上
  那条 include 不存在。用例把那半整段套在 `#if UVCPP_WEBAPP_ENABLE` 里，并打印一行
  **显眼的 `[SKIP]`** —— 不是静默通过（本仓最忌讳的那种绿）。
- **不给"换后端"提供自动迁移**：两边的类型名不同（`uvcpp_json` / `uvcpp_json_doc`），
  换的时候编译器会指出来；但不提供兼容层，也不打算把默认后端换掉。
