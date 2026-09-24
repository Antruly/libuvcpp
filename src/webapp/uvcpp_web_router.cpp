/**
 * @file src/webapp/uvcpp_web_router.cpp
 * @brief uvcpp_web_router 的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <webapp/uvcpp_web_router.h>

#include <algorithm>
#include <map>

#include <webapp/uvcpp_log.h>

namespace uvcpp {

namespace {

// =========================================================================
// 模式段
// =========================================================================

enum segment_kind { SEG_LITERAL = 0, SEG_PARAM = 1, SEG_WILDCARD = 2 };

struct pattern_segment {
  segment_kind kind;
  /// SEG_LITERAL 时是字面量本身；SEG_PARAM/SEG_WILDCARD 时是参数名。
  std::string value;
};

/// 特异度：字面量 > 参数 > 通配。数值只用于比较大小。
int specificity_of(segment_kind k) {
  if (k == SEG_LITERAL) return 2;
  if (k == SEG_PARAM) return 1;
  return 0;
}

/**
 * @brief 逐段比较两个模式的特异度。
 * @return >0 表示 a 更具体，<0 表示 b 更具体，0 表示等价。
 */
int compare_specificity(const std::vector<pattern_segment>& a,
                        const std::vector<pattern_segment>& b) {
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = 0; i < n; ++i) {
    const int d = specificity_of(a[i].kind) - specificity_of(b[i].kind);
    if (d != 0) return d;
  }
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  return 0;
}

// =========================================================================
// 切段
// =========================================================================

/**
 * @brief 把路径切成段。
 *
 * 连续的 `/` 折叠成一个，结尾的 `/` 忽略 —— 所以 `"/a//b/"` 和 `"/a/b"`
 * 得到同样的段。根路径 `"/"` 得到 0 段。
 */
std::vector<std::string> split_path(const std::string& path) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') ++i;
    if (i >= path.size()) break;
    size_t j = i;
    while (j < path.size() && path[j] != '/') ++j;
    out.push_back(path.substr(i, j - i));
    i = j;
  }
  return out;
}

/**
 * @brief 解析模式串。
 * @return 合法返回 true；否则返回 false 并往 `err` 写原因。
 */
bool parse_pattern(const std::string& pattern,
                   std::vector<pattern_segment>& out, std::string& err) {
  out.clear();
  if (pattern.empty()) {
    err = "empty pattern";
    return false;
  }
  if (pattern[0] != '/') {
    err = "pattern must start with '/'";
    return false;
  }

  const std::vector<std::string> segs = split_path(pattern);
  for (size_t i = 0; i < segs.size(); ++i) {
    const std::string& s = segs[i];
    pattern_segment ps;

    if (s[0] == ':') {
      const std::string name = s.substr(1);
      if (name.empty()) {
        err = "empty parameter name in '" + s + "'";
        return false;
      }
      if (name.find(':') != std::string::npos ||
          name.find('*') != std::string::npos) {
        err = "invalid parameter name '" + name + "'";
        return false;
      }
      ps.kind = SEG_PARAM;
      ps.value = name;

    } else if (s[0] == '*') {
      const std::string name = s.substr(1);
      if (name.empty()) {
        err = "empty wildcard name in '" + s + "'";
        return false;
      }
      if (i + 1 != segs.size()) {
        // 通配吃掉剩余全部段，所以它后面再有任何东西都是不可达的。
        err = "wildcard '" + s + "' must be the last segment";
        return false;
      }
      ps.kind = SEG_WILDCARD;
      ps.value = name;

    } else {
      if (s.find(':') != std::string::npos ||
          s.find('*') != std::string::npos) {
        // ':' / '*' 只允许出现在段首（作为参数/通配的引导符）。段中间出现
        // 说明使用者想写通配但写错了位置，静默当字面量处理会很难查。
        err = "':' and '*' are only allowed at the start of a segment ('" + s +
              "')";
        return false;
      }
      ps.kind = SEG_LITERAL;
      ps.value = s;
    }
    out.push_back(ps);
  }
  return true;
}

/// 模式里是否带通配。
bool has_wildcard(const std::vector<pattern_segment>& segs) {
  return !segs.empty() && segs.back().kind == SEG_WILDCARD;
}

