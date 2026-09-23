/**
 * @file tests/functional/http_date_check.h
 * @brief 「这条响应上的 `Date` 合理吗」的共享判据。
 *
 * 三条 h2/流式的用例各自要断言一次同一件事，所以抽出来。判据分两层，**分工**
 * 是刻意的：
 *
 *   1. **格式本身**由 `web_http_date_func.cpp` 用 7 组硬编码向量钉住
 *      （`Sun, 06 Nov 1994 08:49:37 GMT`、`Thu, 01 Jan 1970 00:00:00 GMT` …），
 *      包括跨世纪与闰日；
 *   2. **本头**只管"这条**线上回来的**响应里那个值是不是**刚刚**生成的"。
 *
 * 所以这里**拿生产格式化器造期望值**不是循环论证 —— 如果 `http_date()` 本身
 * 算错了，第 1 层会先红，而本层红不了（两边一起错）。本层要抓的是完全另一类
 * 缺陷：某个出口忘了补 `Date`、补成了常量、补的是**启动时刻**而不是**响应
 * 时刻**、或者补成了本地时间。那些在第 1 层全都是绿的。
 *
 * 为什么不索性在这里独立实现一遍 IMF 解析：那会变成"测试里第二份日期实现"，
 * 两份迟早走散，而走散之后谁也不知道该信哪一份。
 */
#ifndef TESTS_FUNCTIONAL_HTTP_DATE_CHECK_H
#define TESTS_FUNCTIONAL_HTTP_DATE_CHECK_H

#include <ctime>
#include <string>

#include <web/uvcpp_http_date.h>

namespace uvcpp_test {

/// 允许的时钟偏差（秒）。取值比 "±1 秒" 松一档也不必要：本层跑在同一台机器上，
/// 两边读的是同一个 `time(NULL)`，真实偏差只可能来自"响应跨越了秒边界"。
const int kDateSlackSeconds = 1;

/**
 * @brief 从**原始报文**里取一个响应头的值；没有就返回空串。
 *
 * @param key **必须是小写**，并且带不带末尾冒号都行 —— 线上发出来的头名一律
 *            小写（本框架就是这样），所以这里不做大小写折叠。
 *
 * 只在"手上只有一串字节"的用例里用；已经有解析好的结构时走
 * `http_get_header(st.response.headers, ...)`。
 *
 * **必须锚在行首**，这是本函数唯一的讲究：`raw.find("date: ")` 会命中
 * `x-date: `（前缀相同），也会命中任何一个头**值**里恰好出现的同一串。两种都是
 * **假绿** —— 而假绿比漏判坏，因为它看起来是过的。
 *
 * 响应报文的第一个头**一定**跟在状态行的 `\r\n` 之后，所以只需要搜一种前缀。
 */
inline std::string raw_header_value(const std::string& raw,
                                    const std::string& key) {
  std::string k = key;
  if (k.empty()) return std::string();
  if (k[k.size() - 1] != ':') k += ":";
  const std::string needle = "\r\n" + k + " ";
  const size_t p = raw.find(needle);
  if (p == std::string::npos) return std::string();
  const size_t v = p + needle.size();
  const size_t e = raw.find("\r\n", v);
  if (e == std::string::npos) return std::string();
  return raw.substr(v, e - v);
}

/**
 * @brief 检查 `v` 是不是"刚刚"生成的一个 IMF-fixdate。
 *
 * @param v      线上取回来的 `Date` 值。
 * @param why    [out] 不通过时的原因（过了则清空）。**必须带原因** —— 光知道
 *               "这条没过"没法区分"头忘了发"（空串）与"发了个错的格式"。
 * @return true = 通过。
 */
inline bool date_is_fresh_imf(const std::string& v, std::string* why) {
  if (why != nullptr) why->clear();

  if (v.empty()) {
    if (why != nullptr) *why = "Date 头没发（值为空）";
    return false;
  }
  // 长度先判：IMF-fixdate **恒为 29 字节**。长度不对的话下面那个"与期望值
  // 比较"必然也不等，但报出来的原因会是"值不对"，指不到"格式不是这一种"。
  if (v.size() != 29) {
    if (why != nullptr) {
      *why = "Date 长度是 " + std::to_string(v.size()) + "，IMF-fixdate 恒为 29";
    }
    return false;
  }
  // 必须以 GMT 收尾：发本地时间（含带偏移的 `+0800`）是最经典的一种错，而它
  // 恰好也是 29 字节 —— 不给它单独一条就只会表现成"值不对"。
  if (v.compare(25, 4, " GMT") != 0) {
    if (why != nullptr) *why = "Date 不是以 \" GMT\" 收尾：[ " + v + " ]";
    return false;
  }

  const time_t now = ::time(NULL);
  for (int d = -kDateSlackSeconds; d <= kDateSlackSeconds; ++d) {
    if (v == ::uvcpp::http_date(now + d)) return true;
  }
  if (why != nullptr) {
    *why = "Date=[" + v + "] 与 now/now±1 的期望值都不等（期望形如 [" +
           ::uvcpp::http_date(now) + "]）";
  }
  return false;
}

}  // namespace uvcpp_test

#endif  // TESTS_FUNCTIONAL_HTTP_DATE_CHECK_H
