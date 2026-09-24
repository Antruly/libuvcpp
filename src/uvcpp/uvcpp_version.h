/**
 * @file src/uvcpp/uvcpp_version.h
 * @brief Library version macros and helpers.
 * @author zhuweiye
 * @version 1.3.1
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_VERSION_H
#define SRC_UVCPP_UVCPP_VERSION_H

// uvcpp 版本信息头
// 生成类似于其它项目的版本宏与字符串

#define UVCPP_VERSION_MAJOR 1
#define UVCPP_VERSION_MINOR 3
#define UVCPP_VERSION_PATCH 23

// 0 = 非发布版；1 = 发布版
#define UVCPP_VERSION_IS_RELEASE 0

// 可选后缀（例如用于构建元信息），默认为空字符串
#define UVCPP_VERSION_SUFFIX ""

// 辅助宏：双展开字符串化
#define UVCPP_STRINGIFY_HELPER(x) #x
#define UVCPP_STRINGIFY(x) UVCPP_STRINGIFY_HELPER(x)

// RELEASE_TAG：非发布版附加 "-dev"
#if UVCPP_VERSION_IS_RELEASE
#define UVCPP_RELEASE_TAG ""
#else
#define UVCPP_RELEASE_TAG "-dev"
#endif

// 最终版本字符串，例如 "1.0.0" 或 "1.0.0-dev"
#if UVCPP_VERSION_IS_RELEASE
#define UVCPP_VERSION_STRING UVCPP_STRINGIFY(UVCPP_VERSION_MAJOR.UVCPP_VERSION_MINOR.UVCPP_VERSION_PATCH) UVCPP_VERSION_SUFFIX
#else
#define UVCPP_VERSION_STRING UVCPP_STRINGIFY(UVCPP_VERSION_MAJOR.UVCPP_VERSION_MINOR.UVCPP_VERSION_PATCH) UVCPP_VERSION_SUFFIX UVCPP_RELEASE_TAG
#endif

// 版本号的十六进制表示，便于比较
#define UVCPP_VERSION_HEX ((UVCPP_VERSION_MAJOR << 16) | \
                           (UVCPP_VERSION_MINOR << 8)  | \
                           (UVCPP_VERSION_PATCH))


// ---------------------------------------------------------------------------
// 服务器标识
// ---------------------------------------------------------------------------

/**
 * `Server` 响应头的默认值。**刻意不含版本号** —— 把版本号报出去等于告诉扫描器
 * 该试哪个 CVE（见 `uvcpp_web_app.h` 里 `set_server_header()` 那条 @warning）。
 * 想发版本号的使用者自己取 `uvcpp::version_string()` 赋进去，那是他的选择。
 */
#define UVCPP_SERVER_TOKEN "uvcpp"

namespace uvcpp {

/**
 * @brief `UVCPP_VERSION_STRING`：当前源码树的版本串，开发版带 `-dev`。
 *
 * 这是**唯一**的版本号来源（`CMakeLists.txt` 在 configure 期读的也是这个头），
 * 包名、`uvcpp.pc`、README 顶上那两行都跟着它走。在它之前这个宏是死代码
 * （全仓无人引用），所以"版本号有唯一来源"这件事一直没有可用的入口。
 */
inline const char* version_string() { return UVCPP_VERSION_STRING; }

/// @brief `UVCPP_SERVER_TOKEN`：`Server` 头的默认值（不含版本号）。
inline const char* server_token() { return UVCPP_SERVER_TOKEN; }

/// @brief 版本号的十六进制形式（`UVCPP_VERSION_HEX`），便于比较。
inline unsigned version_hex() {
  return static_cast<unsigned>(UVCPP_VERSION_HEX);
}

}  // namespace uvcpp

#endif // SRC_UVCPP_UVCPP_VERSION_H
