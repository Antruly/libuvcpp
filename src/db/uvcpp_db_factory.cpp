/**
 * @file src/db/uvcpp_db_factory.cpp
 * @brief 驱动工厂：按**编译期**开关登记后端，并把连接串里的 scheme 认出来。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 后端在不在这个库里是发行方式决定的（`UVCPP_DB_*_ENABLE`），不是跑起来才发现
 * 的 —— 所以没编进来的 scheme 在这里就返回 nullptr，由 `uvcpp_db_client::open()`
 * 翻成 `NO_DRIVER` 而不是 `BAD_URL`：前者是「这个包没带」，后者是「你写错了」。
 */
#include "db/uvcpp_db_driver.h"

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE

#include <cstdlib>

namespace uvcpp {
namespace db_detail {

namespace {

/// 从 "<scheme>://..." 里切出 scheme，并规范化别名。
/// 认得的别名：postgres / postgresql / pgsql → postgres；mariadb → mysql。
std::string normalize_scheme(const std::string& raw) {
  if (raw == "pg" || raw == "pgsql" || raw == "postgres" ||
      raw == "postgresql") {
    return "postgres";
  }
  if (raw == "mariadb") return "mysql";
  return raw;
}

/// 去掉一层 scheme 前缀，返回剩下的部分（scheme 不认识时返回 false）。
bool strip_scheme(const std::string& text, std::string* scheme,
                  std::string* rest) {
  const size_t pos = text.find(':');
  if (pos == std::string::npos || pos == 0) return false;
  const std::string head = text.substr(0, pos);
  for (size_t i = 0; i < head.size(); ++i) {
    const char c = head[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    if (!ok) return false;
  }
  *scheme = normalize_scheme(head);
  *rest = text.substr(pos + 1);
  return true;
}

std::string percent_decode(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const char hi = text[i + 1];
      const char lo = text[i + 2];
      const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int h = hex(hi), l = hex(lo);
      if (h >= 0 && l >= 0) {
        out.push_back(static_cast<char>(h * 16 + l));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

void split_options(const std::string& text, std::string* body,
                   std::string* options) {
  const size_t q = text.find('?');
  if (q == std::string::npos) {
    *body = text;
    options->clear();
    return;
  }
  *body = text.substr(0, q);
  *options = text.substr(q + 1);
}

/// 解析 `user:password@host:port/database` 这一段。
uvcpp_db_status parse_authority(const std::string& body, uvcpp_db_url* out,
                                std::string* err) {
  std::string rest = body;

  // userinfo：按**最后一个** '@' 切（密码里可以有 '@'）。
  const size_t at = rest.rfind('@');
  if (at != std::string::npos) {
    const std::string userinfo = rest.substr(0, at);
    rest = rest.substr(at + 1);
    const size_t colon = userinfo.find(':');
    if (colon == std::string::npos) {
      out->user = percent_decode(userinfo);
    } else {
      out->user = percent_decode(userinfo.substr(0, colon));
      out->password = percent_decode(userinfo.substr(colon + 1));
    }
  }

  const size_t slash = rest.find('/');
  std::string hostport = rest;
  if (slash != std::string::npos) {
    hostport = rest.substr(0, slash);
    out->database = percent_decode(rest.substr(slash + 1));
  }

  // IPv6 字面量：[::1]:5432
  if (!hostport.empty() && hostport[0] == '[') {
    const size_t close = hostport.find(']');
    if (close == std::string::npos) {
      *err = "连接串里的 IPv6 地址缺少 ']'：" + hostport;
      return uvcpp_db_status::BAD_URL;
    }
    out->host = hostport.substr(1, close - 1);
    if (close + 1 < hostport.size()) {
      if (hostport[close + 1] != ':') {
        *err = "连接串里 ']' 之后只允许 ':端口'：" + hostport;
        return uvcpp_db_status::BAD_URL;
      }
      hostport = hostport.substr(close + 2);
    } else {
      hostport.clear();
    }
  }

  const size_t colon = hostport.rfind(':');
  if (colon != std::string::npos) {
    const std::string port_text = hostport.substr(colon + 1);
    if (!port_text.empty()) {
      char* end = nullptr;
      const long port = std::strtol(port_text.c_str(), &end, 10);
      if (!end || *end != '\0' || port <= 0 || port > 65535) {
        *err = "连接串里的端口不是 1..65535 的整数：" + port_text;
        return uvcpp_db_status::BAD_URL;
      }
      out->port = static_cast<unsigned int>(port);
    }
    hostport = hostport.substr(0, colon);
  }
  if (out->host.empty()) out->host = hostport;
  return uvcpp_db_status::OK;
}

}  // namespace

uvcpp_db_status uvcpp_db_url::parse(const std::string& text, uvcpp_db_url* out,
                                    std::string* err) {
  if (!out) {
    if (err) *err = "uvcpp_db_url::parse 收到了空的 out";
    return uvcpp_db_status::MISUSE;
  }
  *out = uvcpp_db_url();

  std::string scheme;
  std::string rest;
  if (!strip_scheme(text, &scheme, &rest)) {
    if (err) {
      *err = "连接串必须带 scheme（sqlite: / mysql:// / postgres://）：" + text;
    }
    return uvcpp_db_status::BAD_URL;
  }
  out->scheme = scheme;

  // "//" 可有可无：`sqlite:foo.db` 与 `sqlite://foo.db` 等价。
  if (rest.compare(0, 2, "//") == 0) rest = rest.substr(2);

  std::string body;
  split_options(rest, &body, &out->options);

  if (out->scheme == "sqlite") {
    // sqlite 没有主机/口令：冒号之后全是路径。`:memory:` 是内存库。
    if (body.empty() || body == ":memory:") {
      out->database = ":memory:";
    } else {
      out->database = body;
    }
    return uvcpp_db_status::OK;
  }

  return parse_authority(body, out, err);
}

std::unique_ptr<uvcpp_db_driver> uvcpp_db_make_driver(
    const std::string& scheme) {
  if (scheme == "sqlite") {
#if UVCPP_DB_SQLITE_ENABLE
    return uvcpp_db_make_sqlite();
#else
    return nullptr;
#endif
  }
  if (scheme == "mysql") {
#if UVCPP_DB_MYSQL_ENABLE
    return uvcpp_db_make_mysql();
#else
    return nullptr;
#endif
  }
  if (scheme == "postgres") {
#if UVCPP_DB_PGSQL_ENABLE
    return uvcpp_db_make_pgsql();
#else
    return nullptr;
#endif
  }
  return nullptr;
}

bool uvcpp_db_driver_available(const std::string& scheme) {
  if (scheme == "sqlite") {
#if UVCPP_DB_SQLITE_ENABLE
    return true;
#else
    return false;
#endif
  }
  if (scheme == "mysql") {
#if UVCPP_DB_MYSQL_ENABLE
    return true;
#else
    return false;
#endif
  }
  if (scheme == "postgres") {
#if UVCPP_DB_PGSQL_ENABLE
    return true;
#else
    return false;
#endif
  }
  return false;
}

bool uvcpp_db_driver_known(const std::string& scheme) {
  return scheme == "sqlite" || scheme == "mysql" || scheme == "postgres";
}

std::string uvcpp_db_available_drivers() {
  std::string out;
  const char* kAll[] = {"sqlite", "mysql", "postgres"};
  for (size_t i = 0; i < 3; ++i) {
    if (!uvcpp_db_driver_available(kAll[i])) continue;
    if (!out.empty()) out.push_back(',');
    out.append(kAll[i]);
  }
  return out;
}

}  // namespace db_detail
}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