// =========================================================================
// 路径前缀拼接
// =========================================================================

/// 把分组前缀规范化：`"api/"` → `"/api"`，`"/"` → `""`。
std::string normalize_prefix(const std::string& p) {
  if (p.empty()) return std::string();
  std::string s = p;
  if (s[0] != '/') s = "/" + s;
  while (s.size() > 1 && s[s.size() - 1] == '/') s.erase(s.size() - 1);
  if (s == "/") return std::string();
  return s;
}

/// `join_pattern("/api", "users")` → `"/api/users"`。
std::string join_pattern(const std::string& prefix, const std::string& suffix) {
  if (prefix.empty()) {
    if (suffix.empty()) return std::string("/");
    return suffix[0] == '/' ? suffix : ("/" + suffix);
  }
  if (suffix.empty() || suffix == "/") return prefix;
  std::string a = prefix;
  while (a.size() > 1 && a[a.size() - 1] == '/') a.erase(a.size() - 1);
  std::string b = suffix;
  if (b[0] != '/') b = "/" + b;
  return a + b;
}

// =========================================================================
// 方法集合小工具
// =========================================================================

void push_method_unique(std::vector<http_method>& v, http_method m) {
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == m) return;
  }
  v.push_back(m);
}

bool has_method(const std::vector<http_method>& v, http_method m) {
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == m) return true;
  }
  return false;
}

/// 按方法枚举值排序后拼成 `Allow` 头的值。
std::string build_allow(std::vector<http_method>& methods) {
  std::sort(methods.begin(), methods.end(),
            [](http_method a, http_method b) {
              return static_cast<int>(a) < static_cast<int>(b);
            });
  std::string s;
  for (size_t i = 0; i < methods.size(); ++i) {
    if (i) s += ", ";
    const char* name = http_method_str(methods[i]);
    s += (name && *name) ? name : "?";
  }
  return s;
}

// =========================================================================
// 索引桶
// =========================================================================

/**
 * @brief 一组路由的索引桶。
 *
 * 再按首段字面量分一次，是为了让「一万条路由」的查找不必全都看一遍：
 * `/user/:id` 只可能在首段是 `user` 的桶里（或动态首段桶里）。
 */
struct route_bucket {
  std::map<std::string, std::vector<size_t> > by_first;  ///< 首段字面量
  std::vector<size_t> dynamic_first;  ///< 首段是参数或通配

  bool empty() const { return by_first.empty() && dynamic_first.empty(); }

  void add(size_t idx, const std::vector<pattern_segment>& segs) {
    if (segs.empty()) {
      // 0 段的模式（根路径）。挂在空串键上，只有 0 段的请求会查到这里。
      by_first[std::string()].push_back(idx);
      return;
    }
    if (segs[0].kind == SEG_LITERAL) {
      by_first[segs[0].value].push_back(idx);
    } else {
      dynamic_first.push_back(idx);
    }
  }

  /// 遍历首段为 `first` 的候选下标（含动态首段的）。次序与收集时逐条一致。
  ///
  /// **为什么是遍历而不是先收进一个 `std::vector<size_t>`**：那张临时表每次请求
  /// 都要从 0 容量长起来、请求结束又散掉（普查里
  /// `vector<unsigned long>::_M_range_insert` 1.0045 次/请求），而"看一遍候选"
  /// 这件事本身一次分配都不需要。这里把调用方的主循环原样搬进 `fn`，元素与次序
  /// 逐条不变，少掉的是**每请求一对分配/释放**。
  template <typename TFn>
  void for_each(const std::string& first, TFn fn) const {
    for (size_t i = 0; i < dynamic_first.size(); ++i) {
      fn(dynamic_first[i]);
    }
    std::map<std::string, std::vector<size_t> >::const_iterator it =
        by_first.find(first);
    if (it != by_first.end()) {
      const std::vector<size_t>& v = it->second;
      for (size_t i = 0; i < v.size(); ++i) {
        fn(v[i]);
      }
    }
  }
};

}  // namespace

// =========================================================================
// router_table
// =========================================================================

