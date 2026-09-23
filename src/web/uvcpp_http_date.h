/**
 * @file src/web/uvcpp_http_date.h
 * @brief HTTP 日期（IMF-fixdate）格式化，以及 `Date` 响应头的按秒缓存。
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_WEB_UVCPP_HTTP_DATE_H
#define SRC_WEB_UVCPP_HTTP_DATE_H

#include <ctime>
#include <cstddef>
#include <string>

// 显式包含：`.cpp` 里那道 `#if UVCPP_WEB_ENABLE` 靠它。**不能**指望它从别处
// 传递进来 —— 传递断了的话整个 `.cpp` 会静默编成空文件（那是本仓最忌讳的
// "没测表现为通过"的编译期版本），而这里多一行就能杜绝。
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_export.h>
#include <web/uvcpp_http_response.h>

namespace uvcpp {

/**
 * @brief 把 `t` 格式化成 IMF-fixdate，形如 `Sun, 06 Nov 1994 08:49:37 GMT`。
 *
 * 始终按 **GMT** 解释 —— RFC 9110 §5.6.7 只认这一种。用本地时间格式化是这块
 * 最经典的历史缺陷：跨时区的两台机器会给出不同的缓存年龄。
 *
 * 刻意**不**经 `strftime("%a")`/`strftime("%b")`：那两个说明符走 **locale**，
 * 而任何 `setlocale(LC_TIME, "")` 之后的进程里 `%a` 会吐"周日"、`%b` 会吐
 * "11月" —— 那不是格式差异，是**协议错**（对端解析不出来，缓存静默失效）。
 * 星期名与月份名因此是写死的 ASCII 表。
 *
 * `t` 转换失败（`gmtime` 越界）时返回空串。
 *
 * @note 需要"当前时刻"时用 `http_date_now()`：它按秒缓存，不要自己拼
 *       `http_date(::time(NULL))`，那等于每个响应格式化一次。
 */
UVCPP_API std::string http_date(time_t t);

/**
 * @brief 当前时刻的 IMF-fixdate，**按秒缓存**。
 *
 * 与 `http_date(::time(NULL))` 的唯一区别：秒没变就复用上一次的结果。一条
 * keep-alive 连接一秒能过几万个响应，而 `Date` 一秒之内**只有一个值** ——
 * 每个响应都重新 `gmtime` + 格式化是纯白付。
 *
 * 缓存是**线程本地**的（每个循环线程一份），所以 n>1 条循环下不共享一把锁；
 * 槽位只放平凡可析构的 POD（`char[30]` + `time_t`），因此不会注册 TLS
 * 析构回调 —— MinGW 下 DLL 的 TLS 回调里 `delete` 用的不是当初分配的堆
 * （hical 的 `ReadBufferPool` 为这个加了 `#ifndef __MINGW32__`）。
 *
 * 返回值按值返回：调用方（`set_header`）本来就要往头表里拷一份。
 */
UVCPP_API std::string http_date_now();

/**
 * @brief 若 `resp` 上没有 `date` 头，就补一条 `Date: <现在>`。
 *
 * RFC 9110 §6.6.1 的原话是：有钟的源服务器对 **2xx/3xx/4xx 必发**、
 * 对 **1xx/5xx 只是可发**（`MAY`）。装在**服务端出报文的收口**上，而不是
 * `uvcpp_http_response::to_string()` 里面：`to_string()` 是纯序列化，"同一份响应
 * 序列化两次得到同一串"是调用方可以依赖的性质，让它依赖挂钟时间会把这条性质
 * 去掉（逐字节比对响应的用法会当场变脆）。
 *
 * 本框架**没经这个收口**的两条报文因此都不带 `Date`，且都恰好是 1xx（按上面那条
 * "只是可发"，不发合规）：`100 Continue` 是 `uvcpp_http_server.cpp` 里一句字面量
 * 写出去的，WebSocket 的 `101 Switching Protocols` 是 `uvcpp_ws_server.cpp` 里
 * `ostringstream` 拼出来直接写的。注意这是"**没经手**"而**不是**"写了排除" ——
 * 谁哪天把 101 改成走 `send_response`，它就会带上 `Date`，而**那也仍然合规**。
 *
 * 处理函数自己设过 `Date` 就不覆盖 —— 与 `Server` 那条同一个约定。
 */
UVCPP_API void http_ensure_date(uvcpp_http_response& resp);

/**
 * @brief 供用例观测：`http_date_now()` 真正做格式化的次数（进程内累计）。
 *
 * 这是"按秒缓存确实生效"的判据。只比较两次调用返回的串**证不出来**缓存有没有
 * 生效 —— 两次格式化的结果本来就相等（同一秒内）。
 */
UVCPP_API size_t http_date_format_count();

/// @brief 供用例观测：把上面的计数归零。
UVCPP_API void http_date_reset_format_count();

}  // namespace uvcpp

#endif  // SRC_WEB_UVCPP_HTTP_DATE_H
