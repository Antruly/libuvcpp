#pragma once
#ifndef SRC_UVCPP_UVCPP_EXPORT_H
#define SRC_UVCPP_UVCPP_EXPORT_H

/**
 * @file src/uvcpp/uvcpp_export.h
 * @brief Export macro for shared library visibility.
 * @author zhuweiye
 * @version 1.0.0
 */

#if defined(_WIN32) && defined(UVCPP_EXPORTS)
#  define UVCPP_API __declspec(dllexport)
#elif defined(_WIN32) && defined(UVCPP_IMPORTS)
#  define UVCPP_API __declspec(dllimport)
#else
#  define UVCPP_API
#endif

#endif // SRC_UVCPP_UVCPP_EXPORT_H
