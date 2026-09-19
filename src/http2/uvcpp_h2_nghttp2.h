/**
 * @file src/http2/uvcpp_h2_nghttp2.h
 * @brief 私有头：唯一一处把 `<nghttp2/nghttp2.h>` 拉进来的地方。
 * @author zhuweiye
 * @version 1.1.0
 *
 * **这个头不许被任何公开头包含，也不许出现在 `src/http2/` 之外的 `.cpp` 里。**
 * 理由有两条，都是硬的：
 *
 * 1. `nghttp2.h` 在 MSVC 上单独编不过 —— 它需要 `ssize_t`，而 nghttp2 自己那份
 *    定义来自它构建树里生成的 `config.h`，那个定义**不进任何 INTERFACE**。
 *    下面那段 `#if defined(_MSC_VER)` 就是这个缺口的补丁，**只在 MSVC 上打**；
 *    为什么在 POSIX 上打了反而编不过，见那里的注释。散到多个文件里去写，
 *    迟早有人新加一个 .cpp 直接 include 而静默踩上。
 *
 * 2. 公开头里出现它，使用者和测试 TU 就都要 nghttp2 的 include 路径与
 *    `NGHTTP2_STATICLIB` —— 本库为此把 nghttp2 压成静态、链接标了 PRIVATE，
 *    就是为了让 `uvcpp.dll` 的运行时依赖集一条都不多。把包含关系漏进公开头，
 *    前面那些就全白做了。
 */

#ifndef SRC_HTTP2_UVCPP_H2_NGHTTP2_H
#define SRC_HTTP2_UVCPP_H2_NGHTTP2_H

#if UVCPP_NGHTTP2_ENABLE

// nghttp2 是拿 `int` 编的（它自己的 `check_type_size("ssize_t")` 在 MSVC 上失败，
// 生成的 config.h 里写的就是 `#define ssize_t int`），所以消费者必须也用 `int`。
// 写 `SSIZE_T`（LONG_PTR / 64 位）**编得过**，但头文件那边的回调 typedef 会跟着
// 变成 `long long (*)(...)`，于是和 `.lib` 里的函数指针不是同一个类型 —— 静默
// ABI 不符，比编不过危险得多。
//
// **这手只有 MSVC 需要，也只在 MSVC 上打。** 上面那个 `int` 是 MSVC 特有的：
// POSIX 上同一个 `check_type_size` 是**成功**的，库拿真的 `ssize_t` 编，头文件
// 也从 `<sys/types.h>` 拿到真的那个。在 POSIX 上跟着定义就会撞上
// `typedef _ssize_t ssize_t;` —— `nghttp2.h:52` 是**无条件** include
// `<sys/types.h>` 的 —— 宏把它展开成 `typedef _ssize_t int;`，直接编译不过
// （实测报在 `sys/types.h:182` 的 "multiple types in one declaration"）。
// 本机是 MSVC，这条错误在 Windows 上永远看不见。
#if defined(_MSC_VER)
#define ssize_t int
#endif
#include <nghttp2/nghttp2.h>
// include guard 让后续包含变成空操作，所以这里撤掉是安全的。**但撤掉之后自己
// 写回调必须显式写 `int`**：把这个类型检查留给
// `nghttp2_session_callbacks_set_*(...)` 那次赋值去做，类型不对就编不过。
#if defined(_MSC_VER)
#undef ssize_t
#endif

#endif  // UVCPP_NGHTTP2_ENABLE

#endif  // SRC_HTTP2_UVCPP_H2_NGHTTP2_H
