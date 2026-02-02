#include "uvcpp_process.h"
#include <uvcpp/uvcpp_alloc.h>

namespace uvcpp {
const char* _STR_NULL = "";
const char* _STR_NULL_ARRY[] = {_STR_NULL, nullptr};

::std::vector<::std::string> string_split(const ::std::string& str, char delim) {
  ::std::size_t previous = 0;
  ::std::size_t current = str.find(delim);
  ::std::vector<::std::string> elems;
  while (current != ::std::string::npos) {
    if (current > previous) {
      elems.push_back(str.substr(previous, current - previous));
    }
    previous = current + 1;
    current = str.find(delim, previous);
  }
  if (previous != str.size()) {
    elems.push_back(str.substr(previous));
  }
  return elems;
}

uvcpp_process::uvcpp_process() : uvcpp_handle() {
  uv_process_t* process = uvcpp::uvcpp_alloc<uv_process_t>();
  this->set_handle(process, true);
  this->init();
}

uvcpp_process::uvcpp_process(uvcpp_loop* loop) : uvcpp_handle() {
  uv_process_t* process = uvcpp::uvcpp_alloc<uv_process_t>();
  this->set_handle(process, true);
  this->init(loop);
}
uvcpp_process::~uvcpp_process() {
  UVCPP_VFREE(options)
  if (op_args != nullptr) {
    delete op_args;
    op_args = nullptr;
  }
}
int uvcpp_process::init() {
  // ensure options struct is allocated for later set_options usage
  if (options == nullptr) {
    options = uvcpp::uvcpp_alloc<uv_process_options_t>();
  }
  memset(options, 0, sizeof(uv_process_options_t));
  memset(UVCPP_PROCESS_HANDLE, 0, sizeof(uv_process_t));
  this->set_handle_data();
  return 0;
}

int uvcpp_process::init(uvcpp_loop *lp) {
  loop = lp;
  // ensure options struct is allocated for later set_options usage
  if (options == nullptr) {
    options = uvcpp::uvcpp_alloc<uv_process_options_t>();
  }
  memset(options, 0, sizeof(uv_process_options_t));
  memset(UVCPP_PROCESS_HANDLE, 0, sizeof(uv_process_t));
  this->set_handle_data();
  return 0;
}

void uvcpp_process::set_options(const char *file, const char **args, const char **env,
                          const char *cwd, unsigned int flags) {
  memset(options, 0, sizeof(uv_process_options_t));

  if (args == nullptr) {
    args = _STR_NULL_ARRY;
  }
  options->file = file;
  options->args = (char **)args;
  options->env = (char **)env;
  options->cwd = cwd;
  options->flags = flags;
}

void uvcpp_process::set_options(const std::string args) {
  memset(options, 0, sizeof(uv_process_options_t));
  argslist = string_split(args, ' ');

  if (op_args != nullptr) {
    delete op_args;
    op_args = nullptr;
  }
  op_args = new const char *[argslist.size() + 1];
  op_args[argslist.size()] = nullptr;

  int i = 0;
  for (std::vector<std::string>::iterator item = argslist.begin();
       item != argslist.end(); item++, i++) {
    op_args[i] = item->c_str();
  }

  options->file = argslist.begin()->c_str();
  options->args = (char **)op_args;
  options->env = nullptr;
  options->cwd = nullptr;
  options->flags = 0;
}

int uvcpp_process::kill(int signum) {
  return uv_process_kill(UVCPP_PROCESS_HANDLE, signum);
}

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 18
int uvcpp_process::get_pid() { return uv_process_get_pid(UVCPP_PROCESS_HANDLE); }
int uvcpp_process::process_get_pid(const uvcpp_process *hd) {
  return uv_process_get_pid(OBJ_UVCPP_PROCESS_HANDLE(*hd));
}
#endif
#endif

int uvcpp_process::process_kill(uvcpp_process *hd, int signum) {
  return uv_process_kill(OBJ_UVCPP_PROCESS_HANDLE(*hd), signum);
}

int uvcpp_process::kill(int pid, int signum) { return uv_kill(pid, signum); }

int uvcpp_process::start(std::function<void(uvcpp_process *, int64_t, int)> start_cb) {
  if (loop == nullptr) {
    return -1;
  }
  process_start_cb = start_cb;
  options->exit_cb = callback_start;
  return uv_spawn((uv_loop_t *)loop->get_handle(), UVCPP_PROCESS_HANDLE, options);
}

void uvcpp_process::callback_start(uv_process_t *handle, int64_t exit_status,
                              int term_signal) {
  if (reinterpret_cast<uvcpp_process *>(handle->data)->process_start_cb)
    reinterpret_cast<uvcpp_process *>(handle->data)
        ->process_start_cb(reinterpret_cast<uvcpp_process *>(handle->data),
                           exit_status, term_signal);
}

  } // namespace uvcpp