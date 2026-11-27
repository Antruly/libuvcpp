/**
 * @file src/orm/uvcpp_orm_status.cpp
 * @brief `uvcpp_orm_status` 的稳定短名。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "orm/uvcpp_orm_error.h"

#include <uvcpp/uvcpp_config.h>

#if UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE

namespace uvcpp {

const char* uvcpp_orm_status_name(uvcpp_orm_status status) {
  switch (status) {
    case uvcpp_orm_status::OK: return "ok";
    case uvcpp_orm_status::NO_MODEL: return "no_model";
    case uvcpp_orm_status::NO_COLUMNS: return "no_columns";
    case uvcpp_orm_status::EMPTY_TABLE_NAME: return "empty_table_name";
    case uvcpp_orm_status::INVALID_TABLE_NAME: return "invalid_table_name";
    case uvcpp_orm_status::EMPTY_COLUMN_NAME: return "empty_column_name";
    case uvcpp_orm_status::INVALID_COLUMN_NAME: return "invalid_column_name";
    case uvcpp_orm_status::DUPLICATE_COLUMN: return "duplicate_column";
    case uvcpp_orm_status::NO_PRIMARY_KEY: return "no_primary_key";
    case uvcpp_orm_status::MULTIPLE_PRIMARY_KEYS: return "multiple_primary_keys";
    case uvcpp_orm_status::INVALID_AUTO_INCREMENT:
      return "invalid_auto_increment";
    case uvcpp_orm_status::UNSUPPORTED_TYPE: return "unsupported_type";
    case uvcpp_orm_status::CONFLICTING_FLAGS: return "conflicting_flags";
    case uvcpp_orm_status::INVALID_DIALECT: return "invalid_dialect";
    case uvcpp_orm_status::UNKNOWN_COLUMN: return "unknown_column";
    case uvcpp_orm_status::EMPTY_COLUMN_LIST: return "empty_column_list";
    case uvcpp_orm_status::INVALID_OPERATOR: return "invalid_operator";
    case uvcpp_orm_status::EMPTY_IN_VALUES: return "empty_in_values";
    case uvcpp_orm_status::INVALID_ORDER_DIRECTION:
      return "invalid_order_direction";
    case uvcpp_orm_status::INVALID_LIMIT: return "invalid_limit";
    case uvcpp_orm_status::INVALID_OFFSET: return "invalid_offset";
    case uvcpp_orm_status::OFFSET_WITHOUT_LIMIT:
      return "offset_without_limit";
    case uvcpp_orm_status::EMPTY_WHERE: return "empty_where";
    case uvcpp_orm_status::EMPTY_SET: return "empty_set";
    case uvcpp_orm_status::INVALID_COUNT_QUERY:
      return "invalid_count_query";
    case uvcpp_orm_status::EMPTY_RAW_SQL: return "empty_raw_sql";
    case uvcpp_orm_status::PARAM_COUNT_MISMATCH:
      return "param_count_mismatch";
  }
  return "unknown";
}

}  // namespace uvcpp

#endif  // UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE
