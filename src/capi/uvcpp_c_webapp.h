/**
 * @file src/capi/uvcpp_c_webapp.h
 * @brief webapp 层的 C 面：app / 路由 / 中间件 / req / resp / next / 延迟应答 /
 *        静态目录 / WebSocket。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 先读 `uvcpp_c_common.h` 的五条承重规矩
 * --------------------------------------
 * 这一层一条不改地沿用它们（句柄 + 魔数、异常不过边界、回调表以 `size` 打头、
 * 三类所有权、循环亲和），下面只写**这一批新逼出来的**那三件。
 *
 * 这一批新加的三件（都是 webapp 的形状逼出来的，不是顺手多给）
 * -----------------------------------------------------------
 * 1. **回调期句柄。** `uvcpp_c_req` / `uvcpp_c_resp` / `uvcpp_c_next` 这三件
 *    只在**那一次回调里**有效：进你的回调之前把它们登记进活句柄表，回调一返回
 *    立刻反登记 + 毒化。所以回调返回之后再用，一律 `UVCPP_C_E_STALE`
 *    —— **不是崩溃**，与本层别的句柄同一条规矩（先查表、再读魔数）。
 *    要把"这次请求"带出回调，**只有一条路**：`uvcpp_c_defer()`。
 *
 * 2. **延迟应答包的是 `next`，不是别的。** 框架里"晚点再答"的机制是「**留住
 *    `next`**」—— 链会挂起、上下文由那份 `next` 自己续命、之后在**任何线程**上
 *    调它都会由框架投回**正确的那条循环**。三处细节写在 `uvcpp_c_defer()` 旁边。
 *
 * 3. **线程规则的两处例外**：`uvcpp_c_deferred_resume()` 与
 *    `uvcpp_c_app_post_task()`
 *    可以从别的线程调；**别的函数都不行**（与 C++ 侧一致）。
 *
 * 两条**能力边界**（都是"框架没给钩子"，不是这一层懒）
 * --------------------------------------------------
 *   - `uvcpp_c_deferred_post()` 只在**单循环**（`set_loops(1)`，默认）下精确：
 *     C 面拿不到"这条请求在哪号循环上"，而多循环下**唯一**能精确投递的入口是
 *     `uvcpp_web_app::post(fn, loop_index)`，那需要 `loop_index`。所以多循环下
 *     它返回 `UVCPP_C_E_UNSUPPORTED`，**不**假装投到 0 号去。通用出路是那句
 *     老话：在中间件里 `uvcpp_c_defer()`、在路由处理器里答 —— 挂起那一段由
 *     框架自己投回（`uvcpp_c_deferred_resume()` 因此**没有**这条限制）。
 *   - `uvcpp_c_ws_conn` 是**回调期句柄**（与 req / resp 同一类），不是能存起来
 *     的长期句柄。理由：会话被销毁这件事框架**唯一**的钩子是
 *     `uvcpp_ws_sessions::set_retire_observer()`，而 `uvcpp_ws_server` 的
 *     分片表（`shards_`）是私有的；`uvcpp_ws_connection::set_retire_callback()`
 *     是**属主专用**（服务端已经占着那一格，覆盖它就会让会话再也回收不掉），
 *     而 `uvcpp_ws_connection::terminate()`（停机 / `close_all_sessions()` 走
 *     的就是它）**不调**用户的 `on_close`（它只置 `close_notified_`）。于是
 *     "这条连接没了"对 C 面**不可观测**，那就不能给它一枚用完之后会指向已释放
 *     对象的句柄。代价写清楚：**本批做不了"给另一条连接发消息"（广播 /
 *     服务端主动推送）** —— 那需要在回调之外留住一枚连接句柄。回显、给"递进来
 *     的那条连接"发消息、关它自己，都在。
 *
 * 一个最小的 webapp（也是 `tests/capi/capi_webapp_func.c` 的骨架）
 * --------------------------------------------------------------
 * @code
 *   static void on_hello(void* ud, uvcpp_c_req* req, uvcpp_c_resp* resp,
 *                        uvcpp_c_next* next) {
 *     (void)ud; (void)req; (void)next;
 *     uvcpp_c_resp_text(resp, "hello");
 *     uvcpp_c_resp_end(resp);        // 不调 next() 就是"这条链到此为止"
 *   }
 *
 *   uvcpp_c_app* app = uvcpp_c_app_new();
 *   uvcpp_c_app_set_port(app, 0);            // 0 = 让内核挑
 *   uvcpp_c_app_get(app, "/hello", on_hello, NULL);
 *   if (uvcpp_c_app_start_background(app) != UVCPP_C_OK) { // 处理失败 }
 *   int port = uvcpp_c_app_bound_port(app);
 *   // ... 在同一个进程里用一个 client 打 http://127.0.0.1:port/hello ...
 *   uvcpp_c_app_stop(app);
 *   uvcpp_c_app_join(app);
 *   uvcpp_c_app_free(app);
 * @endcode
 *
 * 不提供（刻意）
 * --------------
 *   - `uvcpp_web_next` 的 C++ 类型本身：它在 C 里就是 `uvcpp_c_next` + 一个
 *     `uvcpp_c_next_run()`。"不调 next" 是终止链的唯一表达，与 C++ 侧同义。
 *   - `uvcpp_web_context` 与 `uvcpp_web_context_host`：`enable_shared_from_this`
 *     + 纯虚基类跨不了 C，而且它是框架内部的生命周期机。**C 面不需要它** ——
 *     它唯一被 C 需要的能力（"晚点再答"）由 `uvcpp_c_deferred` 那一族给。
 *   - `req.json(uvcpp_json&)` / `resp.json(const uvcpp_json&)` / 模板
 *     `uvcpp_from_json<T>`：`uvcpp_json` 是 `nlohmann::json`，不进 C 头。
 *     响应侧给 `uvcpp_c_resp_json_str()`（一个字符串），请求侧给 body 原始字节；
 *     C# 侧本来就用 `System.Text.Json` 序列化成字符串再传。
 *   - `req.upload()` / `uvcpp_web_upload_result`：结果里全是
 *     `std::vector<uvcpp_web_upload_file>`，要再造一族句柄才装得下。上传这一批
 *     只给"配好目录 + 挂路由"（`uvcpp_c_app_set_upload_dir()` /
 *     `uvcpp_c_app_post_upload()`），**不给读结果** —— 这与 `doc/webapp-guide.md`
 *     里"上传的 C 面还没做"那一句是同一件事，不假装能做。
 *   - `resp.set_stream_sink()` / `resp.set_file_chunk_gate()`：参数是
 *     `uvcpp_web_stream_sink*`（纯虚基类）与 `uvcpp_web_work_limit*`。流式出口
 *     给的是显式窄口（`uvcpp_c_resp_begin_chunked()` +
 *     `uvcpp_c_resp_write_chunk()`），文件出口给的是
 *     `uvcpp_c_resp_send_file()`（内部自己建 transfer，不用调用方给闸门）。
 *   - `resp.body_move()` / `body_share()` / `adopt_tables()` / `yield_tables()`：
 *     缓冲所有权转移是 C++ 内部机制，C 侧只有"拷贝进去"这一种（规矩 4）。
 *   - `uvcpp_static_server`（web 层那个遗留的）：它自承无生产调用方，而 webapp
 *     的 `uvcpp_web_static` 是它的替代品。C 面只给后者。
 *   - `app.on_connection()` / `on_connection_close()` / `on_raw_tcp_data()` /
 *     `set_raw_data_claim()`：前两个的参数是 `uvcpp_tcp_client*`，最后一个的参数
 *     里还多一个 `uvcpp_web_raw_action` 的认领协议。要给它们就得把"库持有的连接"
 *     包成 C 句柄 —— 那是**第二条所有权模型**（批 1 那条"谁建谁废、服务端交出来
 *     的连接由框架回收"是第一条），本层不做第二条。要看连接，给的是计数
 *     （`uvcpp_c_app_connection_count()` / `_inflight_count()`）。
 *   - **WebSocket 广播 / 服务端主动推送**：需要一枚活得比回调久的连接句柄，
 *     理由见上面「两条能力边界」第二条。C++ 侧要做这件事是自己维护一张
 *     `uvcpp_ws_connection*` 表（`doc/webapp-guide.md` 的聊天室例子就是），
 *     而"会话什么时候死"在那边同样没有通知 —— 它是靠自己的业务状态（进房 /
 *     退房）追的。C 面不给这条路的**一半**（一枚会悬垂的句柄），因为给一半比
 *     不给坏。
 *   - TLS / WSS 相关（`enable_ssl*` / `enable_wss()` / `set_ws_compression()`）：
 *     `uvcpp_ssl_context` 的生命周期与 C 句柄是两套东西，等 ssl 的 C 面定下来
 *     一起给。**因此本批的 `uvcpp_c_app_enable_wss()` 不存在**，也不提供一个
 *     "恒为假"的占位 —— 与 `uvcpp_c_net.h` 里 `is_tls()` 那两条不同，那两条
 *     是**查询**（答 0 是实话），而这里是一个**动作**：给不出一个真的能加密的
 *     动作，就不要给一个假的。
 */

#pragma once
#ifndef SRC_CAPI_UVCPP_C_WEBAPP_H
#define SRC_CAPI_UVCPP_C_WEBAPP_H

#include <stddef.h>
#include <stdint.h>

#include "capi/uvcpp_c_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------
 * 句柄
 * ------------------------------------------------------------------------
 * 定义全在 `uvcpp_c_webapp.cpp` 里，头里永远是不完整类型。
 *
 * 前四个是**回调期句柄**（见文件开头第 1 条），后两个不是：
 *   - `uvcpp_c_app`     ：你建的、你 `_free()` 的，活到你废它为止；
 *   - `uvcpp_c_deferred`：`uvcpp_c_defer()` 建的，活到你 `_free()` 为止，
 *                        而且**它活着的那段时间里 req / resp 也活着**。
 */

/** @brief webapp 句柄。 */
typedef struct uvcpp_c_app uvcpp_c_app;

/** @brief 一次请求（回调期句柄）。 */
typedef struct uvcpp_c_req uvcpp_c_req;

/** @brief 一次响应（回调期句柄；延迟应答期间由 `uvcpp_c_deferred` 续着）。 */
typedef struct uvcpp_c_resp uvcpp_c_resp;

/** @brief 链上"继续往下走"的那一枚（回调期句柄）。 */
typedef struct uvcpp_c_next uvcpp_c_next;

/** @brief 延迟应答句柄。**唯一**能把一次请求带出回调的东西。 */
typedef struct uvcpp_c_deferred uvcpp_c_deferred;

/** @brief 一次 WebSocket 升级请求（回调期句柄）。 */
typedef struct uvcpp_c_ws_req uvcpp_c_ws_req;

/**
 * @brief 一条 WebSocket 连接（**回调期句柄**）。
 *
 * 与 `uvcpp_c_req` / `uvcpp_c_resp` 同一类：只在**递给你它的那一次回调里**
 * 有效（升级回调里由 `uvcpp_c_ws_req_conn()` 给，之后由各事件回调的第二个
 * 参数给）。回调返回之后再用它一律 `E_STALE` —— **不是崩溃**。
 *
 * **为什么不能存起来**：框架没有把"这条会话被销毁了"暴露到 C 面够得着的任何
 * 一个钩子上（详见文件开头「两条能力边界」）。存起来的句柄在连接死后会指向一块
 * 已释放的内存，那种"偶尔崩、看运气"的失败模式正是这一层最不该有的东西。
 *
 * 于是**本批不提供广播 / 服务端主动推送**：那需要一枚活得比回调久的连接句柄。
 */
typedef struct uvcpp_c_ws_conn uvcpp_c_ws_conn;

/* ------------------------------------------------------------------------
 * 回调
 * ------------------------------------------------------------------------ */

/**
 * @brief 一条路由（或一条中间件）的处理函数。
 *
 * 三个句柄都**只在本次回调里有效**；带出去要走 `uvcpp_c_defer(next)`。
 *
 * 关于"这条链继不继续"，与 C++ 侧**逐字同义**：
 *   - 调 `uvcpp_c_next_run(next)` → 继续走链上的下一个（下一个可能是别的中间件，
 *     也可能是最终那条路由）；
 *   - 既不发响应、也不调 next → 链到此为止（**框架不会替你把响应发出去**，
 *     除非你在更后面还有元素把响应写好）；
 *   - 调 `uvcpp_c_defer(next)` → 链挂起，等 `uvcpp_c_deferred_resume()` 再继续。
 *
 * @param user_data 注册时给的那个指针，原样传回。
 *
 * 在**循环线程**上调用，而且是热路上：**不要在这里做重活**。
 */
typedef void (*uvcpp_c_route_cb)(void* user_data,
                                 uvcpp_c_req* req,
                                 uvcpp_c_resp* resp,
                                 uvcpp_c_next* next);

/**
 * @brief 一条 WebSocket 路由的处理函数。
 *
 * 在这里给连接挂事件（`uvcpp_c_ws_conn_set_events()`）并可以立刻发第一帧。
 * 回调返回之后 `uvcpp_c_ws_req` 失效，但 `uvcpp_c_ws_req_conn()` 拿到的那个
 * `uvcpp_c_ws_conn*` 继续有效。
 */
typedef void (*uvcpp_c_ws_cb)(void* user_data, uvcpp_c_ws_req* req);

/** @brief 无参数回调（`uvcpp_c_app_post_task()` 用）。 */
typedef void (*uvcpp_c_void_cb)(void* user_data);

/**
 * @brief 延迟应答的"可以答了"回调（`uvcpp_c_deferred_post()` 用）。
 *
 * 在**请求所属的那条循环线程**上被调用 —— 所以在这个回调里写响应是安全的。
 * 通常它就三件事：写响应、`uvcpp_c_deferred_resume()`、`uvcpp_c_deferred_free()`。
 */
typedef void (*uvcpp_c_deferred_cb)(void* user_data, uvcpp_c_deferred* deferred);

