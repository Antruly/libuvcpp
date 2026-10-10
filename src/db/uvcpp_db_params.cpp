/**
 * @file src/db/uvcpp_db_params.cpp
 * @brief `uvcpp_db_params` 的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "db/uvcpp_db_params.h"

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE

namespace uvcpp {

const uvcpp_db_value& uvcpp_db_params::null_slot() {
  static const uvcpp_db_value kNull;
  return kNull;
}

uvcpp_db_params::uvcpp_db_params(std::initializer_list<uvcpp_db_value> init)
    : values_(init) {}

uvcpp_db_params& uvcpp_db_params::add(uvcpp_db_value value) {
  values_.push_back(std::move(value));
  return *this;
}

uvcpp_db_params& uvcpp_db_params::add_null() {
  values_.emplace_back();
  return *this;
}

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
