/**
 * @file bench/bench_router.cpp
 * @brief `uvcpp_web_router::match()` 的微基准。
 * @author zhuweiye
 *
 * 为什么要有它（而不是拿端到端靶场的 RPS 去判路由改动）
 * ---------------------------------------------------
 * 端到端那个靶场**分辨不了库内的改动**：实测 `uvcpp.dll` 只占服务端 CPU 的
 * 13.67%，剩下 73.5% 在内核网络栈；而且本机回环 TCP 被征了重税、且那笔税
 * **随负载伸缩**（同脚本两次读数差 1.8x）。砍掉半个库，读数埋在噪声里。
 *
 * 路由匹配是**纯粹的函数式匹配**（`web_app_router_func.cpp` 顶上那句：
 * 不起网络，直接喂 (method, path)）。所以它的成本可以在一个没有网络、没有
 * 调度抖动、没有生成器的进程里直接量 —— 这正是本机唯一量得准的形状。
 *
 * 判据怎么读
 * ----------
 * 每个用例跑 `REPEATS` 轮，每轮 `ITERS` 次，报**每轮最小值**（不是平均）。
 * 取最小值是因为要量的是"这条代码路径本身多快"，不是"这台机器当时多忙"：
 * 任何一次被抢占只会把那一轮拉长，最小值对它免疫。绝对 ns 只在同一台机器
 * 上、同一轮里可比。
 *
 * 用例覆盖的不只是快路径，**405 / AUTO_OPTIONS / 404 也要一起量** ——
 * 优化命中路径时最容易顺手把这几条的语义改掉，而它们慢一点无所谓、
 * 错了是 bug（`Allow` 头是给客户端看的契约）。
 *
 * 用法: uvcpp_bench_router [每轮次数] [轮数]
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <webapp/uvcpp_web_router.h>

using namespace uvcpp;

namespace {

/// 什么都不做的处理器 —— 本基准只量匹配，不量调用处理器的开销。
uvcpp_web_handler dummy() {
  return [](uvcpp_web_request&, uvcpp_web_response&, uvcpp_web_next) {};
}

/**
 * @brief 一个用例：固定 (method, path)，外加对结果的断言。
 *
 * 断言写在这里而不是另开一个测试，是为了**让量的东西和断言的东西是同一个
 * 调用**：否则基准可能在量一条早就跑偏了的路径，而断言在另一处还绿着。
 */
struct Case {
  const char* name;
  http_method method;
  const char* path;
  web_route_result want;
  const char* want_allow;  ///< 期望的 `allow` 值；nullptr = 不检查
  const char* want_param;  ///< 期望存在的参数名；nullptr = 不检查
  const char* want_param_val;
};

std::vector<Case> cases() {
  std::vector<Case> v;
  // 命中、无参数 —— Hello World 走的就是这条。
  v.push_back(Case{"matched-1seg", http_method::HTTP_GET, "/json",
                   web_route_result::MATCHED, nullptr, nullptr, nullptr});
  // 命中、有参数。
  v.push_back(Case{"matched-param", http_method::HTTP_GET, "/user/12345",
                   web_route_result::MATCHED, nullptr, "id", "12345"});
  // 命中、多段。
  v.push_back(Case{"matched-3seg", http_method::HTTP_GET, "/api/v1/items",
                   web_route_result::MATCHED, nullptr, nullptr, nullptr});
  // 方法不对 —— 这条**必须**带 Allow。
  v.push_back(Case{"405", http_method::HTTP_POST, "/json",
                   web_route_result::METHOD_NOT_ALLOWED, "GET, HEAD, OPTIONS",
                   nullptr, nullptr});
  // 自动 OPTIONS。
  v.push_back(Case{"auto-options", http_method::HTTP_OPTIONS, "/json",
                   web_route_result::AUTO_OPTIONS, "GET, HEAD, OPTIONS",
                   nullptr, nullptr});
  // 路径不存在。
  v.push_back(Case{"404", http_method::HTTP_GET, "/nope",
                   web_route_result::NOT_FOUND, "", nullptr, nullptr});
  return v;
}

uvcpp_web_router build_router() {
  uvcpp_web_router r;
  r.get("/json", dummy());
  r.get("/plaintext", dummy());
  r.get("/user/:id", dummy());
  r.post("/user/:id", dummy());
  r.get("/api/v1/items", dummy());
  r.get("/api/v1/items/:id", dummy());
  r.get("/files/*rest", dummy());
  return r;
}