/** @brief 响应发送结果。与 C++ 的 `uvcpp_web_sent_info` 同一套字段。 */
typedef struct uvcpp_c_sent_info {
  uint32_t size;          /**< = sizeof(uvcpp_c_sent_info) */
  int      status_code;   /**< 最终状态码 */
  size_t   body_bytes;    /**< 实际写出去的 body 字节数（HEAD 时为 0）*/
  uint64_t connection_id; /**< 所属连接 id（0 = 未知）*/
  int      ok;            /**< 写成功 1 / 失败 0（连接已断等）*/
  int      streamed;      /**< 这次是不是流式（chunked）发的 */
} uvcpp_c_sent_info;

/**
 * @brief "响应真的发出去了"的通知。
 *
 * @param info 只在本次回调里有效，要留就当场拷走。
 *
 * 定义排在上面那个结构体**之后**：C 里 `uvcpp_c_sent_info` 这个 typedef 名
 * 要等结构体那句才诞生，先用它就是"不认得的类型名"。
 */
typedef void (*uvcpp_c_sent_cb)(void* user_data, const uvcpp_c_sent_info* info);

/* ------------------------------------------------------------------------
 * WebSocket：关闭码与事件表
 * ------------------------------------------------------------------------ */

/** @brief WebSocket 关闭码。值与 C++ 侧 `ws_close_code` 逐条相同。 */
enum uvcpp_c_ws_close_code {
  UVCPP_C_WS_NORMAL           = 1000,
  UVCPP_C_WS_GOING_AWAY       = 1001,
  UVCPP_C_WS_PROTOCOL_ERROR   = 1002,
  UVCPP_C_WS_UNSUPPORTED_DATA = 1003,
  UVCPP_C_WS_NO_STATUS        = 1005,
  UVCPP_C_WS_ABNORMAL_CLOSE   = 1006,
  UVCPP_C_WS_INVALID_PAYLOAD  = 1007,
  UVCPP_C_WS_POLICY_VIOLATION = 1008,
  UVCPP_C_WS_MESSAGE_TOO_BIG  = 1009,
  UVCPP_C_WS_EXTENSION_NEEDED = 1010,
  UVCPP_C_WS_INTERNAL_ERROR   = 1011
};

/**
 * @brief 一条 WebSocket 连接上会发生的四件事。
 *
 * 与别的事件表同一条规矩：**以 `size` 打头、逐字段看**。所以将来往表尾加格子
 * 不会破坏今天编出来的客户端；没覆盖到的格子当"这件事我不关心"，永不调用。
 *
 * `on_text` / `on_binary` 的 `data` 只在本次回调里有效（**二进制安全**：
 * 文本消息也可能含 NUL，一律按 `len` 走，别用 `strlen`）。
 */
typedef struct uvcpp_c_ws_events {
  uint32_t size;

  /** 收到一条文本消息（`data` / `len` 有效，`len` 是字节数）。 */
  void (*on_text)(void* user_data, uvcpp_c_ws_conn* conn,
                  const char* data, size_t len);

  /** 收到一条二进制消息。语义与 `on_text` 相同，只是 opcode 不同。 */
  void (*on_binary)(void* user_data, uvcpp_c_ws_conn* conn,
                    const char* data, size_t len);

  /** 连接关了。`code` 是 @ref uvcpp_c_ws_close_code；`reason` 可含 NUL。 */
  void (*on_close)(void* user_data, uvcpp_c_ws_conn* conn, int code,
                   const char* reason, size_t reason_len);

  /** 出错。`status` 是 libuv 错误码或本层错误码，`what` 是一句话。 */
  void (*on_error)(void* user_data, uvcpp_c_ws_conn* conn, int status,
                   const char* what, size_t what_len);
} uvcpp_c_ws_events;

/* ------------------------------------------------------------------------
 * 静态目录：选项表
 * ------------------------------------------------------------------------ */

/** @brief 以点开头的文件（`.env`、`.git/...`）怎么办。值与 C++ 侧相同。 */
enum uvcpp_c_dotfile_policy {
  UVCPP_C_DOTFILE_HIDE  = 0, /**< 当作不存在（404）。C++ 侧默认，这里也是。 */
  UVCPP_C_DOTFILE_DENY  = 1, /**< 明确拒绝（403）。 */
  UVCPP_C_DOTFILE_ALLOW = 2  /**< 照常提供。 */
};

/**
 * @brief `uvcpp_c_app_serve_static()` 的选项。**以 `size` 打头**。
 *
 * 用法：`{0}` 全清零（= 全部取默认值），或者逐格填完之后
 * `opt.size = (uint32_t)sizeof(opt);` —— 后者是唯一需要你自己写的一行。
 * 传 `NULL` 等价于"全默认"。
 *
 * 注意**目录与 URL 前缀不在这张表里**：它们是那次调用的两个字符串参数
 * （表是"选项"，不是"参数"，混在一起会让"表尾加格子"这件事变得危险）。
 */
typedef struct uvcpp_c_static_options {
  uint32_t size;

  /**
   * 目录请求时按顺序试的文件名，**逗号分隔**（如 `"index.html,index.htm"`）。
   * NULL = 用 C++ 侧的默认值（`index.html`）。空串 = **不试**任何索引文件
   * （目录请求直接 404 或走 SPA 兜底）。
   */
  const char* index_files;

  /** 非 0：找不到的路径回落到 `spa_file`（单页应用那套）。 */
  int spa_fallback;
  /** SPA 兜底用的文件（相对根目录）。仅在 `spa_fallback` 非 0 时有意义。 */
  const char* spa_file;

  /** 单个文件的大小上限（字节）。0 = 用默认值。 */
  size_t max_file_size;

  /** 要不要发 `ETag`（1/0）。默认 1。 */
  int etag;
  /** 要不要发 `Last-Modified`（1/0）。默认 1。 */
  int last_modified;
  /** 要不要支持 `Range`（1/0）。默认 1。 */
  int range;

  /** `Cache-Control` 的值；NULL = 不发这个头。 */
  const char* cache_control;

  /** 元数据缓存的格数与字节上限。0 = 用默认值。 */
  size_t cache_max_entries;
  size_t cache_max_bytes;

  /** @ref uvcpp_c_dotfile_policy */
  int dotfiles;

  /** 要不要跟随符号链接（1/0）。默认 0（不跟随）。 */
  int follow_symlinks;

  /** 要不要给文本类型补 `; charset=utf-8`（1/0）。默认 1。 */
  int add_charset;
} uvcpp_c_static_options;

/* ------------------------------------------------------------------------
 * app：建与废
 * ------------------------------------------------------------------------ */

/**
 * @brief 建一个 app。**默认是"什么都没配"的默认值**（端口 0、单循环、
 *        不开压缩、不开访问日志）—— 配置项都是下面那一串 `_set_*`。
 *
 * @return 新句柄；失败返回 NULL（只有内存不够会走到）。
 */
UVCPP_C_API uvcpp_c_app* uvcpp_c_app_new(void);

/**
 * @brief 废掉它（含它内部那些循环与连接）。
 *
 * **必须在回调之外调**：在某次回调里调它返回 `UVCPP_C_E_STATE`
 * （与本层其它句柄同一条规矩 —— 回调里删对象是 FFI 侧最常见的自毁方式）。
 * 还没 `stop()` + `join()` 就废，会先替你收尾一次（等价于
 * `stop(); join();` 再析构）。
 */
