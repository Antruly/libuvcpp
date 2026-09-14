/**
 * @file tests/functional/web_app_router_func.cpp
 * @brief `uvcpp_web_router` 的功能测试。
 *
 * 纯单元测试，不起网络 —— 路由是纯粹的函数式匹配，直接喂 (method, path)
 * 断言结果比跑一遍真连接快得多，也不会被端口/时序问题干扰。
 *
 * 这个文件里有四条断言是**回归护栏**，改动匹配/索引/优先级逻辑时必须
 * 仍然全绿：
 *
 *  1. 优先级 **静态 > 参数 > 通配**，且**与注册顺序无关** —— 所以每条
 *     优先级用例都**正反注册两遍**，两边必须得出同一个赢家；
 *  2. 404 与 405 必须分得开，405 带正确的 `Allow`；
 *  3. `head_as_get` 回退要标记 `head_of_get`，让调用方能丢 body；
 *  4. 非法模式必须**拒绝注册**（静默注册成功但匹配不上是最难查的 bug）。
 *
 * 另外专门测**索引正确性**：候选集是按「段数 + 首段」缩小过的，如果索引
 * 漏了某个维度，表现就是「路由注册了却匹配不到」—— 所以每条匹配用例都
 * 顺带在验证索引。
 */
#include <iostream>
#include <string>
#include <vector>

#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_router.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/// 一个什么都不做的处理器（只关心"命中了哪个"）。
uvcpp_web_handler dummy() {
  return [](uvcpp_web_request&, uvcpp_web_response&, uvcpp_web_next) {};
}

/// 注册一个带名字的 GET 路由，处理器里记下名字。
struct Named {
  std::string last;
};

uvcpp_web_handler named(Named* n, const std::string& id) {
  return [n, id](uvcpp_web_request&, uvcpp_web_response&, uvcpp_web_next) {
    n->last = id;
  };
}

/**
 * @brief 取路径参数的值；不存在时返回 "<null>"。
 *
 * **不要写 `*m.param(x)`** —— `param()` 查不到会返回 nullptr，解引用直接
 * 段错误。那样一来「路由没匹配上」这种最普通的回归会表现为测试崩溃，
 * 拿不到任何可读信息（A/B 时就踩过一次）。
 */
std::string pv(const web_route_match& m, const std::string& name) {
  const std::string* p = m.param(name);
  return p ? *p : std::string("<null>");
}

/// 命中路由 -> 返回它的名字；否则返回 "<结果分类>"。
std::string hit(const uvcpp_web_router& r, Named* n, http_method m,
                const std::string& path) {
  n->last.clear();
  web_route_match mr = r.match(m, path);
  if (mr.result != web_route_result::MATCHED) {
    return std::string("<") + web_route_result_name(mr.result) + ">";
  }
  // 真的调一下处理器，确认拿到的指针是可用的。
  uvcpp_web_request req;
  uvcpp_web_response resp;
  (*mr.handler)(req, resp, uvcpp_web_next());
  return n->last;
}

// =========================================================================
// 1. 字面量匹配
// =========================================================================
void test_literal() {
  Named n;
  uvcpp_web_router r;

  check(r.get("/", named(&n, "root")), "注册根路由");
  check(r.get("/user/list", named(&n, "list")), "注册字面量路由");
  check(r.get("/a/b/c", named(&n, "abc")), "注册三段字面量");

  check(hit(r, &n, http_method::HTTP_GET, "/") == "root", "匹配 /");
  check(hit(r, &n, http_method::HTTP_GET, "/user/list") == "list",
        "匹配 /user/list");
  check(hit(r, &n, http_method::HTTP_GET, "/a/b/c") == "abc", "匹配 /a/b/c");

  // 连续斜杠折叠、结尾斜杠忽略。
  check(hit(r, &n, http_method::HTTP_GET, "/user//list") == "list",
        "连续斜杠折叠");
  check(hit(r, &n, http_method::HTTP_GET, "/user/list/") == "list",
        "结尾斜杠忽略");
  check(hit(r, &n, http_method::HTTP_GET, "//user/list") == "list",
        "前导斜杠折叠");

  // 大小写敏感（路径是大小写敏感的，别顺手做成不敏感）。
  check(hit(r, &n, http_method::HTTP_GET, "/USER/LIST") == "<NOT_FOUND>",
        "路径匹配大小写敏感");

  check(hit(r, &n, http_method::HTTP_GET, "/user") == "<NOT_FOUND>",
        "段数不符 -> 404");
  check(hit(r, &n, http_method::HTTP_GET, "/user/list/x") == "<NOT_FOUND>",
        "多一段 -> 404");
}