/// 一次调用，返回一个能把结果喂给 printf 并**防止整段被优化掉**的指纹。
unsigned long long once(const uvcpp_web_router& r, const Case& c,
                        std::string* allow_out, std::string* param_out) {
  web_route_match m = r.match(c.method, std::string(c.path));
  unsigned long long fp = static_cast<unsigned long long>(m.result);
  if (allow_out != nullptr) *allow_out = m.allow;
  if (param_out != nullptr) {
    const std::string* p = m.param("id");
    *param_out = (p != nullptr) ? *p : std::string("<null>");
  }
  if (m.handler != nullptr) fp += 1;
  if (m.pattern != nullptr) fp += m.pattern->size();
  for (size_t i = 0; i < m.params.size(); ++i) {
    fp += m.params[i].first.size() + m.params[i].second.size();
  }
  return fp;
}

}  // namespace

int main(int argc, char** argv) {
  long long iters = (argc > 1) ? atoll(argv[1]) : 500000;
  int repeats = (argc > 2) ? atoi(argv[2]) : 7;
  if (iters <= 0) iters = 500000;
  if (repeats <= 0) repeats = 7;

  uvcpp_web_router r = build_router();
  std::vector<Case> cs = cases();

  int bad = 0;
  printf("路由数 %zu, 每轮 %lld 次, %d 轮取最小\n", r.route_count(), iters,
         repeats);
  printf("%-14s %14s %10s   %s\n", "case", "min ns/call", "checksum", "verdict");

  for (size_t i = 0; i < cs.size(); ++i) {
    const Case& c = cs[i];
    std::string allow, param;

    // 先断言再计时：断言不过的用例，那个时间没有意义（可能在量另一条路径）。
    unsigned long long fp = once(r, c, &allow, &param);
    bool ok = true;
    std::string why;
    {
      web_route_match m = r.match(c.method, std::string(c.path));
      if (m.result != c.want) {
        ok = false;
        why = std::string("result=") + web_route_result_name(m.result);
      } else if (c.want_allow != nullptr && m.allow != c.want_allow) {
        ok = false;
        why = "allow=\"" + m.allow + "\"";
      } else if (c.want_param != nullptr) {
        const std::string* p = m.param(c.want_param);
        if (p == nullptr) {
          ok = false;
          why = std::string("缺参数 ") + c.want_param;
        } else if (*p != c.want_param_val) {
          ok = false;
          why = "参数=" + *p;
        }
      }
    }
    if (!ok) ++bad;

    // 路径串在**计时循环外**建好。这不是为了公平（两边一样），而是为了让
    // 被测的就是 `match()` 本身 —— 混进一次 SSO 拷贝会把要看的差异稀释掉。
    //
    // 时钟用 `steady_clock` 而不是 `clock()`：MSVC 的 `CLOCKS_PER_SEC` 是
    // **1000**，一次 20 万次调用才 100 个刻度，量化误差就有 1% —— 而要看的
    // 差异比这还小。`steady_clock` 在 MSVC 上走 QPC，分辨率足够。
    const std::string p(c.path);
    double best = 1e18;
    for (int rep = 0; rep < repeats; ++rep) {
      unsigned long long sink = 0;
      const std::chrono::steady_clock::time_point t0 =
          std::chrono::steady_clock::now();
      for (long long k = 0; k < iters; ++k) {
        web_route_match m = r.match(c.method, p);
        sink += static_cast<unsigned long long>(m.result);
        if (m.handler != nullptr) sink += 1;
      }
      const std::chrono::steady_clock::time_point t1 =
          std::chrono::steady_clock::now();
      const double ns =
          static_cast<double>(std::chrono::duration_cast<
                                  std::chrono::nanoseconds>(t1 - t0)
                                  .count()) /
          static_cast<double>(iters);
      // `sink` 必须被"用过"，否则整个循环会被优化掉 —— 那量出来的是 0 ns，
      // 而且看着像"极快"而不是"没量"。
      if (sink == 0xFFFFFFFFFFFFFFFFull) {
        printf("impossible\n");
      }
      if (ns < best) best = ns;
    }
    printf("%-14s %14.1f %10llu   %s%s\n", c.name, best, fp,
           ok ? "ok" : "**FAIL** ", ok ? "" : why.c_str());
  }

  if (bad != 0) {
    printf("\n%d 个用例的语义不对 —— 上面的时间不可用（可能在量另一条路径）\n",
           bad);
    return 1;
  }
  return 0;
}