UVCPP_C_API int uvcpp_c_app_free(uvcpp_c_app* app);

/* ------------------------------------------------------------------------
 * app：配置（都要在 start() 之前）
 * ------------------------------------------------------------------------
 * 每一条的返回值都是 0 / 负数错误码。**这里与 C++ 侧有一个刻意的差别**：
 * C++ 那些链式 setter 返回 `uvcpp_web_app&` 是为了能一路写下来；C 里没有链式
 * 调用这一说，所以一律给错误码 —— 那比"恒为 0 的引用"有用。
 */

/** @brief 监听地址。NULL = 默认（`127.0.0.1`）。 */
UVCPP_C_API int uvcpp_c_app_set_host(uvcpp_c_app* app, const char* host);
/** @brief 监听端口。**0 表示让内核挑**，之后用 `uvcpp_c_app_bound_port()` 问。 */
UVCPP_C_API int uvcpp_c_app_set_port(uvcpp_c_app* app, int port);
/** @brief `listen()` 的 backlog。默认 128。 */
UVCPP_C_API int uvcpp_c_app_set_backlog(uvcpp_c_app* app, int backlog);
/** @brief 请求 body 上限（字节）。超了给 413。 */
UVCPP_C_API int uvcpp_c_app_set_max_body_size(uvcpp_c_app* app, size_t bytes);
/** @brief 请求头总字节上限。 */
UVCPP_C_API int uvcpp_c_app_set_max_header_bytes(uvcpp_c_app* app, size_t bytes);
/** @brief 请求行（URL）上限。 */
UVCPP_C_API int uvcpp_c_app_set_max_url_bytes(uvcpp_c_app* app, size_t bytes);
/** @brief 响应压缩开关（1/0）。没编进 zlib 时开着也不报错，只是不压。 */
UVCPP_C_API int uvcpp_c_app_set_compression(uvcpp_c_app* app, int enable);
/** @brief 访问日志开关（1/0）。 */
UVCPP_C_API int uvcpp_c_app_set_access_log(uvcpp_c_app* app, int enable);
/** @brief `Server:` 头的值。NULL = 不发这个头。 */
UVCPP_C_API int uvcpp_c_app_set_server_header(uvcpp_c_app* app,
                                              const char* value);
/** @brief 停机优雅窗口（毫秒）。 */
UVCPP_C_API int uvcpp_c_app_set_shutdown_grace_ms(uvcpp_c_app* app, int ms);
/** @brief 空闲连接超时（毫秒）。0 = 不超时。 */
UVCPP_C_API int uvcpp_c_app_set_idle_timeout_ms(uvcpp_c_app* app, int ms);
/** @brief 自动应答 `OPTIONS`（1/0）。 */
UVCPP_C_API int uvcpp_c_app_set_auto_options(uvcpp_c_app* app, int enable);
/** @brief `HEAD` 按 `GET` 处理再丢掉 body（1/0）。 */
UVCPP_C_API int uvcpp_c_app_set_head_as_get(uvcpp_c_app* app, int enable);
/** @brief 同一条连接上最多流水线几个请求。 */
UVCPP_C_API int uvcpp_c_app_set_max_pipelined_requests(uvcpp_c_app* app,
                                                       size_t n);

/**
 * @brief 上传落盘的目录。**设了它，`post_upload` 那条路才有地方写文件。**
 *
 * 目录必须已经存在（本层不替你 `mkdir`：那是"静默替调用方决定文件系统状态"，
 * 而失败方式会是"上传回 500 而日志里什么都没有"）。
 */
UVCPP_C_API int uvcpp_c_app_set_upload_dir(uvcpp_c_app* app, const char* dir);
/** @brief 单次请求上传总字节上限。 */
UVCPP_C_API int uvcpp_c_app_set_max_upload_size(uvcpp_c_app* app, uint64_t bytes);
/** @brief 单个上传文件的大小上限。 */
UVCPP_C_API int uvcpp_c_app_set_max_file_size(uvcpp_c_app* app, uint64_t bytes);

/**
 * @brief 事件循环条数（1..64）。
 *
 * 必须在 `start()` / `start_background()` **之前**调，否则 `UV_EINVAL` /
 * `UV_EBUSY`。**它是本层唯一一处"多循环会影响 C 面能力"的地方**：
 * `uvcpp_c_deferred_post()` 只在单循环下能精确工作，见那一条的注释。
 */
UVCPP_C_API int uvcpp_c_app_set_loops(uvcpp_c_app* app, int n);

/** @brief 当前配了多少条循环（默认 1）。 */
UVCPP_C_API int uvcpp_c_app_loop_count(uvcpp_c_app* app);

/* ------------------------------------------------------------------------
 * app：路由与中间件
 * ------------------------------------------------------------------------
 * `pattern` 的语法与 C++ 侧同一套（`/users/:id`、`*` 通配），**不在这里另立一套**。
 *
 * 注册顺序**有意义**：`use()` 进来的中间件按注册顺序排在路由之前，先注册先跑。
 * 这与 C++ 侧逐字相同（`tests/capi/capi_webapp_func.c` 用调用次序把它钉住了）。
 */

/** 一条路由/中间件的注册。`user_data` 原样传回回调。 */
UVCPP_C_API int uvcpp_c_app_get(uvcpp_c_app* app, const char* pattern,
                                uvcpp_c_route_cb cb, void* user_data);
UVCPP_C_API int uvcpp_c_app_post(uvcpp_c_app* app, const char* pattern,
                                 uvcpp_c_route_cb cb, void* user_data);
UVCPP_C_API int uvcpp_c_app_put(uvcpp_c_app* app, const char* pattern,
                                uvcpp_c_route_cb cb, void* user_data);
UVCPP_C_API int uvcpp_c_app_del(uvcpp_c_app* app, const char* pattern,
                                uvcpp_c_route_cb cb, void* user_data);
UVCPP_C_API int uvcpp_c_app_patch(uvcpp_c_app* app, const char* pattern,
                                  uvcpp_c_route_cb cb, void* user_data);
UVCPP_C_API int uvcpp_c_app_head(uvcpp_c_app* app, const char* pattern,
                                 uvcpp_c_route_cb cb, void* user_data);
UVCPP_C_API int uvcpp_c_app_options(uvcpp_c_app* app, const char* pattern,
                                    uvcpp_c_route_cb cb, void* user_data);
UVCPP_C_API int uvcpp_c_app_any(uvcpp_c_app* app, const char* pattern,
                                uvcpp_c_route_cb cb, void* user_data);

/** @brief 挂一条中间件（与路由同一个回调形状）。 */
UVCPP_C_API int uvcpp_c_app_use(uvcpp_c_app* app, uvcpp_c_route_cb cb,
                                void* user_data);

/**
 * @brief 一条上传路由（`multipart/form-data`）。
 *
 * 回调里**读得到字段与文件**吗：读不到 —— 结果类型（`uvcpp_web_upload_result`）
 * 这一批没有 C 面（见文件开头的"不提供"）。这条函数给的是"上传能收下来、
 * 文件落到 `uvcpp_c_app_set_upload_dir()` 指定的目录里"，而你在这个回调里
 * 决定回什么给客户端。
 */
