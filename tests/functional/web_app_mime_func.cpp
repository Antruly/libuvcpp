/**
 * @file tests/functional/web_app_mime_func.cpp
 * @brief `uvcpp_web_mime_map` 的功能测试。
 *
 * 这个模块的价值全在"两件事同时成立"上：
 *   1. 使用者**没碰过**的扩展名，行为必须和内置表完全一致；
 *   2. 使用者**点过名**的扩展名，覆盖必须生效。
 * 所以用例分成这两半，而且第 1 半是**对着 `web_mime_type()` 逐条比**的 ——
 * 不是抄一份期望值列表，而是拿内置表当参照物。抄一份列表的话，日后往内置表
 * 里加条目，这里的用例不会响，而两张表已经漂了。
 *
 * 另外专门盯住 `.ts`：内置表里它是 `video/mp2t`（MPEG 传输流）。这是"改类型"
 * 这个需求最真实的例子，也是最容易被误当成 bug 的一条 —— 它的正确行为是
 * **默认不改**，使用者显式覆盖才变。
 */
#include <cstring>
#include <iostream>
#include <string>

#include <webapp/uvcpp_web_mime.h>
#include <webapp/uvcpp_web_util.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq(const char* got, const char* want, const char* what) {
  if (std::strcmp(got, want) != 0) {
    std::cerr << "  [FAIL] " << what << "\n         期望: [" << want
              << "]\n         实际: [" << got << "]" << std::endl;
    ++g_failures;
  }
}

