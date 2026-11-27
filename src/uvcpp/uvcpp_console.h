/**
 * @file src/uvcpp/uvcpp_console.h
 * @brief Windows 控制台代码页的显式设置与恢复（输出默认 UTF-8）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 中文 Windows 控制台默认是 936(GBK)，而本库的日志与 `printf` 出来的字节都是
 * **UTF-8**，直接打到控制台就是乱码。本模块给应用一个**显式**入口：
 *
 * ```cpp
 * int main() {
 *   uvcpp::uvcpp_console_utf8_scope utf8;   // 真控制台下才生效，析构恢复
 *   // ...
 * }
 * ```
 *
 * ## 边界（有意为之）
 *
 * - **不会在库加载/构造时自动改**：控制台代码页是进程/控制台级状态，改了会
 *   影响宿主程序、其它库以及挂在同一控制台上的子进程；库没有资格替宿主决定。
 * - **只在 stdout/stderr 真的是控制台时动手**（Windows `GetConsoleMode` 成功）；
 *   重定向到文件/管道、Windows 服务、无控制台时返回 `changed=false`，不报错。
 * - **默认只改输出代码页**，输入代码页要显式 `include_input=true` 才动 ——
 *   改输入会连带影响交互读入与批处理的解析。
 * - POSIX 上这些接口是**空操作**（终端编码由 locale/终端决定，库不碰全局
 *   locale），getter 返回 0。
 * - 主线程尽早调用；`restore()` 还原旧值，重复调用无副作用。
 *
 * ## 替代方案
 *
 * - 应用自己的 manifest 里写 `<activeCodePage>UTF-8</activeCodePage>`
 *   （Windows 10 1903+）：影响的是进程的窄字符 API 默认代码页，不改变已有
 *   控制台的输出 CP，二者可以叠加。
 * - 让用户在 shell 里 `chcp 65001`：控制台级设置，退出后仍然保留。
 *
 * Win7 的旧 conhost + 光栅字体对 65001 有已知渲染问题；那条线请自行评估，
 * 本模块不替使用者决定。
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_CONSOLE_H
#define SRC_UVCPP_UVCPP_CONSOLE_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

/// 一次代码页设置的结果。`restore()` 之后两个 `*_changed` 都会复位。
struct UVCPP_API uvcpp_console_encoding {
  uvcpp_console_encoding();

  bool     output_changed;   ///< 输出代码页被改过
  bool     input_changed;    ///< 输入代码页被改过
  unsigned old_output_cp;    ///< 改之前的输出代码页（0 = 不知道）
  unsigned old_input_cp;     ///< 改之前的输入代码页（0 = 不知道）

  /// 还原旧代码页（只还原真的改过的那几项）。幂等。
  void restore();
};

/**
 * @brief 设置控制台**输出**代码页（`cp=0` = 不改；`cp` 与当前相同也不改）。
 *
 * @param include_input 同时把输入代码页设成同一个值（默认 false）。
 *
 * 没有控制台时返回 `changed=false`。Windows 专用；POSIX 空操作。
 */
UVCPP_API uvcpp_console_encoding uvcpp_console_set_output_codepage(
    unsigned cp, bool include_input = false);

/// 便捷入口：`uvcpp_console_set_output_codepage(65001, include_input)`。
UVCPP_API uvcpp_console_encoding uvcpp_console_enable_utf8(
    bool include_input = false);

/// 当前输出/输入代码页；POSIX 返回 0。
UVCPP_API unsigned uvcpp_console_output_codepage();
UVCPP_API unsigned uvcpp_console_input_codepage();

/**
 * @brief RAII：构造时把输出代码页设成 UTF-8，析构时还原。
 *
 * 没有控制台时什么也不做（`active()` 为假）。不可拷贝。
 */
class UVCPP_API uvcpp_console_utf8_scope {
 public:
  explicit uvcpp_console_utf8_scope(bool include_input = false);
  ~uvcpp_console_utf8_scope();

  uvcpp_console_utf8_scope(const uvcpp_console_utf8_scope&) = delete;
  uvcpp_console_utf8_scope& operator=(const uvcpp_console_utf8_scope&) =
      delete;

  /// 是否真的改过（真控制台且设置成功）。
  bool active() const { return state_.output_changed; }

  /// 提前还原（析构时再调一次是幂等的）。
  void restore() { state_.restore(); }

 private:
  uvcpp_console_encoding state_;
};

}  // namespace uvcpp

#endif  // SRC_UVCPP_UVCPP_CONSOLE_H