UVCPP_C_API int uvcpp_c_app_post_upload(uvcpp_c_app* app, const char* pattern,
                                        uvcpp_c_route_cb cb, void* user_data);

/**
 * @brief 把 `root_dir` 挂在 URL 前缀 `prefix` 上。
 *
 * @param options `uvcpp_c_static_options`；**可以传 NULL**（= 全默认）。
 *
 * 挂完之后不再回句柄：缓存格数、命中数那些是**单元测试用的观测口**，不属于
 * 对外 ABI（见文件开头的"不提供"）。
 */
UVCPP_C_API int uvcpp_c_app_serve_static(uvcpp_c_app* app, const char* prefix,
                                         const char* root_dir,
                                         const uvcpp_c_static_options* options);

/* ------------------------------------------------------------------------
 * app：WebSocket 路由
 * ------------------------------------------------------------------------ */

/**
 * @brief 挂一条 WebSocket 路由（`GET` + `Upgrade`）。
 *
 * 内部先做握手，握手成功后按 `uvcpp_c_ws_cb` 调你。**不收 WebSocket 压缩配置**
 * （`uvcpp_ws_deflate_config` 是 C++ 类型，且压不压是两端的协商），
 * 见文件开头的"不提供"。
 */
UVCPP_C_API int uvcpp_c_app_websocket(uvcpp_c_app* app, const char* pattern,
                                      uvcpp_c_ws_cb cb, void* user_data);

/* ------------------------------------------------------------------------
 * app：起停与读数
 * ------------------------------------------------------------------------ */

/**
 * @brief 在当前线程跑事件循环（**阻塞**，直到 `uvcpp_c_app_stop()`）。
 *
 * @return 0；负数错误码（配置不对、端口被占等）。
 */
UVCPP_C_API int uvcpp_c_app_start(uvcpp_c_app* app);

/**
 * @brief 起后台线程跑事件循环，**立刻返回**。
 *
 * 这是"C 程序里同时要有服务端和客户端"的常用形状（用例就是这么打的自己）。
 * 返回 0 之后循环可能还没起来 —— 要确定它开始服务了，请轮询
 * `uvcpp_c_app_bound_port()` 或直接打一次。
 */
UVCPP_C_API int uvcpp_c_app_start_background(uvcpp_c_app* app);

/** @brief 让循环停下来（可从别的线程调：libuv 的 `uv_stop` 会唤醒循环）。 */
UVCPP_C_API int uvcpp_c_app_stop(uvcpp_c_app* app);

/** @brief 等后台线程退出。配合 `uvcpp_c_app_start_background()` 用。 */
UVCPP_C_API int uvcpp_c_app_join(uvcpp_c_app* app);

/**
 * @brief 实际绑到的端口。`set_port(0)` 之后从这里拿真实端口。
 *
 * 还没 `start*()` 时返回 0（那时还没有"绑到了哪"这件事）。
 */
UVCPP_C_API int uvcpp_c_app_bound_port(uvcpp_c_app* app);

/** @brief 循环是否在跑（1/0）。 */
UVCPP_C_API int uvcpp_c_app_running(uvcpp_c_app* app);

/** @brief 当前连着的连接数（所有循环加起来）。 */
UVCPP_C_API size_t uvcpp_c_app_connection_count(uvcpp_c_app* app);

/** @brief 第 `loop_index` 条循环上的连接数（越界返回 0）。 */
UVCPP_C_API size_t uvcpp_c_app_connection_count_at(uvcpp_c_app* app,
                                                   int loop_index);

/** @brief 正在处理（已收下、还没写完响应）的请求数。 */
UVCPP_C_API size_t uvcpp_c_app_inflight_count(uvcpp_c_app* app);

/** @brief 活着的 WebSocket 会话数。 */
UVCPP_C_API size_t uvcpp_c_app_ws_session_count(uvcpp_c_app* app);

/**
 * @brief 把 `cb` 投到 app 的循环上（**可以从别的线程调**）。
 *
 * **单循环下语义精确**（唯一那条循环就是每一格请求所在的那条）。
 * `set_loops(n)` 且 `n > 1` 时本层返回 `UVCPP_C_E_UNSUPPORTED` 而不是
 * "投到 0 号去"：C++ 的 `post(fn)` 从非循环线程投递时也是落 0 号，但 C++ 那边
 * 调用方手里有 `uvcpp_web_context`，能用 `ctx->post()` 走精确的那条；**C 面拿不到
 * context**（它没有任何一条通路能从 req / resp / next 反推，见 `uvcpp_c_defer()`），
 * 所以本层不假装。多循环下能精确投回**请求所属循环**的，只有框架自己那条路
 * （`uvcpp_c_deferred_resume()` —— `next` 自己知道它是哪条循环的），别的都靠
 * 调用方在别的线程上把数据备好。
 */
UVCPP_C_API int uvcpp_c_app_post_task(uvcpp_c_app* app, uvcpp_c_void_cb cb,
                                      void* user_data);

/* ------------------------------------------------------------------------
 * req：读请求（回调期）
 * ------------------------------------------------------------------------
 * 凡是把字符串取出来的，都走 `uvcpp_c_common.h` 那条"调用方给缓冲区"的约定：
 * 返回真实长度、`cap` 够才写、不返回分配的内存。**查不到**返回
 * `UVCPP_C_E_NOT_FOUND`（不是 0 —— 0 是"查到了，是空串"，两者在 HTTP 里不同）。
 */

/** @brief 方法名（`"GET"` / `"POST"` …）。 */
UVCPP_C_API int uvcpp_c_req_method_name(uvcpp_c_req* req, char* buf, size_t cap);
/** @brief 解码后的路径（不含 query）。 */
UVCPP_C_API int uvcpp_c_req_path(uvcpp_c_req* req, char* buf, size_t cap);
/** @brief 原始（未解码）路径。 */
UVCPP_C_API int uvcpp_c_req_raw_path(uvcpp_c_req* req, char* buf, size_t cap);
/** @brief 原始 query 串（`a=1&b=2`，含前导 `?` 时不带）。 */
UVCPP_C_API int uvcpp_c_req_query_string(uvcpp_c_req* req, char* buf,
                                         size_t cap);
/** @brief 一个 query 参数。 */
UVCPP_C_API int uvcpp_c_req_query(uvcpp_c_req* req, const char* name, char* buf,
                                  size_t cap);
/** @brief 一个请求头（大小写不敏感）。 */
UVCPP_C_API int uvcpp_c_req_header(uvcpp_c_req* req, const char* name, char* buf,
                                   size_t cap);
/** @brief 有没有这个请求头（1/0）。 */
UVCPP_C_API int uvcpp_c_req_has_header(uvcpp_c_req* req, const char* name);
/** @brief `Content-Type`。 */
UVCPP_C_API int uvcpp_c_req_content_type(uvcpp_c_req* req, char* buf,
                                         size_t cap);
