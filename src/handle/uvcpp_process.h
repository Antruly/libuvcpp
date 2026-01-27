/**
 * @file src/handle/uvcpp_process.h
 * @brief C++ wrapper for libuv process handles (uv_process_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Allows starting and controlling child processes via libuv.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_PROCESS_H
#define SRC_HANDLE_UVCPP_PROCESS_H

#include <vector>
#include <handle/uvcpp_loop.h>

namespace uvcpp {
class UVCPP_API uvcpp_process : public uvcpp_handle {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_process)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_process)
  /** @brief Construct process wrapper bound to \p loop. */
  uvcpp_process(uvcpp_loop* loop);

  /** @brief Initialize process wrapper. */
  int init();
  /** @brief Initialize on a provided loop. */
  int init(uvcpp_loop* loop);
  /**
   * @brief Set options for spawning a process.
   * @param file Executable path.
   * @param args Null-terminated array of arguments.
   * @param env Optional environment array.
   * @param cwd Optional working directory.
   * @param flags Spawn flags.
   */
  void set_options(const char* file, const char** args,
                  const char** env = nullptr, const char* cwd = nullptr,
                  unsigned int flags = 0);
  /** @brief Convenience overload accepting a single string of args. */
  void set_options(const ::std::string args);
  /** @brief Send signal \p signum to the child process. */
  int kill(int signum);

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 18
  /** @brief Return PID of the child process (if available). */
  int get_pid();
  static int process_get_pid(const uvcpp_process* hd);
#endif
#endif

  static int process_kill(uvcpp_process* hd, int signum);
  static int kill(int pid, int signum);

  /** @brief Start the process and call \p start_cb when it exits. */
  int start(::std::function<void(uvcpp_process*, int64_t, int)> start_cb);

  protected:
  ::std::function<void(uvcpp_process*, int64_t, int)> process_start_cb;
 private:
  /** @brief Internal libuv process exit callback. */
  static void callback_start(uv_process_t* handle, int64_t exit_status,
                             int term_signal);

  private:
  uvcpp_loop* loop = nullptr;
  uv_process_options_t* options = nullptr;
  const char** op_args = nullptr;
  ::std::vector<::std::string> argslist;
};
} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_PROCESS_H