// =========================================================================
// 2. 参数匹配
// =========================================================================
void test_params() {
  uvcpp_web_router r;
  r.get("/user/:id", dummy());
  r.get("/user/:id/post/:pid", dummy());

  web_route_match m = r.match(http_method::HTTP_GET, "/user/42");
  check(m.result == web_route_result::MATCHED, "/user/:id 命中");
  check(m.params.size() == 1, "一个参数");
  check(pv(m, "id") == "42", "id = 42");
  check(m.param("nope") == nullptr, "不存在的参数返回 nullptr");

  // 参数值里的百分号编码是**上游**（web_sanitize_path / 请求层）解好的，
  // 路由层拿到什么就是什么，不再二次解码。
  web_route_match m2 = r.match(http_method::HTTP_GET, "/user/a%20b");
  check(m2.result == web_route_result::MATCHED, "含 %20 的参数命中");
  check(pv(m2, "id") == "a%20b", "路由层不做二次解码");

  // 多个参数，保序。
  web_route_match m3 = r.match(http_method::HTTP_GET, "/user/7/post/99");
  check(m3.result == web_route_result::MATCHED, "双参数路由命中");
  check(m3.params.size() == 2, "两个参数");
  check(pv(m3, "id") == "7" && pv(m3, "pid") == "99", "双参数值正确");
  check(m3.params[0].first == "id" && m3.params[1].first == "pid",
        "参数保序");

  // 参数只吃一段。
  check(r.match(http_method::HTTP_GET, "/user/a/b").result ==
            web_route_result::NOT_FOUND,
        "参数只吃一段（/user/a/b 不命中 /user/:id）");
  check(r.match(http_method::HTTP_GET, "/user").result ==
            web_route_result::NOT_FOUND,
        "参数不能为空段");
}

// =========================================================================
// 3. 通配匹配
// =========================================================================
void test_wildcard() {
  // **只注册一条通配**的表，用来单独验证通配的语义；底下再加一条 /*all
  // 会把它兜住，就测不出「通配至少吃一段」了。
  uvcpp_web_router solo;
  solo.get("/files/*fp", dummy());

  web_route_match m = solo.match(http_method::HTTP_GET, "/files/a/b/c.txt");
  check(m.result == web_route_result::MATCHED, "/files/*fp 命中多段");
  check(pv(m, "fp") == "a/b/c.txt", "通配绑定含 '/' 的剩余路径");
  check(m.params.size() == 1, "通配算一个参数");

  web_route_match m1 = solo.match(http_method::HTTP_GET, "/files/a");
  check(pv(m1, "fp") == "a", "通配可以只吃一段");

  // **通配至少吃一段** —— 这是刻意的语义，让 /files 能是一条独立路由。
  check(solo.match(http_method::HTTP_GET, "/files").result ==
            web_route_result::NOT_FOUND,
        "通配至少吃一段，/files 不命中 /files/*fp");

  // 另一张表：通配 + 根通配并存。
  uvcpp_web_router r;
  r.get("/files/*fp", dummy());
  r.get("/*all", dummy());

  // 单独注册 /files 就能两条共存。
  Named n;
  uvcpp_web_router r2;
  r2.get("/files/*fp", named(&n, "wild"));
  r2.get("/files", named(&n, "exact"));
  check(hit(r2, &n, http_method::HTTP_GET, "/files") == "exact",
        "/files 走独立注册的那条");
  check(hit(r2, &n, http_method::HTTP_GET, "/files/x") == "wild",
        "/files/x 走通配那条");

  // 根通配 /*all 能吃到任意一段以上的路径，但不吃 /。
  check(r.match(http_method::HTTP_GET, "/x/y").result ==
            web_route_result::MATCHED,
        "/*all 命中 /x/y");
  check(pv(r.match(http_method::HTTP_GET, "/x"), "all") == "x",
        "/*all 绑定单段");
  check(r.match(http_method::HTTP_GET, "/").result ==
            web_route_result::NOT_FOUND,
        "/*all 不命中 /（通配至少吃一段）");
  // /files 会被 /*all 吃掉 —— /*all 能吃「一段以上」，/files 正好一段。
  check(r.match(http_method::HTTP_GET, "/files").result ==
            web_route_result::MATCHED,
        "/*all 能吃掉单段的 /files");
  check(pv(r.match(http_method::HTTP_GET, "/files"), "all") == "files",
        "/*all 把 /files 绑成 \"files\"");
}