/** @brief `Content-Length`（没有这个头时为 0）。 */
UVCPP_C_API size_t uvcpp_c_req_content_length(uvcpp_c_req* req);
/** @brief `Host` 头。 */
UVCPP_C_API int uvcpp_c_req_host(uvcpp_c_req* req, char* buf, size_t cap);
/** @brief 对端 IP（点分十进制 / 冒号十六进制）。 */
UVCPP_C_API int uvcpp_c_req_peer_ip(uvcpp_c_req* req, char* buf, size_t cap);
/** @brief 对端端口。 */
UVCPP_C_API unsigned int uvcpp_c_req_peer_port(uvcpp_c_req* req);
/** @brief 路由参数（`/users/:id` 里的 `id`）。 */
UVCPP_C_API int uvcpp_c_req_param(uvcpp_c_req* req, const char* name, char* buf,
                                  size_t cap);
/** @brief 一个 cookie。 */
UVCPP_C_API int uvcpp_c_req_cookie(uvcpp_c_req* req, const char* name, char* buf,
                                   size_t cap);
/** @brief 这条连接想不想 keep-alive（1/0）。 */
UVCPP_C_API int uvcpp_c_req_is_keep_alive(uvcpp_c_req* req);

/**
 * @brief body 的原始字节（**二进制安全**）。
 *
 * 与"调用方给缓冲区"那套刻意不同：这里给的是**指针 + 长度**，因为 body 可能
 * 很大，而调用方常常只想扫一遍（拷进它自己的缓冲、算个哈希）。
 *
 * @param data 出参：指向 body 的第一个字节（空 body 时可能是 NULL）。
 * @param len  出参：body 的字节数。
 *
 * @return `UVCPP_C_OK`，或负数错误码（句柄失效）。返回 `int` 而不是长度，
 *         是因为 `size_t` 报不了"句柄是空的"这件事。
 *
 * @warning `*data` **只在本次回调里有效**，要留就自己拷走。
 */
UVCPP_C_API int uvcpp_c_req_body(uvcpp_c_req* req, const char** data,
                                 size_t* len);
/** @brief body 是不是空（1/0）。 */
UVCPP_C_API int uvcpp_c_req_body_empty(uvcpp_c_req* req);

/* ------------------------------------------------------------------------
 * resp：写响应（回调期 / 延迟应答期）
 * ------------------------------------------------------------------------
 * 这些函数**返回 int 而不是链式引用**：`resp.status(200).text("x")` 那种形状
 * 在 C 里没有对应物，而"每一步都能失败"这件事必须能被看见。
 *
 * 写成响应之后必须 `uvcpp_c_resp_end()`。**不调 end 就等于没答**
 * （框架不会替你发 —— 见 `uvcpp_c_route_cb` 的说明）。
 */

/** @brief 设状态码。 */
UVCPP_C_API int uvcpp_c_resp_status(uvcpp_c_resp* resp, int code);
/** @brief 设状态行里的原因短语（默认由状态码推）。 */
UVCPP_C_API int uvcpp_c_resp_status_message(uvcpp_c_resp* resp,
                                            const char* msg);
/** @brief 当前状态码。 */
UVCPP_C_API int uvcpp_c_resp_status_code(uvcpp_c_resp* resp);
/** @brief 设一个响应头（同名覆盖）。 */
UVCPP_C_API int uvcpp_c_resp_set_header(uvcpp_c_resp* resp, const char* name,
                                        const char* value);
/** @brief 追加一个响应头（同名不覆盖，发两个）。 */
UVCPP_C_API int uvcpp_c_resp_add_header(uvcpp_c_resp* resp, const char* name,
                                        const char* value);
/** @brief 删掉一个响应头。 */
UVCPP_C_API int uvcpp_c_resp_remove_header(uvcpp_c_resp* resp,
                                           const char* name);
/** @brief 有没有这个响应头（1/0）。 */
UVCPP_C_API int uvcpp_c_resp_has_header(uvcpp_c_resp* resp, const char* name);
/** @brief 取一个响应头的值（`uvcpp_c_common.h` 的缓冲区约定）。 */
UVCPP_C_API int uvcpp_c_resp_get_header(uvcpp_c_resp* resp, const char* name,
                                        char* buf, size_t cap);
/** @brief 设一个响应头（`content_type` 的显式版，等价于 set_header）。 */
UVCPP_C_API int uvcpp_c_resp_set_content_type(uvcpp_c_resp* resp,
                                              const char* ct);
/** @brief 取 `Content-Type`。 */
UVCPP_C_API int uvcpp_c_resp_content_type(uvcpp_c_resp* resp, char* buf,
                                          size_t cap);

/**
 * @brief 写一个 cookie（`Set-Cookie`）。
 *
 * @param max_age 秒；`< 0` 表示不带 `Max-Age`（会话 cookie）。
 * @param same_site `"Lax"` / `"Strict"` / `"None"`；NULL = `"Lax"`。
 */
UVCPP_C_API int uvcpp_c_resp_set_cookie(uvcpp_c_resp* resp, const char* name,
                                        const char* value, const char* path,
                                        long max_age, int http_only, int secure,
                                        const char* same_site);

/** @brief 设 body（原始字节）。`content_type` 可以为 NULL（= 不动这个头）。 */
UVCPP_C_API int uvcpp_c_resp_body(uvcpp_c_resp* resp, const void* data,
                                  size_t len, const char* content_type);
/** @brief 设 body 为一段文本，并设 `Content-Type: text/plain; charset=utf-8`。 */
UVCPP_C_API int uvcpp_c_resp_text(uvcpp_c_resp* resp, const char* s);
/** @brief 同上，`text/html`。 */
UVCPP_C_API int uvcpp_c_resp_html(uvcpp_c_resp* resp, const char* s);
/** @brief 设 body 为一段**已经是 JSON 文本**的字节（本层不解析、不生成 JSON）。 */
UVCPP_C_API int uvcpp_c_resp_json_str(uvcpp_c_resp* resp, const char* json);
/** @brief 设 body 为二进制，并设 `Content-Type`（NULL = 默认
 *         `application/octet-stream`）。 */
UVCPP_C_API int uvcpp_c_resp_binary(uvcpp_c_resp* resp, const void* data,
                                    size_t len, const char* content_type);
/** @brief 当前 body 的字节数。 */
UVCPP_C_API size_t uvcpp_c_resp_body_size(uvcpp_c_resp* resp);
/** @brief 清空 body。 */
UVCPP_C_API int uvcpp_c_resp_clear_body(uvcpp_c_resp* resp);

/** @brief 302（或 `code`）重定向到 `url`。 */
UVCPP_C_API int uvcpp_c_resp_redirect(uvcpp_c_resp* resp, const char* url,
                                      int code);
/** @brief 404，body 是 `what`（NULL = 空）。 */
UVCPP_C_API int uvcpp_c_resp_not_found(uvcpp_c_resp* resp, const char* what);
/** @brief 400。 */
UVCPP_C_API int uvcpp_c_resp_bad_request(uvcpp_c_resp* resp, const char* what);
/** @brief 403。 */
UVCPP_C_API int uvcpp_c_resp_forbidden(uvcpp_c_resp* resp, const char* what);
/** @brief 413。 */
UVCPP_C_API int uvcpp_c_resp_payload_too_large(uvcpp_c_resp* resp,
                                               const char* what);
/** @brief 500。 */
UVCPP_C_API int uvcpp_c_resp_server_error(uvcpp_c_resp* resp, const char* what);