struct uvcpp_web_router::router_table {
  struct route_entry {
    http_method method;
    bool any_method;
    std::string pattern;  ///< 完整模式（含分组前缀），便于日志与排障
    std::vector<pattern_segment> segments;
    uvcpp_web_handler handler;
  };

  std::vector<route_entry> routes;
  /// 无通配：按**精确段数**分桶
  std::map<size_t, route_bucket> exact_index;
  /// 带通配：按**通配符之前的段数**分桶（可匹配 N >= k+1）
  std::map<size_t, route_bucket> wildcard_index;

  void add(const route_entry& e) {
    const size_t idx = routes.size();
    routes.push_back(e);
    if (has_wildcard(e.segments)) {
      wildcard_index[e.segments.size() - 1].add(idx, e.segments);
    } else {
      exact_index[e.segments.size()].add(idx, e.segments);
    }
  }

  /// 遍历所有候选下标（次序：先精确段数那一桶，再 k = 0..n-1 的通配桶）。
  ///
  /// 与 `route_bucket::for_each` 同一条理由：调用方要的是"逐个看一遍"，不是
  /// "先攥着一张表"。`out` 这个出参就此消失，那个 `out.clear()` 也随之消失 ——
  /// 空表 + `insert` 正是每请求那次分配。
  template <typename TFn>
  void for_each_candidate(const std::vector<std::string>& segs, TFn fn) const {
    const size_t n = segs.size();
    const std::string first = n > 0 ? segs[0] : std::string();

    std::map<size_t, route_bucket>::const_iterator it = exact_index.find(n);
    if (it != exact_index.end()) it->second.for_each(first, fn);

    // 带 k 个前缀段的通配模式能匹配所有 N >= k+1 的路径。
    for (size_t k = 0; k + 1 <= n; ++k) {
      std::map<size_t, route_bucket>::const_iterator wit =
          wildcard_index.find(k);
      if (wit != wildcard_index.end()) wit->second.for_each(first, fn);
    }
  }

  size_t count_method(http_method m) const {
    size_t n = 0;
    for (size_t i = 0; i < routes.size(); ++i) {
      if (routes[i].any_method || routes[i].method == m) ++n;
    }
    return n;
  }

  void clear() {
    routes.clear();
    exact_index.clear();
    wildcard_index.clear();
  }
};

// =========================================================================
// 匹配
// =========================================================================

namespace {

/**
 * @brief 模式与路径段是否匹配；匹配则填 `params`。
 */
bool match_pattern(const std::vector<pattern_segment>& pat,
                   const std::vector<std::string>& segs,
                   std::vector<std::pair<std::string, std::string> >& params) {
  params.clear();

  if (pat.empty()) return segs.empty();

  const bool wild = has_wildcard(pat);
  const size_t fixed = wild ? pat.size() - 1 : pat.size();

  if (wild) {
    // 通配至少吃一段 —— 所以 `/files/*fp` 不匹配 `/files`。
    if (segs.size() < fixed + 1) return false;
  } else {
    if (segs.size() != fixed) return false;
  }

  for (size_t i = 0; i < fixed; ++i) {
    if (pat[i].kind == SEG_LITERAL) {
      if (segs[i] != pat[i].value) return false;
    } else {
      params.push_back(std::make_pair(pat[i].value, segs[i]));
    }
  }

  if (wild) {
    // 剩余段用 '/' 重新拼起来 —— 通配绑定的值是可以含 '/' 的，
    // 这正是它区别于参数的地方。
    std::string joined;
    for (size_t i = fixed; i < segs.size(); ++i) {
      if (i > fixed) joined += '/';
      joined += segs[i];
    }
    params.push_back(std::make_pair(pat.back().value, joined));
  }
  return true;
}

}  // namespace

// =========================================================================
// web_route_match
// =========================================================================

web_route_match::web_route_match()
    : result(web_route_result::NOT_FOUND),
      handler(nullptr),
      pattern(nullptr),
      head_of_get(false) {}

const std::string* web_route_match::param(const std::string& name) const {
  for (size_t i = 0; i < params.size(); ++i) {
    if (params[i].first == name) return &params[i].second;
  }
  return nullptr;
}

