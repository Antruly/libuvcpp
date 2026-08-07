/**
 * @file src/web/uvcpp_http_parser.cpp
 * @brief Implementation of uvcpp_http_parser — llhttp C++ wrapper.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Bridges nodejs/llhttp (C state machine) to the project's C++ callback
 * model using the trampoline pattern: static C callbacks registered with
 * llhttp forward through parser->data (a void* pointing back to the
 * uvcpp_http_parser instance) to the user-supplied C++ std::function
 * stored as fn_ptr + void* pairs.
 */

#include <web/uvcpp_http_parser.h>

#if UVCPP_WEB_ENABLE

#include <llhttp.h>
#include <cstring>
#include <new>

namespace uvcpp {

// =========================================================================
// Opaque type definitions — map to real llhttp types
// =========================================================================
struct llhttp_raw          { llhttp_t impl; };
struct llhttp_settings_raw { llhttp_settings_t impl; };

// Convenience accessors
static inline llhttp_t*          raw_p(llhttp_raw* p)           { return &p->impl; }
static inline llhttp_settings_t* raw_s(llhttp_settings_raw* p)  { return &p->impl; }

// =========================================================================
// Static helpers
// =========================================================================

/**
 * @brief Recover the uvcpp_http_parser* from llhttp_t::data.
 */
static inline uvcpp_http_parser* self_from_llhttp(llhttp_t* p) {
  return static_cast<uvcpp_http_parser*>(p->data);
}

// =========================================================================
// Construction / Destruction
// =========================================================================

uvcpp_http_parser::uvcpp_http_parser(http_parser_mode mode)
    : mode_(mode) {
  // Allocate llhttp structures on the heap (PIMPL)
  raw_parser_   = new llhttp_raw;
  raw_settings_ = new llhttp_settings_raw;

  std::memset(raw_parser_,   0, sizeof(llhttp_raw));
  std::memset(raw_settings_, 0, sizeof(llhttp_settings_raw));

  llhttp_settings_init(raw_s(raw_settings_));
  bind_settings();

  // Select llhttp parser type
  llhttp_type_t ll_type = (mode_ == http_parser_mode::PARSE_REQUEST)
                              ? HTTP_REQUEST
                              : HTTP_RESPONSE;

  llhttp_init(raw_p(raw_parser_), ll_type, raw_s(raw_settings_));
  raw_p(raw_parser_)->data = this;  // back-pointer for trampolines
}

uvcpp_http_parser::~uvcpp_http_parser() {
  delete raw_parser_; raw_parser_ = nullptr;
  delete raw_settings_; raw_settings_ = nullptr;
  // Clean up trampoline callbacks with correct types
  delete static_cast<std::function<void(const char*, size_t)>*>(url_arg_);
  delete static_cast<std::function<void(const char*, size_t)>*>(status_arg_);
  delete static_cast<std::function<void(const char*, size_t)>*>(field_arg_);
  delete static_cast<std::function<void(const char*, size_t)>*>(value_arg_);
  delete static_cast<std::function<void()>*>(headers_done_arg_);
  delete static_cast<std::function<void(const char*, size_t)>*>(body_arg_);
  delete static_cast<std::function<void()>*>(msg_done_arg_);
  delete static_cast<std::function<void(size_t)>*>(chunk_hdr_arg_);
  delete static_cast<std::function<void()>*>(chunk_done_arg_);
  url_arg_ = status_arg_ = field_arg_ = value_arg_ = nullptr;
  headers_done_arg_ = body_arg_ = msg_done_arg_ = chunk_hdr_arg_ = chunk_done_arg_ = nullptr;
}

// =========================================================================
// Reset
// =========================================================================

void uvcpp_http_parser::reset() {
  llhttp_type_t ll_type = (mode_ == http_parser_mode::PARSE_REQUEST)
                              ? HTTP_REQUEST
                              : HTTP_RESPONSE;

  // Re-init the raw parser; preserves settings pointer
  llhttp_init(raw_p(raw_parser_), ll_type, raw_s(raw_settings_));
  raw_p(raw_parser_)->data = this;

  // Clear accumulated state
  url_buf_.clear();
  headers_.clear();
  cur_header_name_.clear();
  cur_header_value_.clear();
  method_        = http_method::HTTP_GET;
  status_code_   = http_status::OK;
  version_       = static_cast<uvcpp_http_version>(1);
  keep_alive_    = true;
  content_length_ = 0;
  upgrade_       = false;
  last_chunk_size_ = 0;
  state_         = http_parser_state::IDLE;
}

// =========================================================================
// Bind settings
// =========================================================================

void uvcpp_http_parser::bind_settings() {
  raw_s(raw_settings_)->on_message_begin    = ll_on_message_begin;
  raw_s(raw_settings_)->on_url              = ll_on_url;
  raw_s(raw_settings_)->on_status           = ll_on_status;
  raw_s(raw_settings_)->on_header_field           = ll_on_header_field;
  raw_s(raw_settings_)->on_header_value           = ll_on_header_value;
  raw_s(raw_settings_)->on_header_field_complete  = ll_on_header_field_complete;
  raw_s(raw_settings_)->on_header_value_complete  = ll_on_header_value_complete;
  raw_s(raw_settings_)->on_headers_complete       = ll_on_headers_complete;
  raw_s(raw_settings_)->on_body             = ll_on_body;
  raw_s(raw_settings_)->on_message_complete = ll_on_message_complete;
  raw_s(raw_settings_)->on_chunk_header     = ll_on_chunk_header;
  raw_s(raw_settings_)->on_chunk_complete   = ll_on_chunk_complete;
  raw_s(raw_settings_)->on_reset            = ll_on_reset;
}

// =========================================================================
// Execute / Finish
// =========================================================================

size_t uvcpp_http_parser::execute(const char* data, size_t len) {
  if (state_ == http_parser_state::PARSE_ERROR) return 0;
  if (state_ == http_parser_state::COMPLETE) return 0;

  // Set running state
  if (state_ == http_parser_state::IDLE) {
    state_ = http_parser_state::HEADER;
  }

  llhttp_errno_t err = llhttp_execute(raw_p(raw_parser_), data, len);

  if (err == HPE_OK) {
    // Check if message is complete (body fully received)
    // llhttp sets the "finish" flag internally
    if (raw_p(raw_parser_)->finish) {
      state_ = http_parser_state::COMPLETE;
    }
    return len;  // All data consumed
  } else if (err == HPE_PAUSED) {
    // Parser paused by a callback returning HPE_PAUSED
    // Return bytes consumed so far
    const char* pos = llhttp_get_error_pos(raw_p(raw_parser_));
    if (pos != nullptr && pos >= data && pos < data + len) {
      return static_cast<size_t>(pos - data);
    }
    return len;
  } else if (err == HPE_PAUSED_UPGRADE) {
    // Upgrade requested (e.g. WebSocket)
    upgrade_ = true;
    state_ = http_parser_state::COMPLETE;
    const char* pos = llhttp_get_error_pos(raw_p(raw_parser_));
    if (pos != nullptr && pos >= data && pos < data + len) {
      return static_cast<size_t>(pos - data);
    }
    return len;
  } else {
    // Parse error
    state_ = http_parser_state::PARSE_ERROR;
    return 0;
  }
}

int uvcpp_http_parser::finish() {
  if (state_ == http_parser_state::PARSE_ERROR) return -1;

  llhttp_errno_t err = llhttp_finish(raw_p(raw_parser_));
  if (err == HPE_OK) {
    state_ = http_parser_state::COMPLETE;
    return 0;
  }
  state_ = http_parser_state::PARSE_ERROR;
  return static_cast<int>(err);
}

// =========================================================================
// State queries
// =========================================================================

bool uvcpp_http_parser::is_complete() const {
  return state_ == http_parser_state::COMPLETE;
}

bool uvcpp_http_parser::has_error() const {
  return state_ == http_parser_state::PARSE_ERROR;
}

http_parser_state uvcpp_http_parser::get_state() const {
  return state_;
}

const char* uvcpp_http_parser::error_reason() const {
  if (state_ != http_parser_state::PARSE_ERROR) return nullptr;
  // llhttp_errno_name expects the raw parser's error code
  return llhttp_errno_name(
      static_cast<llhttp_errno_t>(raw_p(raw_parser_)->error));
}

// =========================================================================
// Parsed results
// =========================================================================

http_method uvcpp_http_parser::get_method() const {
  return method_;
}

http_status uvcpp_http_parser::get_status_code() const {
  return status_code_;
}

uvcpp_http_version uvcpp_http_parser::get_uvcpp_http_version() const {
  return version_;
}

const std::string& uvcpp_http_parser::get_url() const {
  return url_buf_;
}

const http_headers& uvcpp_http_parser::get_headers() const {
  return headers_;
}

bool uvcpp_http_parser::should_keep_alive() const {
  return keep_alive_;
}

uint64_t uvcpp_http_parser::get_content_length() const {
  return content_length_;
}

bool uvcpp_http_parser::is_upgrade() const {
  return upgrade_;
}

// =========================================================================
// Callback setters
// =========================================================================

void uvcpp_http_parser::set_on_url(std::function<void(const char*, size_t)> cb) {
  delete static_cast<std::function<void(const char*, size_t)>*>(url_arg_);
  if (cb) {
    auto* p = new std::function<void(const char*, size_t)>(std::move(cb));
    url_fn_  = [](const char* d, size_t n, void* a) { (*static_cast<std::function<void(const char*, size_t)>*>(a))(d, n); };
    url_arg_ = p;
  } else { url_fn_ = nullptr; url_arg_ = nullptr; }
}

void uvcpp_http_parser::set_on_status(std::function<void(const char*, size_t)> cb) {
  delete static_cast<std::function<void(const char*, size_t)>*>(status_arg_);
  if (cb) {
    auto* p = new std::function<void(const char*, size_t)>(std::move(cb));
    status_fn_  = [](const char* d, size_t n, void* a) { (*static_cast<std::function<void(const char*, size_t)>*>(a))(d, n); };
    status_arg_ = p;
  } else { status_fn_ = nullptr; status_arg_ = nullptr; }
}

void uvcpp_http_parser::set_on_header_field(std::function<void(const char*, size_t)> cb) {
  delete static_cast<std::function<void(const char*, size_t)>*>(field_arg_);
  if (cb) {
    auto* p = new std::function<void(const char*, size_t)>(std::move(cb));
    field_fn_  = [](const char* d, size_t n, void* a) { (*static_cast<std::function<void(const char*, size_t)>*>(a))(d, n); };
    field_arg_ = p;
  } else { field_fn_ = nullptr; field_arg_ = nullptr; }
}

void uvcpp_http_parser::set_on_header_value(std::function<void(const char*, size_t)> cb) {
  delete static_cast<std::function<void(const char*, size_t)>*>(value_arg_);
  if (cb) {
    auto* p = new std::function<void(const char*, size_t)>(std::move(cb));
    value_fn_  = [](const char* d, size_t n, void* a) { (*static_cast<std::function<void(const char*, size_t)>*>(a))(d, n); };
    value_arg_ = p;
  } else { value_fn_ = nullptr; value_arg_ = nullptr; }
}

void uvcpp_http_parser::set_on_headers_complete(std::function<void()> cb) {
  delete static_cast<std::function<void()>*>(headers_done_arg_);
  if (cb) {
    auto* p = new std::function<void()>(std::move(cb));
    headers_done_fn_  = [](void* a) { (*static_cast<std::function<void()>*>(a))(); };
    headers_done_arg_ = p;
  } else { headers_done_fn_ = nullptr; headers_done_arg_ = nullptr; }
}

void uvcpp_http_parser::set_on_body(std::function<void(const char*, size_t)> cb) {
  delete static_cast<std::function<void(const char*, size_t)>*>(body_arg_);
  if (cb) {
    auto* p = new std::function<void(const char*, size_t)>(std::move(cb));
    body_fn_  = [](const char* d, size_t n, void* a) { (*static_cast<std::function<void(const char*, size_t)>*>(a))(d, n); };
    body_arg_ = p;
  } else { body_fn_ = nullptr; body_arg_ = nullptr; }
}

void uvcpp_http_parser::set_on_message_complete(std::function<void()> cb) {
  delete static_cast<std::function<void()>*>(msg_done_arg_);
  if (cb) {
    auto* p = new std::function<void()>(std::move(cb));
    msg_done_fn_  = [](void* a) { (*static_cast<std::function<void()>*>(a))(); };
    msg_done_arg_ = p;
  } else { msg_done_fn_ = nullptr; msg_done_arg_ = nullptr; }
}

void uvcpp_http_parser::set_on_chunk_header(std::function<void(size_t)> cb) {
  delete static_cast<std::function<void(size_t)>*>(chunk_hdr_arg_);
  if (cb) {
    auto* p = new std::function<void(size_t)>(std::move(cb));
    chunk_hdr_fn_  = [](size_t sz, void* a) { (*static_cast<std::function<void(size_t)>*>(a))(sz); };
    chunk_hdr_arg_ = p;
  } else { chunk_hdr_fn_ = nullptr; chunk_hdr_arg_ = nullptr; }
}

void uvcpp_http_parser::set_on_chunk_complete(std::function<void()> cb) {
  delete static_cast<std::function<void()>*>(chunk_done_arg_);
  if (cb) {
    auto* p = new std::function<void()>(std::move(cb));
    chunk_done_fn_  = [](void* a) { (*static_cast<std::function<void()>*>(a))(); };
    chunk_done_arg_ = p;
  } else { chunk_done_fn_ = nullptr; chunk_done_arg_ = nullptr; }
}

// =========================================================================
// Static C trampolines — called by llhttp with parser->data == this
// =========================================================================

int uvcpp_http_parser::ll_on_message_begin(llhttp_t* p) {
  auto* self = self_from_llhttp(p);
  self->state_ = http_parser_state::HEADER;
  return 0;
}

int uvcpp_http_parser::ll_on_url(llhttp_t* p, const char* at, size_t len) {
  auto* self = self_from_llhttp(p);
  self->url_buf_.append(at, len);
  if (self->url_fn_) self->url_fn_(at, len, self->url_arg_);
  return 0;
}

int uvcpp_http_parser::ll_on_status(llhttp_t* p, const char* at, size_t len) {
  auto* self = self_from_llhttp(p);
  if (self->status_fn_) self->status_fn_(at, len, self->status_arg_);
  return 0;
}

int uvcpp_http_parser::ll_on_header_field(llhttp_t* p, const char* at, size_t len) {
  auto* self = self_from_llhttp(p);
  // If there's a pending header value, push it before starting new name
  if (!self->cur_header_name_.empty() && !self->cur_header_value_.empty()) {
    self->headers_.push_back({self->cur_header_name_, self->cur_header_value_});
    self->cur_header_name_.clear();
    self->cur_header_value_.clear();
  }
  self->cur_header_name_.append(at, len);
  if (self->field_fn_) self->field_fn_(at, len, self->field_arg_);
  return 0;
}

int uvcpp_http_parser::ll_on_header_value(llhttp_t* p, const char* at, size_t len) {
  auto* self = self_from_llhttp(p);
  self->cur_header_value_.append(at, len);
  if (self->value_fn_) self->value_fn_(at, len, self->value_arg_);
  return 0;
}

int uvcpp_http_parser::ll_on_header_field_complete(llhttp_t* p) {
  (void)p;
  return 0;
}

int uvcpp_http_parser::ll_on_header_value_complete(llhttp_t* p) {
  auto* self = self_from_llhttp(p);
  // Push accumulated header (also covers multi-chunk values)
  if (!self->cur_header_name_.empty()) {
    self->headers_.push_back({self->cur_header_name_, self->cur_header_value_});
    self->cur_header_name_.clear();
    self->cur_header_value_.clear();
  }
  return 0;
}

int uvcpp_http_parser::ll_on_headers_complete(llhttp_t* p) {
  auto* self = self_from_llhttp(p);

  // Flush any remaining header pair
  if (!self->cur_header_name_.empty()) {
    self->headers_.push_back({self->cur_header_name_, self->cur_header_value_});
    self->cur_header_name_.clear();
    self->cur_header_value_.clear();
  }

  // Extract metadata from raw parser
  self->extract_metadata();

  if (self->headers_done_fn_) self->headers_done_fn_(self->headers_done_arg_);

  // If this is an Upgrade request (e.g. WebSocket), return 2 to
  // make llhttp_execute return HPE_PAUSED_UPGRADE; we'll detect
  // this at the execute() level.
  if (self->upgrade_) return 2;  // HPE_PAUSED_UPGRADE

  // If no body expected (e.g. HEAD request, 204 No Content, etc.),
  // return 1 to skip body parsing.
  if (p->content_length == 0) {
    // llhttp will set content_length to ULLONG_MAX for chunked
    // or unknown length — we still need to parse those
    return 0;
  }

  return 0;
}

int uvcpp_http_parser::ll_on_body(llhttp_t* p, const char* at, size_t len) {
  auto* self = self_from_llhttp(p);
  // Track that we're in body parsing
  self->state_ = http_parser_state::BODY;
  if (self->body_fn_) self->body_fn_(at, len, self->body_arg_);
  return 0;
}

int uvcpp_http_parser::ll_on_message_complete(llhttp_t* p) {
  auto* self = self_from_llhttp(p);
  self->state_ = http_parser_state::COMPLETE;
  if (self->msg_done_fn_) self->msg_done_fn_(self->msg_done_arg_);
  return 0;
}

int uvcpp_http_parser::ll_on_chunk_header(llhttp_t* p) {
  auto* self = self_from_llhttp(p);
  self->state_ = http_parser_state::CHUNK_HEADER;
  self->last_chunk_size_ = static_cast<size_t>(p->content_length);
  if (self->chunk_hdr_fn_) self->chunk_hdr_fn_(self->last_chunk_size_, self->chunk_hdr_arg_);
  // Transition to CHUNK_BODY when body data starts arriving
  if (self->last_chunk_size_ > 0) {
    self->state_ = http_parser_state::CHUNK_BODY;
  }
  return 0;
}

int uvcpp_http_parser::ll_on_chunk_complete(llhttp_t* p) {
  auto* self = self_from_llhttp(p);
  if (self->chunk_done_fn_) self->chunk_done_fn_(self->chunk_done_arg_);
  return 0;
}

int uvcpp_http_parser::ll_on_reset(llhttp_t* p) {
  // Called when llhttp resets internally (e.g., between Keep-Alive messages)
  auto* self = self_from_llhttp(p);
  self->state_ = http_parser_state::IDLE;
  return 0;
}

// =========================================================================
// extract_metadata — copy info from raw llhttp_t into C++ fields
// =========================================================================

void uvcpp_http_parser::extract_metadata() {
  // Method
  method_ = static_cast<http_method>(raw_p(raw_parser_)->method);

  // Status code
  status_code_ = static_cast<http_status>(raw_p(raw_parser_)->status_code);

  // Version
  if (raw_p(raw_parser_)->http_major == 1 && raw_p(raw_parser_)->http_minor == 0) {
    version_ = static_cast<uvcpp_http_version>(0);
  } else if (raw_p(raw_parser_)->http_major == 1 && raw_p(raw_parser_)->http_minor == 1) {
    version_ = static_cast<uvcpp_http_version>(1);
  } else if (raw_p(raw_parser_)->http_major == 2 && raw_p(raw_parser_)->http_minor == 0) {
    version_ = static_cast<uvcpp_http_version>(2);  // reserved for future
  } else {
    version_ = static_cast<uvcpp_http_version>(1);  // default
  }

  // Keep-Alive
  keep_alive_ = (llhttp_should_keep_alive(raw_p(raw_parser_)) != 0);

  // Content length
  content_length_ = raw_p(raw_parser_)->content_length;

  // Upgrade
  if (raw_p(raw_parser_)->upgrade != 0) {
    upgrade_ = true;
  }
}

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