// =========================================================================
// 4. 回归护栏 1：优先级 静态 > 参数 > 通配，且与注册顺序无关
// =========================================================================
void test_priority_order_independent() {
  // 同一组模式，**正反两个注册顺序**，结果必须一致。
  for (int order = 0; order < 2; ++order) {
    Named n;
    uvcpp_web_router r;

    struct { const char* pat; const char* id; } routes[] = {
        {"/u/:id", "param"},
        {"/u/*rest", "wild"},
        {"/u/list", "static"},
    };
    const int count = 3;
    for (int i = 0; i < count; ++i) {
      const int idx = (order == 0) ? i : (count - 1 - i);
      r.get(routes[idx].pat, named(&n, routes[idx].id));
    }

    const std::string tag = std::string("（注册顺序 ") +
                            (order == 0 ? "静态优先" : "通配优先") + "）";

    check(hit(r, &n, http_method::HTTP_GET, "/u/list") == "static",
          "静态胜过参数/通配 " + tag);
    check(hit(r, &n, http_method::HTTP_GET, "/u/7") == "param",
          "参数胜过通配 " + tag);
    check(hit(r, &n, http_method::HTTP_GET, "/u/a/b") == "wild",
          "只有通配能匹配多段时用它 " + tag);
    // "/u/" 只有 1 段，而 /u/:id 和 /u/list 都要 2 段、/u/*rest 至少要 2 段，
    // 所以三条都不匹配 —— 尾斜杠不产生歧义。
    check(hit(r, &n, http_method::HTTP_GET, "/u/") == "<NOT_FOUND>",
          "尾斜杠路径不命中任何一条 " + tag);
  }

  // 更深一层的优先级：前段更具体的应当胜出，即使后段更宽。
  Named n;
  uvcpp_web_router r;
  r.get("/a/:x/c", named(&n, "param_mid"));
  r.get("/a/b/*z", named(&n, "static_mid"));
  check(hit(r, &n, http_method::HTTP_GET, "/a/b/c") == "static_mid",
        "首段相同的差异点决定胜负");
}

// =========================================================================
// 5. 回归护栏 2：404 / 405 / 自动 OPTIONS
// =========================================================================
void test_404_405_options() {
  Named n;
  uvcpp_web_router r;
  r.get("/res", named(&n, "get"));
  r.post("/res", named(&n, "post"));

  // 405：路径在，方法不对。
  web_route_match m = r.match(http_method::HTTP_PUT, "/res");
  check(m.result == web_route_result::METHOD_NOT_ALLOWED,
        "路径存在但方法不对 -> 405");
  check(m.handler == nullptr, "405 没有 handler");
  // Allow 必须含已注册的方法，且 GET 隐含 HEAD，自动 OPTIONS 开启时含 OPTIONS。
  check(m.allow.find("GET") != std::string::npos, "Allow 含 GET");
  check(m.allow.find("POST") != std::string::npos, "Allow 含 POST");
  check(m.allow.find("HEAD") != std::string::npos, "Allow 含 HEAD（GET 隐含）");
  check(m.allow.find("OPTIONS") != std::string::npos, "Allow 含 OPTIONS");
  check(m.allow == "GET, HEAD, POST, OPTIONS",
        "Allow 排序稳定且完整，实际 = " + m.allow);

  // 404：路径压根不存在。
  web_route_match m2 = r.match(http_method::HTTP_GET, "/nope");
  check(m2.result == web_route_result::NOT_FOUND, "路径不存在 -> 404");
  check(m2.allow.empty(), "404 没有 Allow");

  // 自动 OPTIONS。
  web_route_match m3 = r.match(http_method::HTTP_OPTIONS, "/res");
  check(m3.result == web_route_result::AUTO_OPTIONS,
        "未注册 OPTIONS 时自动生成");
  check(m3.handler == nullptr, "自动 OPTIONS 没有 handler");
  check(m3.allow.find("GET") != std::string::npos, "自动 OPTIONS 带 Allow");

  // 注册了真的 OPTIONS 就用真的。
  Named n2;
  uvcpp_web_router r2;
  r2.get("/res", named(&n2, "get"));
  r2.options("/res", named(&n2, "opt"));
  check(hit(r2, &n2, http_method::HTTP_OPTIONS, "/res") == "opt",
        "注册了真 OPTIONS 时优先用它");

  // 关掉自动 OPTIONS：OPTIONS 变成 405。
  // 用一张**只注册了 GET** 的表 —— r2 上有真 OPTIONS 路由，关掉开关它照样
  // 命中那一条，测不出关闭的效果。
  Named n5;
  uvcpp_web_router r5;
  r5.get("/res", named(&n5, "get"));
  r5.set_auto_options(false);
  check(r5.match(http_method::HTTP_OPTIONS, "/res").result ==
            web_route_result::METHOD_NOT_ALLOWED,
        "关掉 auto_options 后 OPTIONS -> 405");
  check(r5.match(http_method::HTTP_GET, "/res").allow.find("OPTIONS") ==
            std::string::npos,
        "关掉 auto_options 后 Allow 不含 OPTIONS");

  // OPTIONS 打到不存在的路径仍然是 404（不是 405）。
  check(r.match(http_method::HTTP_OPTIONS, "/nope").result ==
            web_route_result::NOT_FOUND,
        "不存在的路径 + OPTIONS -> 404");
}