const char* web_route_result_name(web_route_result r) {
  switch (r) {
    case web_route_result::MATCHED: return "MATCHED";
    case web_route_result::AUTO_OPTIONS: return "AUTO_OPTIONS";
    case web_route_result::METHOD_NOT_ALLOWED: return "METHOD_NOT_ALLOWED";
    case web_route_result::NOT_FOUND: return "NOT_FOUND";
  }
  return "?";
}

// =========================================================================
// uvcpp_web_router
// =========================================================================

uvcpp_web_router::uvcpp_web_router()
    : table_(new router_table()),
      head_as_get_(true),
      auto_options_(true) {}

uvcpp_web_router::uvcpp_web_router(const std::string& prefix)
    : table_(new router_table()),
      prefix_(normalize_prefix(prefix)),
      head_as_get_(true),
      auto_options_(true) {}

uvcpp_web_router::~uvcpp_web_router() {}

uvcpp_web_router::uvcpp_web_router(const uvcpp_web_router& other)
    : table_(other.table_),
      prefix_(other.prefix_),
      head_as_get_(other.head_as_get_),
      auto_options_(other.auto_options_) {}

uvcpp_web_router& uvcpp_web_router::operator=(const uvcpp_web_router& other) {
  if (this != &other) {
    table_ = other.table_;
    prefix_ = other.prefix_;
    head_as_get_ = other.head_as_get_;
    auto_options_ = other.auto_options_;
  }
  return *this;
}

std::string uvcpp_web_router::full_pattern(const std::string& pattern) const {
  return join_pattern(prefix_, pattern);
}

// -------------------------------------------------------------------------
// 注册
// -------------------------------------------------------------------------

bool uvcpp_web_router::add(http_method method, const std::string& pattern,
                           const uvcpp_web_handler& handler) {
  if (!handler) {
    UVCPP_LOG_WARN(log_category::ROUTER)
        << "拒绝注册空 handler：" << pattern;
    return false;
  }
  if (pattern.empty()) {
    // 不能省。`full_pattern("")` 会经 join_pattern 变成 "/"，于是
    // `get("")` 被**静默**注册成根路由 —— 调用方以为注册失败了（或者以为
    // 注册了别的什么），实际却抢走了 "/"。宁可吵。
    UVCPP_LOG_WARN(log_category::ROUTER) << "拒绝注册空模式";
    return false;
  }
  if (!table_) table_.reset(new router_table());

  router_table::route_entry e;
  e.method = method;
  e.any_method = false;
  e.pattern = full_pattern(pattern);
  e.handler = handler;

  std::string err;
  if (!parse_pattern(e.pattern, e.segments, err)) {
    // 静默失败是最糟的：使用者以为路由注册上了，线上却 404。
    UVCPP_LOG_WARN(log_category::ROUTER)
        << "路由模式非法，未注册：" << e.pattern << " —— " << err;
    return false;
  }

  table_->add(e);
  UVCPP_LOG_DEBUG(log_category::ROUTER)
      << "注册 " << http_method_str(method) << " " << e.pattern;
  return true;
}

bool uvcpp_web_router::any(const std::string& pattern,
                           const uvcpp_web_handler& handler) {
  if (!handler) {
    UVCPP_LOG_WARN(log_category::ROUTER)
        << "拒绝注册空 handler：" << pattern;
    return false;
  }
  if (pattern.empty()) {
    UVCPP_LOG_WARN(log_category::ROUTER) << "拒绝注册空模式";
    return false;
  }
  if (!table_) table_.reset(new router_table());

  router_table::route_entry e;
  e.method = http_method::HTTP_GET;  // any 时这个字段不参与匹配
  e.any_method = true;
  e.pattern = full_pattern(pattern);
  e.handler = handler;

  std::string err;
  if (!parse_pattern(e.pattern, e.segments, err)) {
    UVCPP_LOG_WARN(log_category::ROUTER)
        << "路由模式非法，未注册：" << e.pattern << " —— " << err;
    return false;
  }

  table_->add(e);
  UVCPP_LOG_DEBUG(log_category::ROUTER) << "注册 ANY " << e.pattern;
  return true;
}

