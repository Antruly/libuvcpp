/**
 * @file src/uvcpp/uvcpp_version.h
 * @brief Library version macros and helpers.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_VERSION_H
#define SRC_UVCPP_UVCPP_VERSION_H

// uvcpp 版本信息头
// 生成类似于其它项目的版本宏与字符串

#define UVCPP_VERSION_MAJOR 1
#define UVCPP_VERSION_MINOR 0
#define UVCPP_VERSION_PATCH 0

// 0 = 非发布版；1 = 发布版
#define UVCPP_VERSION_IS_RELEASE 1

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


#endif // SRC_UVCPP_UVCPP_VERSION_H