// =========================================================================
// 6. 回归护栏 3：HEAD 回退
// =========================================================================
void test_head_as_get() {
  Named n;
  uvcpp_web_router r;
  r.get("/thing", named(&n, "get"));

  web_route_match m = r.match(http_method::HTTP_HEAD, "/thing");
  check(m.result == web_route_result::MATCHED, "HEAD 回退到 GET");
  check(m.head_of_get, "回退时 head_of_get 为真（调用方要丢 body）");

  // 直接 GET 不该被标记。
  web_route_match m2 = r.match(http_method::HTTP_GET, "/thing");
  check(m2.result == web_route_result::MATCHED, "GET 正常命中");
  check(!m2.head_of_get, "GET 不标记 head_of_get");

  // 注册了真的 HEAD 就用真的，且不标记。
  Named n2;
  uvcpp_web_router r2;
  r2.get("/thing", named(&n2, "get"));
  r2.head("/thing", named(&n2, "head"));
  web_route_match m3 = r2.match(http_method::HTTP_HEAD, "/thing");
  check(m3.result == web_route_result::MATCHED, "有真 HEAD 路由时命中");
  check(!m3.head_of_get, "真 HEAD 路由不标记回退");
  check(hit(r2, &n2, http_method::HTTP_HEAD, "/thing") == "head",
        "真 HEAD 路由优先于回退");

  // 关掉回退：HEAD 变成 405。
  // **注意要用一张只注册了 GET 的表** —— r2 上有真 HEAD 路由，关掉回退
  // 它照样会命中那一条，测不出关闭的效果。
  Named n3;
  uvcpp_web_router r3;
  r3.get("/thing", named(&n3, "get"));
  r3.set_head_as_get(false);
  check(r3.match(http_method::HTTP_HEAD, "/thing").result ==
            web_route_result::METHOD_NOT_ALLOWED,
        "关掉 head_as_get 后 HEAD -> 405");

  // 回退只在 GET 存在时发生；只有 POST 时 HEAD 仍是 405。
  Named n4;
  uvcpp_web_router r4;
  r4.post("/only", named(&n4, "post"));
  check(r4.match(http_method::HTTP_HEAD, "/only").result ==
            web_route_result::METHOD_NOT_ALLOWED,
        "只有 POST 时 HEAD 不回退");
}