bool uvcpp_web_router::get(const std::string& p,
                           const uvcpp_web_handler& h) {
  return add(http_method::HTTP_GET, p, h);
}
bool uvcpp_web_router::post(const std::string& p,
                            const uvcpp_web_handler& h) {
  return add(http_method::HTTP_POST, p, h);
}
bool uvcpp_web_router::put(const std::string& p,
                           const uvcpp_web_handler& h) {
  return add(http_method::HTTP_PUT, p, h);
}
bool uvcpp_web_router::del(const std::string& p,
                           const uvcpp_web_handler& h) {
  return add(http_method::HTTP_DELETE, p, h);
}
bool uvcpp_web_router::patch(const std::string& p,
                             const uvcpp_web_handler& h) {
  return add(http_method::HTTP_PATCH, p, h);
}
bool uvcpp_web_router::head(const std::string& p,
                            const uvcpp_web_handler& h) {
  return add(http_method::HTTP_HEAD, p, h);
}
bool uvcpp_web_router::options(const std::string& p,
                               const uvcpp_web_handler& h) {
  return add(http_method::HTTP_OPTIONS, p, h);
}

uvcpp_web_router uvcpp_web_router::group(const std::string& prefix) const {
  uvcpp_web_router g;
  g.table_ = table_;
  // `group("/")` / `group("")` 是空操作：前缀保持原样，而不是变成 "/"。
  // 走 join_pattern 的话空后缀会被当成根路径，得到 "/"，再往下拼就成了
  // "//users" —— 段切分虽然会折叠掉，但 prefix() 读出来是错的。
  const std::string p = normalize_prefix(prefix);
  g.prefix_ = p.empty() ? prefix_ : join_pattern(prefix_, p);
  // 分组是「注册视图」，策略开关跟着父对象走，免得在分组上改了开关
  // 却对根对象的 match() 毫无影响 —— 那是个很难查的坑。
  g.head_as_get_ = head_as_get_;
  g.auto_options_ = auto_options_;
  return g;
}

// -------------------------------------------------------------------------
// 匹配
// -------------------------------------------------------------------------

