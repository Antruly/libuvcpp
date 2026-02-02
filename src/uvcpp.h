/**
 * @file src/uvcpp.h
 * @brief Single-include header for consumers of the uvcpp library.
 * @author zhuweiye
 * @version 1.0.0
 *
 * This header aggregates the most commonly used uvcpp headers for ease
 * of inclusion by downstream projects.
 */

#pragma once
#ifndef SRC_UVCPP_H
#define SRC_UVCPP_H

#include "uvcpp/uvcpp_define.h"
#include "uvcpp/uvcpp_version.h"
#include "uvcpp/uvcpp_export.h"

// Common handles and reqs
#include "handle/uvcpp_handle.h"
#include "handle/uvcpp_loop.h"
#include "req/uvcpp_req.h"

#endif // SRC_UVCPP_H