void check_true(bool cond, const char* what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

// =========================================================================
// 1. 不碰也得对：默认表 == 内置表
// =========================================================================

void test_default_matches_builtin() {
  std::cout << "[mime] default_matches_builtin" << std::endl;

  uvcpp_web_mime_map mime;  // 默认带内置表

  check_true(mime.size() == uvcpp_web_mime_map::builtin_size(),
             "默认表的条目数应等于内置表条目数");
  check_true(uvcpp_web_mime_map::builtin_size() > 0, "内置表不该是空的");

  // 逐条比：拿内置表本体当参照物，而不是在这里抄一份期望值。
  size_t n = 0;
  const web_mime_builtin_entry* builtin = web_mime_builtin_table(n);
  int mismatches = 0;
  for (size_t i = 0; i < n; ++i) {
    const std::string probe = std::string("f.") + builtin[i].ext;
    if (std::strcmp(mime.lookup(probe), builtin[i].type) != 0) {
      if (mismatches < 3) {
        std::cerr << "  [FAIL] 内置扩展名 " << builtin[i].ext
                  << " 在默认表里查成了 " << mime.lookup(probe) << "（期望 "
                  << builtin[i].type << "）" << std::endl;
      }
      ++mismatches;
    }
    // 顺带确认 `lookup()` 与 `lookup_ext()` 不会给出不同答案 —— 它们是
    // 两个入口，实现里路径要经过一次 `?`/`/` 截断，很容易在这里分叉。
    if (std::strcmp(mime.lookup_ext(builtin[i].ext), builtin[i].type) != 0) {
      ++mismatches;
    }
  }
  check_true(mismatches == 0, "默认表必须与内置表逐条一致");

  // 与 `web_mime_type()` 也要一致：同一个文件在两个入口下类型不同，
  // 是最难排查的一种不一致。
  check_eq(mime.lookup("/x/app.js"), web_mime_type("/x/app.js"),
           "lookup 与 web_mime_type 对 .js 应一致");
  check_eq(mime.lookup("/x/a.png"), web_mime_type("/x/a.png"),
           "lookup 与 web_mime_type 对 .png 应一致");
}

// =========================================================================
// 2. 未知扩展名的兜底
// =========================================================================

void test_fallback() {
  std::cout << "[mime] fallback" << std::endl;

  uvcpp_web_mime_map mime;

  check_eq(mime.lookup("/a/definitely-not-a-real-ext.zzqq"),
           "application/octet-stream", "未知扩展名应兜底");
  check_eq(mime.lookup("/a/noext"), "application/octet-stream",
           "没有扩展名应兜底");
  check_eq(mime.lookup("/a/trailing."), "application/octet-stream",
           "以点结尾应兜底");
  check_eq(mime.lookup("/a/dir.d/file"), "application/octet-stream",
           "点出现在目录名里时，不应把它当扩展名");

  // 空表：没有内置条目，但兜底仍在。
  uvcpp_web_mime_map empty(false);
  check_true(empty.size() == 0, "with_builtin=false 应得到空表");
  check_eq(empty.lookup("/a/x.png"), "application/octet-stream",
           "空表对已知扩展名也应兜底");
}

// =========================================================================
// 3. 覆盖
// =========================================================================

void test_override() {
  std::cout << "[mime] override" << std::endl;

  uvcpp_web_mime_map mime;

  // `.ts` 是"改类型"最真实的例子：内置表给的是 video/mp2t。
  check_eq(mime.lookup("/src/main.ts"), "video/mp2t",
           "默认不该动 .ts（内置表就是 video/mp2t）");

  check_true(mime.set("ts", "text/typescript"), "set(\"ts\") 应成功");
  check_eq(mime.lookup("/src/main.ts"), "text/typescript",
           "覆盖之后 .ts 应变成 text/typescript");
  check_eq(mime.lookup("/SRC/MAIN.TS"), "text/typescript",
           "扩展名匹配应大小写不敏感");

  // 带点不带点等价 —— 这是最容易写错的一处。
  check_true(mime.set(".tsx", "text/typescript"), "set(\".tsx\") 应成功");
  check_eq(mime.lookup("/a/b.tsx"), "text/typescript", "带点的写法应生效");
  check_true(mime.set("TSX", "text/x-tsx"), "大写扩展名应被接受");
  check_eq(mime.lookup("/a/b.tsx"), "text/x-tsx",
           "大写扩展名应归一化到同一条目");

  // 覆盖只影响被点名的那条。
  check_eq(mime.lookup("/a/b.js"), "application/javascript",
           "覆盖 .ts 不该动到 .js");

  // 大小写不敏感的查值
  check_eq(mime.lookup_ext("TS"), "text/typescript",
           "lookup_ext 也应大小写不敏感");

  // 路径里取的是**最后一个**点
  check_eq(mime.lookup("/a/archive.tar.gz"), "application/gzip",
           "多段扩展名应看最后一段");
  check_eq(mime.lookup("/a/lib..min.js"), "application/javascript",
           "名字里有连续的点不该影响判定");
}

// =========================================================================
// 4. 批量设置
// =========================================================================

void test_set_from_string() {
  std::cout << "[mime] set_from_string" << std::endl;

  uvcpp_web_mime_map mime;

  const size_t applied =
      mime.set_from_string("yaml=text/yaml; toml=text/toml, mdx=text/mdx");
  check_true(applied == 3, "三条都应生效");

  check_eq(mime.lookup("/a/c.yaml"), "text/yaml", "yaml 应生效");
  check_eq(mime.lookup("/a/c.toml"), "text/toml", "toml 应生效");
  check_eq(mime.lookup("/a/c.mdx"), "text/mdx", "逗号分隔也应生效");

  // 半截配置（没有 `=`）应被跳过，而不是猜一个类型出来。
  const size_t applied2 = mime.set_from_string("png;heic=image/heic");
  check_true(applied2 == 1, "没有 = 的项应被跳过，只生效 1 条");
  check_eq(mime.lookup("/a/p.png"), "image/png",
           "被跳过的项不该改掉 .png");

  // 空串与空白
  check_true(mime.set_from_string("") == 0, "空串应生效 0 条");
  check_true(mime.set_from_string("  ;  , ;; ") == 0, "全空白应生效 0 条");
}

// =========================================================================
// 5. 删除 / 清空 / 复位
// =========================================================================

void test_remove_and_reset() {
  std::cout << "[mime] remove_and_reset" << std::endl;

  uvcpp_web_mime_map mime;

  check_true(mime.remove("png"), "删除已存在的扩展名应返回 true");
  check_eq(mime.lookup("/a/x.png"), "application/octet-stream",
           "删掉之后应兜底");
  check_true(!mime.remove("png"), "重复删除应返回 false");
  check_true(!mime.remove("zzz-nope"), "删除不存在的应返回 false");

  // 删除后重新设置
  check_true(mime.set("png", "image/png"), "删掉之后应能重新设置");
  check_eq(mime.lookup("/a/x.png"), "image/png", "重新设置应生效");

  // 传空 mime 等价于删除
  check_true(mime.set("png", ""), "set(ext, \"\") 应按删除处理并返回 true");
  check_eq(mime.lookup("/a/x.png"), "application/octet-stream",
           "set(ext, \"\") 之后应兜底");

  // 复位能同时清掉覆盖、补回被删的内置项
  mime.set("ts", "text/typescript");
  mime.remove("css");
  mime.reset_to_builtin();
  check_eq(mime.lookup("/a/main.ts"), "video/mp2t",
           "复位应把 .ts 送回内置值");
  check_eq(mime.lookup("/a/s.css"), "text/css",
           "复位应把被删的 .css 补回来");
  check_true(mime.size() == uvcpp_web_mime_map::builtin_size(),
             "复位后条目数应回到内置表大小");

  // clear 之后是真空表
  mime.clear();
  check_true(mime.size() == 0, "clear 之后应为空表");
  check_eq(mime.lookup("/a/s.css"), "application/octet-stream",
           "空表也应兜底");
  mime.reset_to_builtin();
  check_true(mime.size() == uvcpp_web_mime_map::builtin_size(),
             "reset_to_builtin 应能恢复");
}

// =========================================================================
// 6. 无效输入
// =========================================================================

void test_invalid_input() {
  std::cout << "[mime] invalid_input" << std::endl;

  uvcpp_web_mime_map mime;
  const size_t before = mime.size();

  // 这些都不该被收下：收下了就是一条永远匹配不到的条目，而且 size() 会
  // 悄悄变大，让人以为配置生效了。
  check_true(!mime.set("", "text/x"), "空扩展名应被拒");
  check_true(!mime.set(".", "text/x"), "只有点应被拒");
  check_true(!mime.set("...", "text/x"), "只有点（多点）应被拒");
  check_true(!mime.set("a/b", "text/x"), "含分隔符应被拒");
  check_true(!mime.set("a b", "text/x"), "含空格应被拒");
  check_true(!mime.set("pn\ng", "text/x"), "含换行应被拒");
  check_true(!mime.set("a?b", "text/x"), "含 ? 应被拒");

  check_true(mime.size() == before, "无效输入不该改变表的大小");
  check_true(!mime.remove(""), "无效扩展名的删除应返回 false");

  // 值两端空白会被裁掉
  check_true(mime.set("qq", "  text/x-qq  "), "带空白的值应被接受");
  check_eq(mime.lookup("/a/f.qq"), "text/x-qq", "值的空白应被裁掉");
}

// =========================================================================
// 7. is_text
// =========================================================================

void test_is_text() {
  std::cout << "[mime] is_text" << std::endl;

  check_true(uvcpp_web_mime_map::is_text("text/html"), "text/html 是文本");
  check_true(uvcpp_web_mime_map::is_text("text/html; charset=utf-8"),
             "带参数的 text/* 仍是文本");
  check_true(uvcpp_web_mime_map::is_text("application/json"),
             "application/json 是文本");
  check_true(uvcpp_web_mime_map::is_text("application/manifest+json"),
             "+json 后缀是文本");
  check_true(uvcpp_web_mime_map::is_text("image/svg+xml"), "svg 是文本");
  check_true(!uvcpp_web_mime_map::is_text("image/png"), "png 不是文本");
  check_true(!uvcpp_web_mime_map::is_text("application/octet-stream"),
             "octet-stream 不是文本");
  check_true(!uvcpp_web_mime_map::is_text("video/mp2t"), "视频不是文本");

  // 必须与 util 的判据完全一致（同一件事实不能有两个答案）
  check_true(uvcpp_web_mime_map::is_text("application/javascript") ==
                 web_mime_is_text("application/javascript"),
             "is_text 应与 web_mime_is_text 一致（javascript）");
  check_true(uvcpp_web_mime_map::is_text("image/png") ==
                 web_mime_is_text("image/png"),
             "is_text 应与 web_mime_is_text 一致（png）");
}

}  // namespace

int main() {
  test_default_matches_builtin();
  test_fallback();
  test_override();
  test_set_from_string();
  test_remove_and_reset();
  test_invalid_input();
  test_is_text();

  if (g_failures == 0) {
    std::cout << "[mime] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[mime] FAIL (" << g_failures << " checks failed)" << std::endl;
  return 2;
}