web_route_match uvcpp_web_router::match(http_method method,
                                        const std::string& path) const {
  web_route_match r;
  if (!table_ || table_->routes.empty()) return r;  // NOT_FOUND

  const std::vector<std::string> segs = split_path(path);

  std::vector<std::pair<std::string, std::string> > params;
  std::vector<std::pair<std::string, std::string> > best_params;

  const router_table::route_entry* best = nullptr;
  bool best_head_of_get = false;

  // HEAD 没有专门注册时回退到 GET。
  const bool head_fallback = (method == http_method::HTTP_HEAD) && head_as_get_;

  // 候选**边走边判**，不再先落进一张临时表。闭包按引用捕获，主循环里的
  // `continue` 就地变 `return` —— 都是"跳过这个候选"，语义逐条相同。
  table_->for_each_candidate(segs, [&](size_t ci) {
    const router_table::route_entry& e = table_->routes[ci];
    if (!match_pattern(e.segments, segs, params)) return;

    bool hit = e.any_method || (e.method == method);
    bool head_of_get = false;
    if (!hit && head_fallback && e.method == http_method::HTTP_GET) {
      hit = true;
      head_of_get = true;
    }
    if (!hit) return;

    // 取最具体的那条。compare_specificity 是全序，所以结果与注册顺序无关。
    bool take = false;
    if (best == nullptr) {
      take = true;
    } else {
      const int cmp = compare_specificity(e.segments, best->segments);
      if (cmp > 0) {
        take = true;
      } else if (cmp == 0) {
        // 特异度打平时，**真的方法匹配**优先于 HEAD 回退。
        //
        // 这一条不能省：注册了 `head("/x")` 和 `get("/x")` 之后，两条模式
        // 完全相同、特异度打平，如果不特判，先注册的那条会一直赢 ——
        // 表现就是「HEAD 路由注册了却不生效」，而且它只在同时注册了同名
        // GET 时才出现，非常难查。
        take = best_head_of_get && !head_of_get;
      }
    }
    if (take) {
      best = &e;
      // 换而不是拷：`match_pattern` 进来第一句就是 `params.clear()`，
      // 所以把旧缓冲留给 `best_params` 去持有、下一轮由 `clear()` 复用，
      // 语义不变。有参数的路径上这一下省掉的是一次 vector 分配 + N 个
      // pair<string,string> 的深拷贝。
      best_params.swap(params);
      best_head_of_get = head_of_get;
    }
  });

  if (best != nullptr) {
    r.result = web_route_result::MATCHED;
    r.handler = &best->handler;
    r.pattern = &best->pattern;
    r.head_of_get = best_head_of_get;
    // 同样换而不是拷（`r.params` 是刚构造出来的空 vector）。
    r.params.swap(best_params);
    // `allow` / `allowed_methods` 在这条路径上**一个读者都没有**：
    // `uvcpp_web_app::dispatch()` 命中后只用 `params` / `head_of_get` /
    // `handler`，而 405 与自动 OPTIONS 是**各自再调一次 `match()`** 拿这两个
    // 字段的（那两次分别落在 `best == nullptr` 的两个分支上，见下）。
    // 所以这里不填 —— 填了就是每请求白白多一次 sort、一次字符串拼接、
    // 两次分配。契约已写进头文件：这两个字段只在 405 / AUTO_OPTIONS 时有意义。
    return r;
  }

  // ---------------------------------------------------------------------
  // 没命中。**只有走到这里才需要** `Allow` —— 405 与自动 OPTIONS 都要把它
  // 发给客户端，命中路径不要。所以收集动作从主循环里挪到了这里。
  //
  // 代价是这条路要把「路径是否匹配」重跑一遍（主循环里那个 `hit` 判据不参与）。
  // 这是刻意的：这条是**错误路径**，每请求一次的冷代码；而命中路径是热代码。
  // 拿冷路径多一次循环，换热路径少一次分配，方向是对的。
  // ---------------------------------------------------------------------
  std::vector<http_method> allowed;
  {
    // 收集用的临时容器，值本身不参与判定（只看路径是否匹配），所以复用一个。
    std::vector<std::pair<std::string, std::string> > scratch;
    table_->for_each_candidate(segs, [&](size_t ci) {
      const router_table::route_entry& e = table_->routes[ci];
      if (!match_pattern(e.segments, segs, scratch)) return;
      if (!e.any_method) {
        push_method_unique(allowed, e.method);
      }
    });
  }

  if (!allowed.empty()) {
    // 405 / OPTIONS 的 Allow 要列全：GET 隐含 HEAD（RFC 7231 §4.3.2），
    // 开了自动 OPTIONS 就再加上 OPTIONS。
    if (has_method(allowed, http_method::HTTP_GET)) {
      push_method_unique(allowed, http_method::HTTP_HEAD);
    }
    if (auto_options_) {
      push_method_unique(allowed, http_method::HTTP_OPTIONS);
    }
    r.allow = build_allow(allowed);
    r.allowed_methods = allowed;

    if (method == http_method::HTTP_OPTIONS && auto_options_) {
      r.result = web_route_result::AUTO_OPTIONS;
    } else {
      r.result = web_route_result::METHOD_NOT_ALLOWED;
    }
    return r;
  }

  r.result = web_route_result::NOT_FOUND;
  return r;
}

// -------------------------------------------------------------------------
// 策略开关
// -------------------------------------------------------------------------

void uvcpp_web_router::set_head_as_get(bool v) { head_as_get_ = v; }
bool uvcpp_web_router::head_as_get() const { return head_as_get_; }

void uvcpp_web_router::set_auto_options(bool v) { auto_options_ = v; }
bool uvcpp_web_router::auto_options() const { return auto_options_; }

// -------------------------------------------------------------------------
// 观测
// -------------------------------------------------------------------------

size_t uvcpp_web_router::route_count() const {
  return table_ ? table_->routes.size() : 0;
}

size_t uvcpp_web_router::route_count(http_method method) const {
  return table_ ? table_->count_method(method) : 0;
}

void uvcpp_web_router::clear() {
  if (table_) table_->clear();
}

}  // namespace uvcpp