/**
 * @brief **把响应发出去。** 每次响应必须恰好调一次。
 *
 * 它在链的当前位置把响应交给框架（此后 `uvcpp_c_resp_*` 的写口一律
 * `UVCPP_C_E_STATE`）。在延迟应答那条路上，`end()` 之后还要
 * `uvcpp_c_deferred_resume()` —— 两件事：`end()` 说"响应是这个"，`resume()`
 * 说"链可以往下走了，框架可以发了"。
 */
UVCPP_C_API int uvcpp_c_resp_end(uvcpp_c_resp* resp);
/** @brief 已经 `end()` 过没有（1/0）。 */
UVCPP_C_API int uvcpp_c_resp_ended(uvcpp_c_resp* resp);

/** @brief 响应真的写完之后通知一次（`uvcpp_c_sent_cb`）。 */
UVCPP_C_API int uvcpp_c_resp_on_sent(uvcpp_c_resp* resp, uvcpp_c_sent_cb cb,
                                     void* user_data);

/* --- 流式（chunked）出口 ------------------------------------------------- */

/**
 * @brief 开始一段 chunked 响应（状态行与响应头**当场**发出去）。
 *
 * 之后用 `uvcpp_c_resp_write_chunk()` 一块块写，最后仍要 `uvcpp_c_resp_end()`。
 * `content_type` 为 NULL = `text/plain; charset=utf-8`。
 */
UVCPP_C_API int uvcpp_c_resp_begin_chunked(uvcpp_c_resp* resp,
                                           const char* content_type);
/** @brief 写一块（长度 0 合法：那是一块空的 chunk）。 */
UVCPP_C_API int uvcpp_c_resp_write_chunk(uvcpp_c_resp* resp, const void* data,
                                         size_t len);
/** @brief 写对端**消化得动**了再通知一次（背压用）。 */
UVCPP_C_API int uvcpp_c_resp_on_drain(uvcpp_c_resp* resp, uvcpp_c_void_cb cb,
                                      void* user_data);
/** @brief 是不是已经在流式发送（1/0）。 */
UVCPP_C_API int uvcpp_c_resp_streaming(uvcpp_c_resp* resp);
/** @brief 流式已写出的字节数。 */
UVCPP_C_API uint64_t uvcpp_c_resp_stream_bytes_written(uvcpp_c_resp* resp);

/* --- 文件出口 ------------------------------------------------------------ */

/**
 * @brief 把 `path` 这个文件当 body 发出去。
 *
 * 内部自己建 transfer、自己分片读 —— 所以**不需要** `set_file_chunk_gate()`
 * 那一族（C 侧不出现 `uvcpp_web_work_limit`）。
 *
 * @param done 完成时回调：`(user, status, bytes_sent)`；可为 NULL。
 *
 * 注意**它异步**：返回 0 只表示"开始发了"。
 */
UVCPP_C_API int uvcpp_c_resp_send_file(
    uvcpp_c_resp* resp, const char* path,
    void (*done)(void* user_data, int status, uint64_t bytes_sent),
    void* user_data);

/**
 * @brief 只发文件的 `[first, last]` 字节（闭区间，`Range` 请求用）。
 *
 * 与 `uvcpp_c_resp_send_file()` 同一条路，只是带了区间。
 */
UVCPP_C_API int uvcpp_c_resp_send_file_range(
    uvcpp_c_resp* resp, const char* path, uint64_t first, uint64_t last,
    void (*done)(void* user_data, int status, uint64_t bytes_sent),
    void* user_data);

/* ------------------------------------------------------------------------
 * next：继续走链
 * ------------------------------------------------------------------------ */

/** @brief 继续走链上的下一个。**同一枚 next 只能 run 一次**（第二次 `E_STATE`）。 */
UVCPP_C_API int uvcpp_c_next_run(uvcpp_c_next* next);

/* ------------------------------------------------------------------------
 * deferred：延迟应答
 * ------------------------------------------------------------------------
 * 这一族只解决一件事：**把这次请求带出回调**。四件事按顺序读一遍。
 *
 * 机制（不是本层发明的，是框架自己的形状）
 * ----------------------------------------
 * 框架这样判"这次请求是不是还没答完"：handler 返回时，如果**外面还留着一份
 * `next`**，链就挂起、上下文就不析构（那份 `next` 里的 `ctx_` 自己就是续命的
 * 引用）；等那份 `next` 被调用时，链接着走、走到头才由框架把响应发出去。
 * 所以"延迟应答"= **留住 next**，本层的 `uvcpp_c_deferred` 就是那份 next 的
 * 盒子（另加两个指针，见第 3 条）。
 *
 * 1. **不要**指望 `uvcpp_web_response::set_deferred(true)`：它在框架里**没有任何
 *    读取点**（`doc/webapp-guide.md` 也是这么写的），置它不改变任何行为。真正
 *    决定"框架替不替你发"的是上面说的"留没留 next"。
 * 2. **`uvcpp_c_deferred_resume()` 可以从任何线程调**：不在循环线程上时，
 *    框架会把续跑投回**正确的那条循环**（`next_resume_chain()` 里那两行）。
 *    所以"在工作线程里做完异步活、再调 resume"是**这一族唯一鼓励的跨线程形状**。
 * 3. `uvcpp_c_deferred_req()` / `_resp()` 给的句柄指向的还是**同一对对象**
 *    （按值挂在上下文里的那两个），只要 deferred 活着它们就活着。**但它们只能
 *    在请求所属那条循环线程上碰** —— 规矩 5 没有例外。
 * 4. `uvcpp_c_defer()` 之后，**原来那次回调收到的 req / resp / next 一律作废**
 *    （回调一返回它们就被毒化），一律走 `uvcpp_c_deferred_req()/_resp()`。
 *
 * 一个"晚点再答"的完整形状（`tests/capi/capi_webapp_func.c` 里那两条就是它）
 * -------------------------------------------------------------------------
 * @code
 *   // 路由：把它挂起，自己去别的线程干活
 *   static void on_slow(void* ud, uvcpp_c_req* req, uvcpp_c_resp* resp,
 *                       uvcpp_c_next* next) {
 *     (void)req; (void)resp;
 *     uvcpp_c_deferred* d = uvcpp_c_defer(next);   // 链挂起；d 里存着那份 next
 *     spawn_worker(d);                             // 你自己的线程
 *   }
 *
 *   // 工作线程：活干完了
 *   void worker_done(void* ud, uvcpp_c_deferred* d) {
 *     uvcpp_c_deferred_post(d, write_answer, ud);  // 投回它那条循环
 *   }
 *
 *   // 循环线程：这才可以碰 resp
 *   void write_answer(void* ud, uvcpp_c_deferred* d) {
 *     uvcpp_c_resp* r = uvcpp_c_deferred_resp(d);
 *     uvcpp_c_resp_text(r, "late but on time");
 *     uvcpp_c_resp_end(r);
 *     uvcpp_c_deferred_resume(d);   // 链继续 -> 走到头 -> 框架发出
 *     uvcpp_c_deferred_free(d);
 *   }
 * @endcode
 */

