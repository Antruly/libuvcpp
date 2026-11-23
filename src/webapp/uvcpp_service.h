/**
 * @file src/webapp/uvcpp_service.h
 * @brief 服务化部署支持：Windows 服务封装 + 前台运行（issue #73）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 一个 `main()` 里多一行，应用就能装成 Windows 服务，或者在没有 SCM 的
 * Linux/macOS 上以**前台模式**跑起来（信号驱动优雅停机）：
 *
 * ```cpp
 * int main(int argc, char** argv) {
 *   uvcpp::uvcpp_web_app app;
 *   // ... 配路由 ...
 *   return uvcpp::uvcpp_service_run(app, argc, argv);
 * }
 * ```
 *
 * 命令行（第一条非选项参数是子命令，缺省 `run`）：
 *
 * ```text
 * app run [--foreground]     # 前台运行（Windows 上如果是从控制台直接跑，
 *                            # 也走这条；从 SCM 拉起时走服务模式）
 * app install [--name N]    # Windows：注册服务（自动启动 + 失败后自动重启）
 * app uninstall [--name N]  # Windows：停止并注销
 * app start|stop|status     # Windows：SCM 控制
 * ```
 *
 * 选项（`install` 时会写进服务的命令行，运行期也认）：
 *
 * - `--name <名>` / `--display-name <名>` / `--description <文字>`
 * - `--config <路径>`（只记录并交给 `configure` 回调，本模块不解析任何格式）
 * - `--log <路径>` / `--log-max-mb <n>` / `--log-files <n>`
 * - `--workdir <目录>` / `--env K=V`（可重复）/ `--grace-ms <n>`
 * - `--no-single-instance` / `--foreground` / `--help`
 *
 * ## 语义与边界
 *
 * - **优雅停机**：信号 / SCM 的 STOP 会调 `uvcpp_web_app::stop()` —— 不再接新
 *   连接、等在途请求（`shutdown_grace_ms`）→ 关剩下的连接 → 停循环；本模块
 *   随后 `join()`。应用不用自己写信号处理。
 * - **单实例**：默认开。Windows 用命名互斥体，POSIX 用 `$TMPDIR` 下的
 *   `flock` 锁文件（进程退出自动释放，不会留死锁）。第二个实例拿到
 *   `UV_EBUSY` 后**明确报错退出**，不静默降级。
 * - **日志**：`--log` 给了才重定向 `stdout`/`stderr`；`--log-max-mb` 到了就
 *   轮转（`x.log` → `x.log.1` …，最多保留 `--log-files` 份）。前台模式也能用。
 * - **崩溃后拉起**由 SCM 负责（`install` 会配 `SC_ACTION_RESTART`）；本模块
 *   不自己 fork/监督进程。Windows 多进程仍按 #32 的设计另行处理。
 * - **暂停/继续不支持**：应用层没有"暂停"这个概念，服务控制里也不接受
 *   `SERVICE_ACCEPT_PAUSE_CONTINUE` —— 与其装作支持，不如让 SCM 的按钮是灰的。
 * - 只在**主线程**上调一次；`configure` / `on_started` 也都在主线程上跑。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_SERVICE_H
#define SRC_WEBAPP_UVCPP_SERVICE_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WEBAPP_ENABLE

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <uvcpp/uvcpp_define.h>

namespace uvcpp {

class uvcpp_web_app;

/// `uvcpp_service_run()` 的配置。命令行选项会覆盖这里的同名字段。
struct UVCPP_API uvcpp_service_options {
  uvcpp_service_options();

  /// 服务名 / 单实例名。空 = 用可执行文件名。
  std::string name;
  std::string display_name;
  std::string description;
  /// 启动前 `chdir` 到这里（空 = 不改）。
  std::string working_dir;
  /// 配置文件路径：本模块只负责解析并传给 `configure`，不解析内容。
  std::string config_path;
  /// 启动前设置的环境变量。
  std::vector<std::pair<std::string, std::string> > env;
  /// 非空 = 把 stdout/stderr 重定向到这个文件。
  std::string log_file;
  /// 单份日志上限（字节）；0 = 不轮转。
  std::size_t log_max_bytes;
  /// 轮转保留的份数（不含当前文件）；0 = 不轮转。
  int log_max_files;
  /// >0 时套到 `uvcpp_web_app::set_shutdown_grace_ms()`。
  int stop_grace_ms;
  /// 是否坚持单实例（默认 true）。
  bool single_instance;

  /**
   * @brief 开始服务前的配置钩子（可空）。
   *
   * 命令行已经解析完、工作目录 / 环境变量 / 日志都就绪，`app.start()` **还没
   * 调**。参数里的 `config_path` 就是 `--config` 的值。返回非 0 会中止启动，
   * 那个值原样成为 `uvcpp_service_run()` 的返回值。
   */
  std::function<int(uvcpp_web_app&, const std::string& config_path)> configure;

  /**
   * @brief `app.start()` 成功之后的回调（可空）。
   *
   * 适合打印实际端口、给 systemd 发 `READY=1`、写 pid 文件等。
   */
  std::function<void(uvcpp_web_app&)> on_started;
};

/** @brief 用默认选项 + 命令行运行。等价于 `uvcpp_service_run(app, opts, argc, argv)`。 */
int uvcpp_service_run(uvcpp_web_app& app, int argc, char** argv);

/**
 * @brief 解析命令行并执行对应子命令。
 *
 * `run`：前台或 Windows 服务模式，阻塞到停机完成。`install` / `uninstall` /
 * `start` / `stop` / `status`：Windows 上操作 SCM；其他平台打印一条说明并返回
 * `UV_ENOTSUP`（前台运行仍然可用）。命令行错误返回 `UV_EINVAL`。
 */
int uvcpp_service_run(uvcpp_web_app& app, const uvcpp_service_options& opts,
                      int argc, char** argv);

/// 最近一次解析出的 `--config`（没有 = 空串）。在服务启动前就是最终值。
const std::string& uvcpp_service_config_path();

/// 当前进程是否由 SCM 以服务方式拉起（Windows；其他平台恒 false）。
bool uvcpp_service_is_service();

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE
#endif  // SRC_WEBAPP_UVCPP_SERVICE_H
