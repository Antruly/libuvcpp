/**
 * @file src/db/uvcpp_db_status.cpp
 * @brief `uvcpp_db_status_name()` 的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "db/uvcpp_db_status.h"

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE

namespace uvcpp {

const char* uvcpp_db_status_name(uvcpp_db_status status) {
  switch (status) {
    case uvcpp_db_status::OK: return "ok";
    case uvcpp_db_status::BAD_URL: return "bad_url";
    case uvcpp_db_status::NO_DRIVER: return "no_driver";
    case uvcpp_db_status::OPEN_FAILED: return "open_failed";
    case uvcpp_db_status::NOT_CONNECTED: return "not_connected";
    case uvcpp_db_status::PREPARE_FAILED: return "prepare_failed";
    case uvcpp_db_status::EXEC_FAILED: return "exec_failed";
    case uvcpp_db_status::BIND_FAILED: return "bind_failed";
    case uvcpp_db_status::UNSUPPORTED: return "unsupported";
    case uvcpp_db_status::MISUSE: return "misuse";
    case uvcpp_db_status::OUT_OF_MEMORY: return "out_of_memory";
  }
  return "unknown";
}

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
