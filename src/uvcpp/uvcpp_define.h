#pragma once
#ifndef SRC_UVCPP_UVCPP_DEFINE_H
#define SRC_UVCPP_UVCPP_DEFINE_H

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

#include <cstdlib>
#include <uvcpp/uvcpp_uv_define.h>
// Allocation helpers
#include <uvcpp/uvcpp_alloc.h>
// Export macro for shared library visibility
#include <uvcpp/uvcpp_export.h>

// Visual C++ 6.0
#define _MSC_VER_VS6 1200 // _MSC_VER = 1200

// Visual C++ .NET 2002 (7.0)
#define _MSC_VER_VS2002 1300 // _MSC_VER = 1300

// Visual C++ .NET 2003 (7.1)
#define _MSC_VER_VS2003 1310 // _MSC_VER = 1310

// Visual C++ 2005 (8.0)
#define _MSC_VER_VS2005 1400 // _MSC_VER = 1400

// Visual C++ 2008 (9.0)
#define _MSC_VER_VS2008 1500 // _MSC_VER = 1500

// Visual C++ 2010 (10.0)
#define _MSC_VER_VS2010 1600 // _MSC_VER = 1600

// Visual C++ 2012 (11.0)
#define _MSC_VER_VS2012 1700 // _MSC_VER = 1700

// Visual C++ 2013 (12.0)
#define _MSC_VER_VS2013 1800 // _MSC_VER = 1800

// Visual C++ 2015 (14.0)
#define _MSC_VER_VS2015 1900 // _MSC_VER = 1900

// Visual C++ 2017 (15.0)
#define _MSC_VER_VS2017 1910 // _MSC_VER = 1910

// Visual C++ 2019 (16.0)
#define _MSC_VER_VS2019 1920 // _MSC_VER = 1920

// Visual C++ 2022 (17.0)
#define _MSC_VER_VS2022 1939 // _MSC_VER = 1939

#define UVCPP_VFREE(_data)                                                     \
  if ((_data) != nullptr) {                                                    \
    uvcpp::uvcpp_free_bytes((void *)(_data));                                     \
    (_data) = nullptr;                                                         \
  };
#define VFREE_ARRAY(_data, minNum, maxNum)                                     \
  {                                                                            \
    if (_data != nullptr) {                                                    \
      for (int i = minNum; i < maxNum; i++) {                                  \
        UVCPP_VFREE(_data[i])                                                  \
      }                                                                        \
      UVCPP_VFREE(_data)                                                       \
    }                                                                          \
  }

#define UVCPP_VDELETE(_data)                                                   \
  if (_data != nullptr) {                                                      \
    delete (_data);                                                            \
    _data = nullptr;                                                           \
  };
#define UVCPP_VDELETE_ARRAY(_data, minNum, maxNum)                             \
  {                                                                            \
    if (_data != nullptr) {                                                    \
      for (int i = minNum; i < maxNum; i++) {                                  \
        UVCPP_VDELETE(_data[i]);                                               \
      }                                                                        \
      UVCPP_VDELETE(_data);                                                    \
    }                                                                          \
  }

#define UVCPP_DEFINE_FUNC(type)                                                \
  explicit type();                                                             \
  virtual ~type();

#define UVCPP_DEFINE_INHERIT_FUNC(type)                                        \
  explicit type();                                                             \
  explicit type(type *t_p);                                                    \
  virtual ~type();

#define UVCPP_DEFINE_INHERIT(fatype):fatype(nullptr)

#define UVCPP_DEFINE_COPY_FUNC_DELETE(type)                                    \
  type(const type &obj) = delete;                                              \
  type &operator=(const type &obj) = delete;

#define UVCPP_DEFINE_COPY_FUNC(type)                                           \
  type(const type &obj);                                                       \
  type &operator=(const type &obj);

#define UVCPP_ANSI_COLOR_RED "\x1B[31m"
#define UVCPP_ANSI_COLOR_GREEN "\x1B[32m"
#define UVCPP_ANSI_COLOR_YELLOW "\x1B[33m"
#define UVCPP_ANSI_COLOR_BLUE "\x1B[34m"
#define UVCPP_ANSI_COLOR_MAGENTA "\x1B[35m"
#define UVCPP_ANSI_COLOR_CYAN "\x1B[36m"
#define UVCPP_ANSI_COLOR_WHITE "\x1B[37m"
#define UVCPP_ANSI_COLOR_RESET "\x1B[0m"

#define UVCPP_MS_SLEEP(mtime)                                                  \
  ::std::this_thread::sleep_for(::std::chrono::milliseconds(mtime))

#endif // SRC_UVCPP_UVCPP_DEFINE_H