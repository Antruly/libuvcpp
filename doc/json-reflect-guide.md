# JSON 反射指南

一个宏标出字段表，两个方向各一次调用：

```cpp
#include <uvcpp/uvcpp_json_reflect.h>

#include <string>

struct user {
  std::string name;
  int         age;
  UVCPP_JSON_FIELDS(user, name, age)
};
```

写出去是 `uvcpp_to_json(u, writer)`，读回来是 `uvcpp_from_json(dom, u)`。**没有中间 DOM、
不用手写字段名、不用为每个结构体单独写一遍解析**。

这一页讲"怎么用"和"它的边界在哪"。它不是 [JSON 构造器](json-guide.md)的替代品 ——
写出侧的最后一公里仍然落在那个 writer 上，本页只是把"哪些字段、按什么顺序"这件事
从手写挪到了宏里。

## 目录

1. [这一层解决什么](#1-这一层解决什么)
2. [编译期条件与包含](#2-编译期条件与包含)
3. [五分钟上手](#3-五分钟上手)
4. [字段表：顺序来自宏](#4-字段表顺序来自宏)
5. [写出侧：每个类别](#5-写出侧每个类别)
6. [读入侧的三条语义](#6-读入侧的三条语义)
7. [失败怎么报](#7-失败怎么报)
8. [不抛异常](#8-不抛异常)
9. [C++11 的代价](#9-c11-的代价)
10. [接到请求与响应上](#10-接到请求与响应上)
11. [这份代码的判据](#11-这份代码的判据)
12. [没做的（如实列出）](#12-没做的如实列出)

---

## 1. 这一层解决什么

| 做法 | 一个结构体的代价 |
|---|---|
| `resp.json_str("{\"name\":\"" + u.name + "\",\"age\":" + std::to_string(u.age) + "}")` | 字段名硬编码在字符串里，转义全靠人；**加一个字段要改两处** |
| 手搓 `uvcpp_json`（nlohmann）再 `dump()` | 要先建一棵树；读回来还要 `j["age"].get<int>()` 一句一句写，**字段名再抄一遍** |
| `UVCPP_JSON_FIELDS` + 两次调用 | 字段名只在宏里出现一次，两个方向共用同一张表 |

第三行的"同一张表"是要点：**写出与读入的字段顺序不可能走散**，因为它们读的是同一个
`std::tuple`。手写那两种做法里，"解析时写错一个字段名"是个纯拼写错误，编译器不管。

**这一层不是 hical 的移植。** hical 的 `HICAL_JSON` 长得一样，但它跑在 **C++20** 上
（`requires` 表达式、`std::void_t`、`if constexpr`），而本仓是 **C++11**（`CMakeLists.txt`
里 `CMAKE_CXX_STANDARD 11`，没有 per-target 覆盖）。能借的只有形状：字段描述器
（名字 + 成员指针）、SFINAE 探测有没有字段表、按下标展开元组。§9 讲 C++11 化时多出来的
那件手工活。

两处**有意与 hical 不同**：

- hical 的 `fromJson` 失败时**抛 `std::runtime_error`**。本仓铁律是"异常不得穿透 libuv
  的回调"（一个 HTTP 处理函数里抛出去就是 `std::terminate`），所以本层一律返回
  `json_status`，与 `uvcpp_json_parse` 同形。
- hical 的字段描述器有三档（`name` / `required` / `ignored`），本层**只有 name**。
  required 换成了**整份读入的全局开关**（§6.1）—— 见 §12 的"没做的"。

## 2. 编译期条件与包含

**这一层分在两层，这是本页最要紧的一条。** 两个头各在一个模块：

| 方向 | 头 | 模块 | 依赖 |
|---|---|---|---|
| 写（结构体 → JSON） | `src/uvcpp/uvcpp_json_reflect.h` | **核心** | 只要 `uvcpp_json_writer`，零依赖 |
| 读（JSON → 结构体） | `src/webapp/uvcpp_web_json_reflect.h` | **webapp** | 要一棵解析好的 DOM，而本仓的 DOM 就是 `uvcpp_json`（nlohmann 后端） |

为什么不能合成一个头：读入侧的输入类型 `uvcpp_json` **整个挂在 webapp 模块上**，而
`UVCPP_BUILD_WEBAPP` **默认 OFF**。合成一个头的话，"只想写不想读"的核心工程会被迫
拉进 nlohmann —— 那正是写出侧在任务 5 里挣来的那个杠杆（**不吃 webapp 门禁**）。

于是依赖方向只有一条：`webapp/uvcpp_web_json_reflect.h` include `uvcpp/uvcpp_json_reflect.h`，
**不是反过来**。用读入侧的翻译单元要自己带两个头（或者带 webapp 那个，它顺带拉了核心那个）。

```cpp
#include <uvcpp/uvcpp_json_reflect.h>
#include <webapp/uvcpp_web_json_reflect.h>

#include <string>

struct doc_thing {
  std::string name;
  int         age;
  UVCPP_JSON_FIELDS(doc_thing, name, age)
};
```

两个头都**不在 `<uvcpp.h>` 里**：那个聚合头只收 `uvcpp_define/version/export` 与
`handle/loop/req` 三组。两个头都被 `CMakeLists.txt:472` 与 webapp 那份同形的 GLOB 自动
收进构建与安装，**不需要往任何清单里加一行**。

- **没有开关。** 写出侧不吃 `UVCPP_ENABLE_*` 任何一个；读入侧只吃它本来就依赖的
  `UVCPP_BUILD_WEBAPP`（与 nlohmann 同一道门）。
- **C++11**，所以没有 `if constexpr`、没有 `std::void_t`、没有 `std::make_index_sequence`。
- 导出的东西：写出侧 `uvcpp_to_json`、`uvcpp_json_field`、`uvcpp_make_field`、
  `UVCPP_JSON_FIELDS`；读入侧 `uvcpp_from_json`、`uvcpp_from_json_options`。

## 3. 五分钟上手

### 写出去

```cpp
#include <uvcpp/uvcpp_json_reflect.h>

#include <string>

struct doc_user {
  std::string name;
  int         age;
  UVCPP_JSON_FIELDS(doc_user, name, age)
};

std::string doc_write_user(const doc_user& u) {
  uvcpp::uvcpp_json_writer w;
  if (uvcpp::uvcpp_to_json(u, w) != uvcpp::json_write_status::OK) return std::string();
  if (w.finish() != uvcpp::json_write_status::OK) return std::string();
  return w.str();          // {"name":"...","age":30}
}
```

产出直接落在 writer 的缓冲里 —— **没有中间表示**，也就是 §11 里说的"写出侧无 DOM"。

### 读回来

```cpp
#include <uvcpp/uvcpp_json_reflect.h>
#include <webapp/uvcpp_web_json.h>
#include <webapp/uvcpp_web_json_reflect.h>

#include <string>

struct doc_user {
  std::string name;
  int         age;
  UVCPP_JSON_FIELDS(doc_user, name, age)
};

uvcpp::json_status doc_read_user(const std::string& body, doc_user& out,
                                 const char** bad_field) {
  uvcpp::uvcpp_json dom;
  const uvcpp::json_status parse_st = uvcpp::uvcpp_json_parse(body, dom);
  if (parse_st != uvcpp::json_status::OK) return parse_st;   // 报文本身的问题
  return uvcpp::uvcpp_from_json(dom, out, uvcpp::uvcpp_from_json_options(), bad_field);
}
```

两个返回值要分开看：`EMPTY` / `SYNTAX` / `TOO_DEEP` / `TOO_LARGE` 是**报文**的问题
（通常回 400/415），`MISMATCH` / `MISSING` / `UNKNOWN` 是**字段**的问题（通常回 400 并带上
`bad_field`）。两者共用一个枚举，就是为了让调用处一个 `switch` 能覆盖全部失败形状。

## 4. 字段表：顺序来自宏

`UVCPP_JSON_FIELDS(Type, ...)` 展开成三样东西：一个成员 typedef（元组的类型）、一个
静态成员函数 `uvcppJsonFields()`（返回那张表）、以及表本身。宏的**第一个参数必须是
当前类型自己的名字**（成员指针的类型要用它拼出来），所以它出现在两个地方。

顺序**由宏里列出的顺序决定**，与成员在结构体里声明的顺序无关：

```cpp
#include <uvcpp/uvcpp_json_reflect.h>

#include <string>

struct doc_reordered {
  int a;
  int b;
  int c;
  UVCPP_JSON_FIELDS(doc_reordered, c, b, a)
};

std::string doc_show_order() {
  doc_reordered r;
  r.a = 1;
  r.b = 2;
  r.c = 3;
  uvcpp::uvcpp_json_writer w;
  if (uvcpp::uvcpp_to_json(r, w) != uvcpp::json_write_status::OK) return std::string();
  w.finish();
  return w.str();          // {"c":3,"b":2,"a":1}
}
```

**读入侧用的是同一张表**，所以 `where`（§7）报出来的"第一个缺的字段"也按宏的顺序，
不是声明顺序。这条是契约的一部分：只判"产出里有没有这些字段"的话，两种实现都绿 ——
有用例专门用"声明顺序与宏顺序相反"的结构体把这件事钉住。

上限是 **16 个字段**（宏按参数个数分派到 `UVCPP_JSON_MAP_1` … `UVCPP_JSON_MAP_16`）。
超了是编译期错误（宏展开不出对应的那个名字），不是运行时。

## 5. 写出侧：每个类别

| C++ 类型 | 产出 |
|---|---|
| `bool` | `true` / `false` |
| 有符号整型（`signed char` / `short` / `int` / `long` / `long long` + `char`） | 十进制数，走 `long long` 那个重载 |
| 无符号整型 | 十进制数 |
| `float` | 走 **`float`** 重载（`%.9g`） |
| `double` / `long double` | 走 **`double`** 重载（`%.17g`） |
| `std::string` / `const char*` / `char*` | JSON 字符串；**指针是 `nullptr` 时产出 `null`** |
| 标了 `UVCPP_JSON_FIELDS` 的结构体 | JSON 对象，递归 |
| 上面任一种的 `std::vector<T>` | JSON 数组 |

```cpp
#include <uvcpp/uvcpp_json_reflect.h>

#include <string>
#include <vector>

struct doc_point {
  int x;
  int y;
  UVCPP_JSON_FIELDS(doc_point, x, y)
};

struct doc_all {
  bool                       flag;
  long long                  big;
  unsigned short             small;
  float                      ratio;
  double                     score;
  std::string                text;
  const char*                maybe;   // 可为 nullptr
  doc_point                  pos;
  std::vector<int>           tags;
  std::vector<doc_point>     pts;
  UVCPP_JSON_FIELDS(doc_all, flag, big, small, ratio, score, text, maybe, pos, tags, pts)
};
```

`float` 与 `double` **故意分两路**：writer 的"最短往返"判据是按**原类型**的精度算的。
把 `float` 转成 `double` 再写，`0.1f` 会写成 `0.10000000149011612` —— 数值没错，但产出
又多又难看，而且与直接写 `0.1f` 不一致。

`long double` **能编，但走 `double` 那一支（掉精度）**。见 §12。

## 6. 读入侧的三条语义

三条都是**有意的**，不是实现细节。写代码前先认下这三条。

### 6.1 没给的字段不动原值

**缺字段**与**值是 `null`** 都算"没给"：目标里的原值保持不动。于是可以先把默认值放进
结构体，JSON 里出现的字段覆盖它、没出现的保持原样 —— 这是"部分更新"的形状，也是最不
容易踩坑的一种（对比：把缺失当零值填进去，会把"没设"写成"设成了 0"）。

```cpp
#include <uvcpp/uvcpp_json_reflect.h>
#include <webapp/uvcpp_web_json.h>
#include <webapp/uvcpp_web_json_reflect.h>

struct doc_pair {
  int a;
  int b;
  UVCPP_JSON_FIELDS(doc_pair, a, b)
};

uvcpp::json_status doc_partial_update(const std::string& body, doc_pair& out) {
  out.a = 7;                 // 默认值
  out.b = 8;
  uvcpp::uvcpp_json dom;
  const uvcpp::json_status st = uvcpp::uvcpp_json_parse(body, dom);
  if (st != uvcpp::json_status::OK) return st;
  // body 是 {"b":20} ⇒ out.a 还是 7，out.b 变成 20
  return uvcpp::uvcpp_from_json(dom, out);
}
```

想要"缺一个都不行"就把 `missing_is_error` 打开：

```cpp
#include <uvcpp/uvcpp_json_reflect.h>
#include <webapp/uvcpp_web_json.h>
#include <webapp/uvcpp_web_json_reflect.h>

struct doc_pair {
  int a;
  int b;
  UVCPP_JSON_FIELDS(doc_pair, a, b)
};

uvcpp::json_status doc_strict(const uvcpp::uvcpp_json& dom, doc_pair& out,
                              const char** bad_field) {
  const uvcpp::uvcpp_from_json_options opts(true);    // missing_is_error = true
  return uvcpp::uvcpp_from_json(dom, out, opts, bad_field);
}
```

打开之后缺字段返回 `json_status::MISSING`，`where` 给**第一个**缺的字段名。

### 6.2 类型不对一律报错，不做隐式转换

`"age":"30"` 是 `MISMATCH`，**不会**被解析成 30。`true` 读进整型也是 `MISMATCH`。
唯一放宽的一条是**整数读进浮点字段**（JSON 本来就不分 int / double，`{"score":1}` 该收）。

整数读进整数**逐宽度查上下界**：`70000` 读进 `short` 是 `MISMATCH`，**不是静默截成
`4464`**；`-1` 读进 `unsigned int` 也是 `MISMATCH`。这件事只有"值域边界两侧各来一发"
判得住 —— 用例里就是这么写的。

浮点读进整型**不收**（`1.5` 进 `int` 是 `MISMATCH`）—— 那才是需要隐式截断的方向。

### 6.3 多出来的成员默认忽略

```
{"a":1,"b":2,"nope":3}   →   OK（nope 被忽略）
```

打开 `unknown_is_error` 才报 `UNKNOWN`。**默认关掉它有代价上的理由**：打开之后每读一次
都要拿 DOM 的每个成员去字段表里逐个比，是 `O(成员 × 字段)`。

```cpp
#include <uvcpp/uvcpp_json_reflect.h>
#include <webapp/uvcpp_web_json.h>
#include <webapp/uvcpp_web_json_reflect.h>

struct doc_pair {
  int a;
  int b;
  UVCPP_JSON_FIELDS(doc_pair, a, b)
};

uvcpp::json_status doc_strict_unknown(const uvcpp::uvcpp_json& dom, doc_pair& out) {
  const uvcpp::uvcpp_from_json_options opts(false, true);   // unknown_is_error = true
  return uvcpp::uvcpp_from_json(dom, out, opts);
}
```

两个开关是**整份读入的**，不是 per-field —— §12。

## 7. 失败怎么报

三样东西，都不抛异常（§8）：

1. **返回码**：第一个出错的那一步的 `json_status`。字段展开是**粘性**的：错一次就停，
   后面的字段**根本不再读**（所以"第一个缺的字段"报的就是字段表里最靠前的那个，
   而不是"缺的里面最严重的那个"）。
2. **`where`**：出错字段的名字。静态字符串（宏给的就是字面量 `#字段`），**可以直接
   交出去，不必拷贝、不会悬垂**。嵌套结构体里出错给的是**最深那一层**的名字，
   不是外层那个：

```cpp
#include <uvcpp/uvcpp_json_reflect.h>
#include <webapp/uvcpp_web_json.h>
#include <webapp/uvcpp_web_json_reflect.h>

#include <string>

struct doc_point {
  int x;
  int y;
  UVCPP_JSON_FIELDS(doc_point, x, y)
};

struct doc_shape {
  std::string name;
  doc_point   pos;
  UVCPP_JSON_FIELDS(doc_shape, name, pos)
};

uvcpp::json_status doc_where(const std::string& body, const char** bad_field) {
  uvcpp::uvcpp_json dom;
  uvcpp::json_status st = uvcpp::uvcpp_json_parse(body, dom);
  if (st != uvcpp::json_status::OK) return st;
  doc_shape s;
  // body 是 {"name":"n","pos":{"x":"bad"}} ⇒ st 是 MISMATCH，*bad_field 是 "x"
  st = uvcpp::uvcpp_from_json(dom, s, uvcpp::uvcpp_from_json_options(), bad_field);
  return st;
}
```

   多成员那条路上，`where` 指向的是 **DOM 里的那个键**（字段表里没有的名字）——
   它活到 `dom` 还在为止。

3. **`out` 可能被改了一部分**。前几个字段已经写进去了、后一个失败 —— 这是文档化的行为，
   不是疏漏。"全成功或全不动"要先建一份临时对象再搬，那是另一层代价。规矩与写出侧
   一致：**非 OK 就丢弃产出**。

   这条容易被当成缺陷"修掉"，所以用例里**专门为它写了断言**。真有人改成原子语义，
   那几条会红 —— 那时应当改的是文档，不是把实现改回去。

## 8. 不抛异常

nlohmann 的 `get<T>()` 在类型不符时会抛、越界也会抛。本层**先看类型再取值**
（`is_boolean()` / `is_number_unsigned()` / `is_string()` …），所以那些抛点根本到不了。
按存储类型分两支走（`is_number_unsigned()` 必须先判 —— nlohmann 的 `is_number_integer()`
**对无符号也为真**），取的都是那个类型的精确类型，于是取值这一步没有抛点。

剩下唯一可能的异常是**分配失败**：`std::string` 的赋值、`std::vector` 的 `push_back`。
公开入口 `uvcpp_from_json` 处接住 `std::bad_alloc` 并返回 `json_status::NO_MEMORY`。

写出侧同理：writer 自己是粘性的、不抛（见 [JSON 构造器指南](json-guide.md) §6）。

## 9. C++11 的代价

`std::make_index_sequence` 是 **C++14** 才有的。而"按下标展开一个元组"正是本模块的核心
手法（字段表是个**异构**元组，每个元素的类型都不一样，取元素只能靠下标），所以必须自己
造一个。这是本层唯一的技术活：

```cpp
#include <uvcpp/uvcpp_json_reflect.h>

#include <type_traits>

// 升序是要紧的：降序也能编过，但写出字段的顺序就反了。
// 两个名字都在 `uvcpp::json_detail` 里 —— `_detail` 后缀是"这是内部件"的标记，
// 写应用代码时用不到它们，这一页把它们摆出来只是因为它们**是**这一层的手工活。
static_assert(
    std::is_same<uvcpp::json_detail::uvcpp_make_index_seq<4>,
                 uvcpp::json_detail::uvcpp_index_sequence<0, 1, 2, 3> >::value,
    "uvcpp_make_index_seq<N> 必须是 0, 1, ..., N-1");
```

形状是标准的递归：`N` 逐个减到 0，每减一层把当前值**贴到前面**
（`src/uvcpp/uvcpp_json_reflect.h:133`），于是最终的参数包是升序的。展开元组用的是
`int swallow[] = { 0, (rc = 写一个字段, 0)... };` —— 花括号初始化列表的元素求值顺序是
**左到右**，逗号表达式又是序列点，两条合起来才保证字段按顺序写出。这是 C++11 里
"按顺序展开一个包"的标准手法，不是可有可无的写法。

同一个探测用什么？C++11 没有 `std::void_t`。本层用一个只有成员 typedef 的空模板
（C++11 版 void_t）配上 `decltype(T::uvcppJsonFields())` 的偏特化做 SFINAE 探测
"这个类型有没有字段表"。**没标宏的类型调 `uvcpp_to_json` / `uvcpp_from_json` 是编译期
错误**，不是运行时报错。

## 10. 接到请求与响应上

响应侧**不需要新 API**：产出是"已经序列化好的 JSON 串"，`json_str()` 就是给它准备的。

```cpp
#include <uvcpp/uvcpp_json_reflect.h>
#include <webapp/uvcpp_web_response.h>

#include <string>

struct doc_user {
  std::string name;
  int         age;
  UVCPP_JSON_FIELDS(doc_user, name, age)
};

void doc_user_response(const doc_user& u, uvcpp::uvcpp_web_response& resp) {
  std::string body;
  uvcpp::uvcpp_json_writer w(body);          // 外部缓冲：由调用方持有
  if (uvcpp::uvcpp_to_json(u, w) != uvcpp::json_write_status::OK ||
      w.finish() != uvcpp::json_write_status::OK) {
    resp.status(500).text("json build failed");
    return;
  }
  resp.json_str(w.str());     // 已序列化好 ⇒ 走 json_str，不走 json()
}
```

请求侧有一个一步到位的重载（`src/webapp/uvcpp_web_request.h:385`）：解析 body + 按字段表
填结构体，返回 `bool`：

```cpp
#include <uvcpp/uvcpp_json_reflect.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_json.h>
#include <webapp/uvcpp_web_response.h>

#include <string>

struct doc_user {
  std::string name;
  int         age;
  UVCPP_JSON_FIELDS(doc_user, name, age)
};

void doc_handle(const uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp) {
  doc_user u;
  u.name = "anonymous";       // 默认值，JSON 里给了就覆盖（§6.1）
  u.age  = 0;
  uvcpp::json_status st = uvcpp::json_status::OK;
  const char* bad_field = nullptr;
  if (!req.json(u, &st, &bad_field)) {
    if (st == uvcpp::json_status::EMPTY || st == uvcpp::json_status::SYNTAX) {
      resp.bad_request().text("body is not JSON").end();
      return;
    }
    resp.bad_request().text(std::string(uvcpp::json_status_name(st)) + ": " +
                            (bad_field != nullptr ? bad_field : "?"))
        .end();
    return;
  }

  uvcpp::uvcpp_json_writer w;
  if (uvcpp::uvcpp_to_json(u, w) == uvcpp::json_write_status::OK &&
      w.finish() == uvcpp::json_write_status::OK) {
    resp.json_str(w.str());
  } else {
    resp.status(500).text("json build failed");
  }
}
```

`req.json(u, ...)` 里的 DOM 是**那个函数里的临时对象**：所有值都**拷进** `u` 了，
没有指向 DOM 的视图，所以 `u` 里不会有悬垂引用。（`const char*` 字段读不进来，
所以不存在"指向临时 DOM 的指针"这种形状 —— 见 §12。）

## 11. 这份代码的判据

| 用例 | 它判什么 |
|---|---|
| `tests/functional/json_reflect_func.cpp` | 序列化侧：手写 `index_sequence` 的升序、字段顺序来自宏（用"声明顺序与宏顺序相反"的结构体）、产出逐字节、八个整型宽度、`float`/`double` 分路、C 字符串 `nullptr`、vector（空 / 嵌套 / 套结构体）、错误码的传递 |
| `tests/functional/web_app_json_reflect_func.cpp` | 读入侧：writer→读回的往返（期望串手写）、每个类别的值（含内嵌 NUL 与 `\uXXXX`）、逐宽度的值域两侧、类型不符的每个方向、缺失 / `null`、多成员、容器（含空数组必须清空）、四个新状态名、`req.json()` 入口 |

两个文件的**命名**是有讲究的：写出侧那条**不带** `web_` / `web_app_` / `web_ssl_` / `h2_`
任何前缀（它只用核心模块，与四个开关都无关）；读入侧那条**必须**带 `web_app_`，而且
**故意不带** `web_ssl_` / `h2_`。`tests/functional/CMakeLists.txt` 里记着那个老坑：
名字没命中它所属的每一道过滤器时，用例会"注册、通过、一个断言都没跑"。

两页都跑过**变异核查**（改一处实现、用例必须变红、再还原）：这条链上一共 11 个变异
（写出侧 5 个、读入侧 6 个），逐条都把对应那组判据跑红。**变异台本身也交回了一条真发现**：
"负的有符号数进无符号目标"原先**判不出来** —— 对窄目标（`unsigned char` / `short` /
`int`），"先查 `s < 0`"与"再把 `s` 转成无符号比上界"是**重合**的，少了前者后者照样把
`-1` 挡下来；只有 64 位那档的上界恰好是 `ULLONG_MAX`，`(unsigned long long)(-1)` 正好
等于它、**不大于它**，于是少一行就会把 `-1` 静默写成 `18446744073709551615`。补了一条
`unsigned long long` 拒 `-1` 的断言之后它才红。**这类缺口靠读实现是读不出来的。**

## 12. 没做的（如实列出）

- **没有 per-field 的 required / 别名 / 校验**。缺字段算不算错是**整份读入**的开关
  （`uvcpp_from_json_options`），不是字段属性；不支持"这个字段叫 `userName` 也认"，
  也不做区间/正则之类的约束。想校验请在读入之后自己判。
- **不支持的类型**（在**编译期**被 `static_assert` 拦下，不是运行时返错）：C 数组、
  `std::map`、`std::optional`、枚举、除 `std::string` / C 字符串指针以外的指针。
- **`const char*` / `char*` 能写不能读**。它不是个能持有内存的字段：让它指向 DOM 里的
  字符串，DOM 一析构就悬垂；"指向谁"是使用者的事，不是库能替他决定的。改成
  `std::string` 就能读 —— 真去读它是**编不过**，报错信息里就写着这句话。
- **`long double` 掉到 `double` 精度**（两个方向都是）。
- **`char` 是有符号还是无符号随平台**。本层按平台自己的答案分派（有符号平台走有符号那支），
  所以"把一个 ≥ 0x80 的字节放进 `char` 再写出"在两种平台上产出**不同**的数字。
  要字节语义请用 `unsigned char`。
- **失败不是原子的**（§7.3）。
- **`where` 不告诉你是第几个元素**：`vector<T>` 里第 3 个元素出错，只报字段名，不报下标。
- **未知成员扫描是 `O(成员 × 字段)`**（§6.3），默认关掉。
- **没有"一步到串"的便捷函数**（`to_json_string(obj)` 之类）。要串就在外面拿 writer 包
  一层（§3 那段就是），或者用外部缓冲自己持有那份 `std::string`。
- **不做 JSON Pointer / 路径定位**，也不做"只更新 DOM 里某一个成员"那种补丁形状。
- **空 `vector` 字段与"没给这个字段"是两件事**：`[]` 会把目标清空，缺字段不动它（§6.1）。