// =========================================================================
// 7. 分组
// =========================================================================
void test_groups() {
  Named n;
  uvcpp_web_router root;
  root.get("/health", named(&n, "health"));

  uvcpp_web_router api = root.group("/api");
  api.get("/users", named(&n, "api_users"));
  api.get("/users/:id", named(&n, "api_user"));

  uvcpp_web_router v2 = api.group("/v2");
  v2.get("/ping", named(&n, "v2_ping"));

  // 分组注册的路由，父对象一样匹配得到（共享同一张表）。
  check(hit(root, &n, http_method::HTTP_GET, "/api/users") == "api_users",
        "分组路由在根对象上可匹配");
  check(hit(root, &n, http_method::HTTP_GET, "/api/users/9") == "api_user",
        "分组 + 参数");
  check(hit(root, &n, http_method::HTTP_GET, "/api/v2/ping") == "v2_ping",
        "嵌套分组");
  check(hit(root, &n, http_method::HTTP_GET, "/health") == "health",
        "根路由不受分组影响");

  // 没加前缀的路径不该被分组的路由吃掉。
  check(root.match(http_method::HTTP_GET, "/users").result ==
            web_route_result::NOT_FOUND,
        "分组前缀是必须的");

  check(root.route_count() == 4, "分组注册的路由计入同一张表");
  check(root.prefix().empty(), "根视图无前缀");
  check(api.prefix() == "/api", "分组视图带前缀");

  // 前缀规范化。
  check(root.group("api").prefix() == "/api", "前缀自动补前导斜杠");
  check(root.group("/api/").prefix() == "/api", "前缀去掉尾斜杠");
  check(root.group("/").prefix().empty(), "group(\"/\") 等于不加前缀");

  // 拷贝是共享句柄，不是复制路由表。
  uvcpp_web_router copy = api;
  copy.get("/copied", named(&n, "copied"));
  check(hit(root, &n, http_method::HTTP_GET, "/api/copied") == "copied",
        "拷贝出来的分组共享同一张表");
  check(root.route_count() == 5, "共享表上的新增对根可见");
}

// =========================================================================
// 8. 回归护栏 4：非法模式必须被拒绝
// =========================================================================
void test_invalid_patterns() {
  uvcpp_web_router r;

  struct { const char* pat; const char* why; } bad[] = {
      {"", "空模式"},
      {"/a/:", "参数名为空"},
      {"/a/:x/y/*", "通配名为空"},
      {"/a/*x/y", "通配不在最后一段"},
      {"/a/b*c", "段中间出现 *"},
      {"/a/b:c", "段中间出现 :"},
      {"/:a:b", "参数名里有 :"},
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    check(!r.get(bad[i].pat, dummy()),
          std::string("拒绝非法模式：") + bad[i].why + " ('" + bad[i].pat +
              "')");
  }
  check(r.route_count() == 0, "非法模式一个都没注册进去");

  // 空 handler 也要拒绝 —— 注册上了但一调就崩。
  check(!r.get("/ok", uvcpp_web_handler()), "拒绝空 handler");
  check(r.route_count() == 0, "空 handler 没注册进去");

  // 合法的都要接受。
  struct { const char* pat; } good[] = {
      {"/"}, {"/a"}, {"/:x"}, {"/a/:b"}, {"/a/*c"}, {"/*d"}, {"/a/b/c/d"},
  };
  for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
    check(r.get(good[i].pat, dummy()),
          std::string("接受合法模式：") + good[i].pat);
  }
  check(r.route_count() == 7, "合法模式全部注册成功");
}

// =========================================================================
// 9. any() 与路由计数
// =========================================================================
void test_any_and_counts() {
  Named n;
  uvcpp_web_router r;
  r.any("/hook", named(&n, "any"));
  r.get("/g", named(&n, "g"));
  r.post("/p", named(&n, "p"));

  check(hit(r, &n, http_method::HTTP_GET, "/hook") == "any", "any 命中 GET");
  check(hit(r, &n, http_method::HTTP_POST, "/hook") == "any", "any 命中 POST");
  check(hit(r, &n, http_method::HTTP_DELETE, "/hook") == "any",
        "any 命中 DELETE");
  check(hit(r, &n, http_method::HTTP_PATCH, "/hook") == "any",
        "any 命中 PATCH");

  check(r.route_count() == 3, "总共 3 条路由");
  check(r.route_count(http_method::HTTP_GET) == 2, "GET 视角 2 条（含 any）");
  check(r.route_count(http_method::HTTP_POST) == 2, "POST 视角 2 条（含 any）");
  check(r.route_count(http_method::HTTP_PUT) == 1, "PUT 视角 1 条（只有 any）");

  // any 命中时不该出现 405。
  check(r.match(http_method::HTTP_PUT, "/g").result ==
            web_route_result::METHOD_NOT_ALLOWED,
        "普通路由 PUT 仍是 405");

  r.clear();
  check(r.route_count() == 0, "clear() 清空");
  check(r.match(http_method::HTTP_GET, "/hook").result ==
            web_route_result::NOT_FOUND,
        "clear() 之后匹配不到");
}