/**
 * @brief 拿走 `next`，把这枚请求挂起。
 *
 * @return 失败返回 NULL（NULL 入参 / 那枚 next 已经被 run 过 / 内存不够）。
 *
 * **拿了就一定要还**：每条 deferred 最终必须走上
 * `uvcpp_c_deferred_resume()`（继续）或 `uvcpp_c_deferred_free()`（放弃）之一。
 * 只 free 不 resume = 这个请求永远答不上、那条连接上的流水线也永远等不到它。
 */
UVCPP_C_API uvcpp_c_deferred* uvcpp_c_defer(uvcpp_c_next* next);

/**
 * @brief 这个请求自己的 req 句柄（只在**请求所属循环线程**上碰）。
 *
 * @return 指向 deferred 内部那枚句柄；**不要 free 它**，它随 deferred 一起走。
 */
UVCPP_C_API uvcpp_c_req* uvcpp_c_deferred_req(uvcpp_c_deferred* deferred);

/** @brief 同 `uvcpp_c_deferred_req()`，给的是 resp 句柄。 */
UVCPP_C_API uvcpp_c_resp* uvcpp_c_deferred_resp(uvcpp_c_deferred* deferred);

/**
 * @brief 把 `cb` 投到**这次请求所属那条循环**上（可从别的线程调）。
 *
 * **只在单循环（默认）下可用**，多循环下返回 `UVCPP_C_E_UNSUPPORTED` ——
 * C 面拿不到"这条请求在哪号循环上"，理由见文件开头「两条能力边界」第一条。
 * 单循环下它投的就是那条唯一的循环，所以语义精确。
 *
 * @return `UVCPP_C_OK`；多循环 `UVCPP_C_E_UNSUPPORTED`；循环已停时
 *         `UVCPP_C_E_STATE`（框架会打一条 WARN，与本层"静默丢弃投递任务
 *         = 悬案"的口径一致）。
 *
 * @warning `cb` **跑起来之前 deferred 必须还活着**（它是这条投递的凭据）。
 *          典型形状是投完就在 `cb` 里 `_resume()` + `_free()`。
 */
UVCPP_C_API int uvcpp_c_deferred_post(uvcpp_c_deferred* deferred,
                                      uvcpp_c_deferred_cb cb, void* user_data);

/**
 * @brief 继续走链（`next()`）。**可以从任何线程调。**
 *
 * 调它之前，响应应该已经写好并 `end()` 过；否则链走到头时发出去的是一个空响应。
 * 调完之后 deferred 还可以 `_free()`（那份 next 已经交出去了）。
 */
UVCPP_C_API int uvcpp_c_deferred_resume(uvcpp_c_deferred* deferred);

/**
 * @brief 放掉这个句柄（**不**继续链）。
 *
 * 在 `resume()` 之后调它是清理；在 `resume()` 之前调它就是"放弃这次请求"
 * —— 框架不会收到任何续作，那条请求会一直挂在链上（**不是崩溃，是泄漏**）。
 * 要"用错误码回掉它"，正确做法是 `_resp()` 写个 500 + `_resume()`，不是 free。
 */
UVCPP_C_API int uvcpp_c_deferred_free(uvcpp_c_deferred* deferred);

/* ------------------------------------------------------------------------
 * WebSocket：请求与连接
 * ------------------------------------------------------------------------ */

/** @brief 升级请求的路径（已解码）。 */
UVCPP_C_API int uvcpp_c_ws_req_path(uvcpp_c_ws_req* req, char* buf, size_t cap);
/** @brief 命中的路由模式（`uvcpp_c_app_websocket()` 注册的那个）。 */
UVCPP_C_API int uvcpp_c_ws_req_route(uvcpp_c_ws_req* req, char* buf, size_t cap);
/** @brief 路由参数。 */
UVCPP_C_API int uvcpp_c_ws_req_param(uvcpp_c_ws_req* req, const char* name,
                                     char* buf, size_t cap);
/** @brief query 参数。 */
UVCPP_C_API int uvcpp_c_ws_req_query(uvcpp_c_ws_req* req, const char* name,
                                     char* buf, size_t cap);
/** @brief cookie。 */
UVCPP_C_API int uvcpp_c_ws_req_cookie(uvcpp_c_ws_req* req, const char* name,
                                      char* buf, size_t cap);
/** @brief 升级请求的请求头。 */
UVCPP_C_API int uvcpp_c_ws_req_header(uvcpp_c_ws_req* req, const char* name,
                                      char* buf, size_t cap);
/** @brief 对端 IP。 */
UVCPP_C_API int uvcpp_c_ws_req_peer_ip(uvcpp_c_ws_req* req, char* buf,
                                       size_t cap);

/**
 * @brief 这条升级请求背后的连接。
 *
 * @return 连接句柄；**它比本次回调活得久**（升级回调返回之后照样能用它发消息、
 *         挂事件）。没有 `_free()`：它的生死由那条连接决定。
 */
UVCPP_C_API uvcpp_c_ws_conn* uvcpp_c_ws_req_conn(uvcpp_c_ws_req* req);

/**
 * @brief 给连接挂事件表（同一枚连接重复挂 = 覆盖）。
 *
 * 表**按值拷进框架**（事件回调存在那条会话对象里），所以 `events` 指向的那块
 * 内存返回之后你可以随便释放 —— 它只在这次调用里有意义。`user_data` 原样传回。
 */
UVCPP_C_API int uvcpp_c_ws_conn_set_events(uvcpp_c_ws_conn* conn,
                                           const uvcpp_c_ws_events* events,
                                           void* user_data);

/** @brief 发一条文本消息。**二进制安全**（按 `len` 走，别用 `strlen`）。 */
UVCPP_C_API int uvcpp_c_ws_conn_send_text(uvcpp_c_ws_conn* conn,
                                          const char* data, size_t len);
/** @brief 发一条二进制消息。 */
UVCPP_C_API int uvcpp_c_ws_conn_send_binary(uvcpp_c_ws_conn* conn,
                                            const char* data, size_t len);
/** @brief 发一个 ping。 */
UVCPP_C_API int uvcpp_c_ws_conn_send_ping(uvcpp_c_ws_conn* conn);
/** @brief 发一个 pong。 */
UVCPP_C_API int uvcpp_c_ws_conn_send_pong(uvcpp_c_ws_conn* conn);

/**
 * @brief 发一条 close 帧并开始关闭握手。
 *
 * @param code @ref uvcpp_c_ws_close_code（0 = 不发码）。
 * @param reason 可以为 NULL；`reason_len` 是它的字节数（**可以不 NUL 结尾**）。
 */
UVCPP_C_API int uvcpp_c_ws_conn_close(uvcpp_c_ws_conn* conn, int code,
                                      const char* reason, size_t reason_len);

/** @brief 立刻断链（不发 close 帧）。对端只会看到 TCP 断。 */
UVCPP_C_API int uvcpp_c_ws_conn_terminate(uvcpp_c_ws_conn* conn);

/** @brief 这条连接还在开着吗（1/0）。 */
UVCPP_C_API int uvcpp_c_ws_conn_is_open(uvcpp_c_ws_conn* conn);

/** @brief 单条消息的大小上限。 */
UVCPP_C_API int uvcpp_c_ws_conn_set_max_message_size(uvcpp_c_ws_conn* conn,
                                                     size_t bytes);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* SRC_CAPI_UVCPP_C_WEBAPP_H */