// =========================================================================
// 10. 索引正确性：多段数、多首段混在一起
// =========================================================================
void test_index_correctness() {
  // 一次注册很多条，覆盖不同的段数和首段，确认索引没有漏维度。
  Named n;
  uvcpp_web_router r;

  // 全部集中在首段 "a" 以外的地方，逼索引走不同的桶。
  r.get("/a", named(&n, "a"));
  r.get("/a/b", named(&n, "ab"));
  r.get("/a/b/c", named(&n, "abc"));
  r.get("/a/:x", named(&n, "ax"));
  r.get("/a/*rest", named(&n, "arest"));
  r.get("/z/b", named(&n, "zb"));
  r.get("/:p/b", named(&n, "pb"));
  r.get("/*w", named(&n, "w"));

  check(hit(r, &n, http_method::HTTP_GET, "/a") == "a", "1 段字面量");
  check(hit(r, &n, http_method::HTTP_GET, "/a/b") == "ab", "2 段字面量");
  check(hit(r, &n, http_method::HTTP_GET, "/a/b/c") == "abc", "3 段字面量");
  check(hit(r, &n, http_method::HTTP_GET, "/a/q") == "ax",
        "首段 a + 参数胜过首段 a + 通配");
  check(hit(r, &n, http_method::HTTP_GET, "/a/q/r") == "arest",
        "首段 a + 通配");
  check(hit(r, &n, http_method::HTTP_GET, "/z/b") == "zb",
        "不同首段的字面量");
  check(hit(r, &n, http_method::HTTP_GET, "/m/b") == "pb",
        "动态首段 + 字面量次段");
  check(hit(r, &n, http_method::HTTP_GET, "/q/w/e") == "w",
        "根通配兜底");

  // 动态首段桶：/:p/b 与 /*w 同时在，2 段的路径应当参数胜出。
  check(hit(r, &n, http_method::HTTP_GET, "/anything/b") == "pb",
        "参数首段胜过通配首段");
  // 3 段的路径只有 /*w 能匹配。
  check(hit(r, &n, http_method::HTTP_GET, "/x/y/z") == "w",
        "只有通配能匹配时用它");

  // 段数索引不能把「通配能匹配更长路径」这件事漏掉。
  Named n2;
  uvcpp_web_router r2;
  r2.get("/p/*rest", named(&n2, "p"));
  for (size_t depth = 1; depth <= 6; ++depth) {
    std::string path = "/p";
    for (size_t i = 0; i < depth; ++i) path += "/s";
    check(hit(r2, &n2, http_method::HTTP_GET, path) == "p",
          "通配匹配任意深度（" + path + "）");
  }
}

// =========================================================================
// 11. 匹配结果复用
// =========================================================================
void test_result_reuse() {
  uvcpp_web_router r;
  r.get("/x/:id", dummy());

  // 连续调用 match() 不能互相污染（params 是每次新建的）。
  web_route_match a = r.match(http_method::HTTP_GET, "/x/1");
  web_route_match b = r.match(http_method::HTTP_GET, "/x/2");
  check(pv(a, "id") == "1", "第一次匹配的参数不受第二次影响");
  check(pv(b, "id") == "2", "第二次匹配的参数正确");

  // 空表匹配不崩。
  uvcpp_web_router empty;
  check(empty.match(http_method::HTTP_GET, "/").result ==
            web_route_result::NOT_FOUND,
        "空表返回 404 而不是崩溃");
  check(empty.route_count() == 0, "空表计数为 0");

  // 默认策略。
  uvcpp_web_router d;
  check(d.head_as_get(), "head_as_get 默认开");
  check(d.auto_options(), "auto_options 默认开");
  d.set_head_as_get(false);
  d.set_auto_options(false);
  check(!d.head_as_get() && !d.auto_options(), "开关可关闭");
}

}  // namespace

int main() {
  std::cout << std::unitbuf;  // 崩了也不要丢缓冲的输出

  test_literal();
  test_params();
  test_wildcard();
  test_priority_order_independent();
  test_404_405_options();
  test_head_as_get();
  test_groups();
  test_invalid_patterns();
  test_any_and_counts();
  test_index_correctness();
  test_result_reuse();

  if (g_failures == 0) {
    std::cout << "[router] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[router] FAIL (" << g_failures << " checks failed)" << std::endl;
  return 2;
}
