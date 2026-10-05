/**
 * @file bindings/csharp/UvcppNative.Protocols.cs
 * @brief C ABI 的**协议三片**（http2 / quic / http3）的 C# P/Invoke 绑定。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一份是什么
 * ------------
 * `src/capi/` 是本库的纯 C 门面：不透明句柄 + `extern "C"` 函数 + C 函数指针
 * 回调（头是纯 C99，实现是 `.cpp`）。本文件把其中 **http2 / quic / http3 三片**
 * 的头（`src/capi/uvcpp_c_http2.h` / `uvcpp_c_quic.h` / `uvcpp_c_http3.h`）逐条
 * 镜像成 C# 的 `DllImport`，不增不减：C# 方法名就是 C 里的名字，参数与返回值按
 * `doc/capi-guide.md` §5 那几条映射（句柄 = `IntPtr` 别名；`char* buf` + `size_t
 * cap` = `byte[]` + `nuint`；`const char*` = UTF-8 字符串；`size_t` = `nuint`；
 * 函数指针 = `[UnmanagedFunctionPointer(Cdecl)]` 委托）。
 *
 * 它是 `UvcppNative` 的另一半
 * --------------------------
 * 本文件是 `partial class UvcppNative` 的**后半**：公共地基（错误码枚举、
 * `UVCPP_C_ABI_VERSION`、异常与错误信息类型）以及 common / net / web / webapp
 * 四片都在另一半 `UvcppNative.cs` 里。两半合起来才是完整的一层 —— 所以本文件
 * **不**重复声明 `UvcppError` / `UvcppCException` / `UvcppErrorInfo` /
 * `UVCPP_C_ABI_VERSION` / `private const string Lib`，也不重复声明那四片的任何
 * 方法或结构体。用到 net 的类型（`uvcpp_c_read_result`、`uvcpp_c_status_cb`）或
 * common 的 `Lib` 时，直接按另一半给的名字引用。
 *
 * 不透明句柄一律是 `IntPtr` 的文件级 `using` 别名（见下面的别名块）：`using`
 * 别名是**每个文件各自一份**，所以本文件与另一半会各声明自己用到的那几个，
 * 名字都取 C 类型名本身。不要给这些类型声明同名的 `struct` 去
 * `Marshal.PtrToStructure` —— 头里给的是不完整类型，布局没有承诺。
 *
 * 枚举命名的一句说明：C 里 `enum uvcpp_c_h2_stream_state` 与函数
 * `uvcpp_c_h2_stream_state()` 分属两个命名空间（tag 与普通标识符），C# 却把
 * 两者放进同一个类里，无法同名。所以枚举**类型名**按另一半 `enum UvcppError`
 * 的约定去掉 `uvcpp_c_` 前缀（`UvcppH2StreamState` / `UvcppQuicState`），成员名
 * 仍与头里逐条对齐。
 *
 * 只用 `System.Runtime.InteropServices`：没有 `unsafe`、没有 `NativeLibrary` /
 * `LibraryImport` / 源生成器。
 *
 * 编译门槛是**量出来的**（本机 `dotnet` 8.0.404，两份文件一起编）：
 *
 *   - `net8.0`：0 警告 0 错误 —— `examples/QuicEcho` 就是它，而且**真跑过**；
 *   - `netstandard2.1` + `<LangVersion>10</LangVersion>`：0 警告 0 错误（只编
 *     过，手上没有那个框架的运行时去跑）。
 *
 * 编不过的两档各只有**一个**原因，都不是"写法不兼容"而是缺件：
 *
 *   - `netstandard2.0`：参考程序集里 `UnmanagedType` 没有 `LPUTF8Str`（报错的
 *     98 处全是它）。UTF-8 那条路要么这个枚举值、要么手写 `Marshal` 拷贝；
 *   - C# 9 及更早：文件范围的命名空间（`namespace Uvcpp;`）要 C# 10。
 *
 * 所以**不要**说"Unity 也编得过" —— Unity 2022 还是 C# 9。真要在那上面用，办法
 * 是把两处 `namespace Uvcpp;` 改成块形式（要重新缩进整个文件），那一档**没验过**。
 */

using System;
using System.Runtime.InteropServices;

/* --------------------------------------------------------------------------
 * 不透明句柄的别名
 * --------------------------------------------------------------------------
 * 头里的 `typedef struct uvcpp_c_xxx uvcpp_c_xxx;` 永远是不完整类型 —— C# 侧
 * 一律照抄成不透明的 `IntPtr`。**`using` 别名是文件作用域**：本文件与另一半个
 * `UvcppNative.cs` 会各自声明自己用到的那几个（名字都取 C 类型名），互不冲突。
 * -------------------------------------------------------------------------- */
using uvcpp_c_h2_connection = System.IntPtr;
using uvcpp_c_h2_stream = System.IntPtr;
using uvcpp_c_h2_request = System.IntPtr;
using uvcpp_c_h2_response = System.IntPtr;
using uvcpp_c_quic_client = System.IntPtr;
using uvcpp_c_quic_server = System.IntPtr;
using uvcpp_c_quic_tls = System.IntPtr;
using uvcpp_c_quic_connection = System.IntPtr;
using uvcpp_c_h3_connection = System.IntPtr;
using uvcpp_c_h3_request = System.IntPtr;
using uvcpp_c_h3_response = System.IntPtr;
using uvcpp_c_h3_request_view = System.IntPtr;
/* net 层的句柄：http2 的 `uvcpp_c_h2_connection_new()` 收一条既有的 TCP 连接，
 * 所以本文件也要这个别名（它归另一半声明的那份互相独立）。 */
using uvcpp_c_tcp_client = System.IntPtr;

namespace Uvcpp;

public static partial class UvcppNative
{
    /* ======================================================================
     * http2 片（uvcpp_c_http2.h）
     * ====================================================================== */

    /// <summary>`uvcpp_c_h2_stream_state()` 的取值，与 C++ 的 `h2_stream_state` 逐条对应。
    /// 类型名去掉 `uvcpp_c_` 前缀（见文件头），成员名与头逐条对齐。</summary>
    public enum UvcppH2StreamState
    {
        /// <summary>请求头已收全，业务处理中（可能正在收 body）。</summary>
        UVCPP_C_H2_OPEN = 0,
        /// <summary>流式响应：头部已发，body 还要一块块补。</summary>
        UVCPP_C_H2_HEADERS_SENT = 1,
        /// <summary>响应已一次提交完。</summary>
        UVCPP_C_H2_SENT = 2,
        /// <summary>两端都已结束。当前从不出现。</summary>
        UVCPP_C_H2_CLOSED = 3,
        /// <summary>协议层拒了它。当前从不出现。</summary>
        UVCPP_C_H2_REJECTED = 4,
    }

    /// <summary>收到请求头（服务端）或响应头（客户端）；`end_stream` 非 0 表示没有 body。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_h2_on_request_cb(IntPtr user_data, uvcpp_c_h2_connection c, uvcpp_c_h2_stream st, int end_stream);

    /// <summary>请求收完（没有它就分不出"头到了"和"请求全到了"）。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_h2_on_request_end_cb(IntPtr user_data, uvcpp_c_h2_connection c, uvcpp_c_h2_stream st);

    /// <summary>收到响应头（客户端侧）；`end_stream` 语义同上。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_h2_on_response_cb(IntPtr user_data, uvcpp_c_h2_connection c, uvcpp_c_h2_stream st, int end_stream);

    /// <summary>响应收完（客户端侧），与 `on_request_end` 对称。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_h2_on_response_end_cb(IntPtr user_data, uvcpp_c_h2_connection c, uvcpp_c_h2_stream st);

    /// <summary>收到一段 body（服务端是请求体、客户端是响应体）。</summary>
    /// <remarks>`data` 只在那次回调里有效，要留就当场拷走（规矩 4 第二类）。</remarks>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_h2_on_body_cb(IntPtr user_data, uvcpp_c_h2_connection c, uvcpp_c_h2_stream st, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] data, nuint len);

    /// <summary>流结束（两端的 END_STREAM 都发完，或收到 RST_STREAM）；递的是 stream_id 而不是句柄。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_h2_on_close_cb(IntPtr user_data, uvcpp_c_h2_connection c, int stream_id, uint error_code);

    /// <summary>连接级致命错误；收到它就只有一个正确动作：发 GOAWAY（能发就发）然后关连接。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_h2_on_fatal_cb(IntPtr user_data, uvcpp_c_h2_connection c, int nghttp2_error);

    /// <summary>底层连接结束了（对端关、读错、或我们关完了）。</summary>
    /// <remarks>回调返回后不要再用这个句柄 —— 按契约应当在这里 `uvcpp_c_h2_conn_free()`。</remarks>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_h2_on_disconnect_cb(IntPtr user_data, uvcpp_c_h2_connection c);

    /// <summary>`uvcpp_c_h2_conn_send_data()` 的 `done`：那一块上线后回调一次，参数是 `UV_ECANCELED`（流被 RST 或连接断了）。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_h2_send_data_done_cb(IntPtr user_data, int status);

    /// <summary>h2 协议层的回调表（以 `uint32_t size` 打头）。</summary>
    /// <remarks>`size` 必须最先填（`(uint)Marshal.SizeOf&lt;T&gt;()`）；本层逐字段检查你覆盖到哪一格，没覆盖的当"不关心"，永远不会被调。全部在循环线程上调用。</remarks>
    [StructLayout(LayoutKind.Sequential)]
    public struct uvcpp_c_h2_callbacks
    {
        public uint size;
        public uvcpp_c_h2_on_request_cb on_request;
        public uvcpp_c_h2_on_request_end_cb on_request_end;
        public uvcpp_c_h2_on_response_cb on_response;
        public uvcpp_c_h2_on_response_end_cb on_response_end;
        public uvcpp_c_h2_on_body_cb on_body;
        public uvcpp_c_h2_on_close_cb on_close;
        public uvcpp_c_h2_on_fatal_cb on_fatal;
    }

    /// <summary>h2 连接层的回调表（以 `uint32_t size` 打头，规则同上）。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct uvcpp_c_h2_conn_callbacks
    {
        public uint size;
        public uvcpp_c_h2_on_disconnect_cb on_disconnect;
    }

    /// <summary>在一条已有的 TCP 连接上建一层 h2 驱动。</summary>
    /// <remarks>`client` 由调用方所有、本层不拥有（必须比本句柄活得久）；失败返回 NULL，失败原因不在这里报。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_connection_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_h2_connection uvcpp_c_h2_connection_new(uvcpp_c_tcp_client client, int server_side);

    /// <summary>释放句柄（只做 C++ 对象的析构，不会替你对端断连、也不发 GOAWAY）。</summary>
    /// <remarks>要优雅收尾先 `_shutdown()`；已 free 过或传空分别得到 `E_STALE` / `E_INVALID_ARG`，用户给的回调一个都不会被调。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_free(uvcpp_c_h2_connection c);

    /// <summary>建会话、提交初始 SETTINGS、起读、把首轮字节发出去。</summary>
    /// <remarks>只能在循环线程调用；表按 `size` 逐格读（`size` 连 4 都没盖住一律 `E_INVALID_ARG`）。C 面允许传 NULL 表，C# 侧以 `ref` 传、无法表达 NULL。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_start", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_start(uvcpp_c_h2_connection c, ref uvcpp_c_h2_callbacks h2_cbs, ref uvcpp_c_h2_conn_callbacks conn_cbs, IntPtr user_data);

    /// <summary>提交一个最小响应（只有 `:status` 与 `content-length`）并立刻冲出去（`submit_status` + `flush` 的合并版）。</summary>
    /// <returns>0 成功；负值见 `uvcpp_c_strerror()` 与 libuv 错误码。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_send_status", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_send_status(uvcpp_c_h2_connection c, int stream_id, int status, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 4)] byte[] body, nuint body_len);

    /// <summary>提交一个完整响应并立刻冲出去；`omit_body` 非 0 = 只发头部。</summary>
    /// <remarks>`resp` 交出去时立刻拷贝，返回后可继续改或 free；`UV_EINVAL` / `UV_EMSGSIZE` 是可重试的。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_send_response", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_send_response(uvcpp_c_h2_connection c, int stream_id, uvcpp_c_h2_response resp, int omit_body);

    /// <summary>提交流式响应的头部（不结束流）+ 冲一次；之后用 `_send_data()` 一块块补 body。</summary>
    /// <remarks>顺序反了（先 `send_response` 再 `send_headers`）会拿到 `UV_EALREADY`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_send_headers", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_send_headers(uvcpp_c_h2_connection c, int stream_id, uvcpp_c_h2_response resp);

    /// <summary>提交一块流式 body + 冲一次；`end_stream` 非 0 表示本块之后收尾。</summary>
    /// <remarks>`done` 在那块上线后回调一次（绝不在本次调用里同步跑），可传 NULL；返回只表示"受没受理"，非 0 时 `done` 不会被调，调用方自己收尾。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_send_data", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_send_data(uvcpp_c_h2_connection c, int stream_id, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] data, nuint len, int end_stream, uvcpp_c_h2_send_data_done_cb done, IntPtr done_user_data);

    /// <summary>提交一个请求（转发给 session 的 `submit_request`）并顺手冲一次。</summary>
    /// <returns>成功时是流号（正数），失败时是负的错误码（冲失败时返回冲的错误码，那时流已经建了）。</returns>
    /// <remarks>`req` / `body` 交出去时立刻拷贝；`req` 里必须带 `host` 头（本层用它填 `:authority`，没有兜底）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_submit_request", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_submit_request(uvcpp_c_h2_connection c, uvcpp_c_h2_request req, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] body, nuint body_len);

    /// <summary>暂停一条流的收方向（转发给 session 的 `pause_stream`），不 flush。</summary>
    /// <remarks>幂等；对不存在或已关闭的流返回 `UV_EINVAL`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_pause_stream", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_pause_stream(uvcpp_c_h2_connection c, int stream_id);

    /// <summary>恢复一条流的收方向（转发给 session 的 `resume_stream`）+ flush。</summary>
    /// <remarks>必须用这一个：只排 WINDOW_UPDATE 不冲的话对端会永远等不到窗口，而且不报任何错。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_resume_stream", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_resume_stream(uvcpp_c_h2_connection c, int stream_id);

    /// <summary>给一条流发 RST_STREAM（不自动 flush，要发出去紧接着 `_flush()`）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_submit_rst", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_submit_rst(uvcpp_c_h2_connection c, int stream_id, uint error_code);

    /// <summary>把会话里待发的字节全部写出去；可以重复调（没东西发就是空操作）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_flush", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_flush(uvcpp_c_h2_connection c);

    /// <summary>只发 GOAWAY、不关连接：不再接新流，已有的流照跑完；发过一次就不再发。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_begin_goaway", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_begin_goaway(uvcpp_c_h2_connection c);

    /// <summary>直接提交一条 GOAWAY（转发给 session 的 `submit_goaway`，自己指定错误码）；不自动 flush。</summary>
    /// <remarks>`debug` / `debug_len` 是 GOAWAY 的调试正文，可空。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_submit_goaway", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_submit_goaway(uvcpp_c_h2_connection c, uint error_code, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] debug, nuint debug_len);

    /// <summary>主动关：尽量把待发字节（含 GOAWAY）冲出去，再关底层连接。</summary>
    /// <remarks>返回 0 只表示"我受理了"；真正的结束是之后 `on_disconnect` 响的时候。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_shutdown", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_shutdown(uvcpp_c_h2_connection c);

    /// <summary>立刻关，不发任何东西；返回 0 同样不是"已经关完"。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_close_now", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_close_now(uvcpp_c_h2_connection c);

    /// <summary>底层连接已经结束（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_closed", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_closed(uvcpp_c_h2_connection c);

    /// <summary>正在关（等最后一笔写出去）（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_closing", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_closing(uvcpp_c_h2_connection c);

    /// <summary>当前活跃流数。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_stream_count", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern nuint uvcpp_c_h2_conn_stream_count(uvcpp_c_h2_connection c);

    /// <summary>最近一次致命错误的原始码（0 表示还没有）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_last_error", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_last_error(uvcpp_c_h2_connection c);

    /// <summary>输入的字节数（供测试与诊断用）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_bytes_in", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern nuint uvcpp_c_h2_conn_bytes_in(uvcpp_c_h2_connection c);

    /// <summary>输出的字节数（供测试与诊断用）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_bytes_out", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern nuint uvcpp_c_h2_conn_bytes_out(uvcpp_c_h2_connection c);

    /// <summary>收到过对端的 GOAWAY 吗（1/0）；"没收到"与"收到的是 NO_ERROR"是两件事。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_peer_goaway_received", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_peer_goaway_received(uvcpp_c_h2_connection c);

    /// <summary>对端 GOAWAY 里的错误码；没收到过时是 0（NO_ERROR）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_peer_goaway_error_code", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uint uvcpp_c_h2_conn_peer_goaway_error_code(uvcpp_c_h2_connection c);

    /// <summary>对端 GOAWAY 里的 `last_stream_id`；没收到过时是 0。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_conn_peer_goaway_last_stream_id", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_conn_peer_goaway_last_stream_id(uvcpp_c_h2_connection c);

    /// <summary>流号。</summary>
    /// <remarks>回调期句柄：拿到已经出了回调的那个流句柄再问，一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_stream_id", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_stream_id(uvcpp_c_h2_stream st);

    /// <summary>当前状态，取值见 `UvcppH2StreamState`。</summary>
    /// <remarks>回调期句柄，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_stream_state", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_stream_state(uvcpp_c_h2_stream st);

    /// <summary>协议层拒过这条流吗（1/0）；正常路径上恒为 0。</summary>
    /// <remarks>回调期句柄，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_stream_rejected", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_stream_rejected(uvcpp_c_h2_stream st);

    /// <summary>这条流的收方向被 `_pause_stream()` 暂停了吗（1/0）。</summary>
    /// <remarks>回调期句柄，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_stream_paused", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_stream_paused(uvcpp_c_h2_stream st);

    /// <summary>已经收到的 body 字节数（按流记）。</summary>
    /// <remarks>回调期句柄，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_stream_body_bytes", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern nuint uvcpp_c_h2_stream_body_bytes(uvcpp_c_h2_stream st);

    /// <summary>对端宣告的 `content-length`（只在服务端那一侧有意义），非空时写出那个数。</summary>
    /// <returns>对端给了 `content-length` → 0；没给 → `E_NOT_FOUND`（"没给"与"给了 0"是两件事）。</returns>
    /// <remarks>客户端侧这一句永远拿到 `E_NOT_FOUND`；客户端要读响应体的 `content-length` 请用 `_response_header()`。回调期句柄，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_stream_expected_body", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_stream_expected_body(uvcpp_c_h2_stream st, out nuint @out);

    /// <summary>请求的方法名；调用方给缓冲区（约定见 `uvcpp_c_common.h`）。</summary>
    /// <remarks>回调期句柄，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_stream_request_method_name", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_stream_request_method_name(uvcpp_c_h2_stream st, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>请求的 URL（h2 侧是 `:path`）；调用方给缓冲区，规则同上。</summary>
    /// <remarks>回调期句柄，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_stream_request_path", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_stream_request_path(uvcpp_c_h2_stream st, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>取一个请求头（名字大小写不敏感）；查不到返回 `E_NOT_FOUND`。</summary>
    /// <remarks>回调期句柄，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_stream_request_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_stream_request_header(uvcpp_c_h2_stream st, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] buf, nuint cap);

    /// <summary>请求里有这个头吗（1/0）；空值也算"有"。</summary>
    /// <remarks>回调期句柄，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_stream_request_has_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_stream_request_has_header(uvcpp_c_h2_stream st, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    /// <summary>对端发来的响应的状态码（客户端侧）。</summary>
    /// <returns>状态码；对端的响应头还没到时返回 `E_NOT_FOUND`（不是 0，也不是默认的 200）。</returns>
    /// <remarks>回调期句柄，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_stream_response_status", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_stream_response_status(uvcpp_c_h2_stream st);

    /// <summary>取响应头（客户端侧，名字大小写不敏感）；查不到返回 `E_NOT_FOUND`。</summary>
    /// <remarks>回调期句柄，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_stream_response_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_stream_response_header(uvcpp_c_h2_stream st, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] buf, nuint cap);

    /// <summary>建一个请求构造器；失败返回 NULL。</summary>
    /// <remarks>调用方建、调用方废；每个 `_set_*` 都是覆盖语义。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_request_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_h2_request uvcpp_c_h2_request_new([MarshalAs(UnmanagedType.LPUTF8Str)] string method, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);

    /// <summary>废掉请求构造器；已废/传空分别得到 `E_STALE` / `E_INVALID_ARG`。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_request_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_request_free(uvcpp_c_h2_request req);

    /// <summary>设一个头（同名覆盖，大小写不敏感）；`host` 必须设（见 `_submit_request()`）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_request_set_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_request_set_header(uvcpp_c_h2_request req, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPUTF8Str)] string value);

    /// <summary>建一个响应构造器；失败返回 NULL。</summary>
    /// <remarks>调用方建、调用方废；每个 `_set_*` 都是覆盖语义。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_response_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_h2_response uvcpp_c_h2_response_new(int status);

    /// <summary>废掉响应构造器。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_response_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_response_free(uvcpp_c_h2_response resp);

    /// <summary>改状态码。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_response_set_status", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_response_set_status(uvcpp_c_h2_response resp, int status);

    /// <summary>设一个头（同名覆盖）；h2 里不能出现的连接专属头会在提交时由内层拒掉。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_response_set_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_response_set_header(uvcpp_c_h2_response resp, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPUTF8Str)] string value);

    /// <summary>设/覆盖 `content-type`（等价于设同名头）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_response_set_content_type", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_response_set_content_type(uvcpp_c_h2_response resp, [MarshalAs(UnmanagedType.LPUTF8Str)] string content_type);

    /// <summary>设/覆盖响应体。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h2_response_set_body", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h2_response_set_body(uvcpp_c_h2_response resp, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] data, nuint len);

    /* ======================================================================
     * quic 片（uvcpp_c_quic.h）
     * ====================================================================== */

    /// <summary>`uvcpp_c_quic_conn_state()` 的取值，与 C++ 的 `quic_connection_state` 逐条对应。
    /// 类型名去掉 `uvcpp_c_` 前缀（见文件头），成员名与头逐条对齐。</summary>
    public enum UvcppQuicState
    {
        /// <summary>还没有对端。</summary>
        UVCPP_C_QUIC_IDLE = 0,
        /// <summary>握手进行中。</summary>
        UVCPP_C_QUIC_HANDSHAKING = 1,
        /// <summary>握手完成，可以收发应用数据。</summary>
        UVCPP_C_QUIC_ESTABLISHED = 2,
        /// <summary>已发 CONNECTION_CLOSE，等对端确认。</summary>
        UVCPP_C_QUIC_CLOSING = 3,
        /// <summary>收到对端的 CONNECTION_CLOSE，等计时器到期。</summary>
        UVCPP_C_QUIC_DRAINING = 4,
        /// <summary>已终结，对象可以销毁。</summary>
        UVCPP_C_QUIC_CLOSED = 5,
    }

    /// <summary>收到一条流上的数据（带 stream_id —— QUIC 与 TCP 的本质差别：一条连接上并行跑着很多条流）。</summary>
    /// <remarks>形状与 net 那层同一个结构体（`uvcpp_c_read_result`）；`result.data` 只在那次回调里有效，要留就当场拷走。</remarks>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_quic_on_read_cb(IntPtr user_data, uvcpp_c_quic_connection c, long stream_id, ref uvcpp_c_read_result result);

    /// <summary>对端放开了流额度：`bidi` 非 0 是双向额度，否则单向；`max_streams` 是总额。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_quic_on_streams_available_cb(IntPtr user_data, uvcpp_c_quic_connection c, int bidi, ulong max_streams);

    /// <summary>对端对一条流发了 STOP_SENDING（读方向被掐掉），带它的应用错误码。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_quic_on_stop_sending_cb(IntPtr user_data, uvcpp_c_quic_connection c, long stream_id, ulong app_error_code);

    /// <summary>流被建出来（本端或对端）。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_quic_on_stream_open_cb(IntPtr user_data, uvcpp_c_quic_connection c, long stream_id);

    /// <summary>一次 `_write_stream()` 的结果（一次调用一次回调）；`status` 0 成功，非 0 是失败码。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_quic_on_write_cb(IntPtr user_data, uvcpp_c_quic_connection c, long stream_id, int status);

    /// <summary>握手协商出的 ALPN（只在这次回调里有效）；h3 那三条关键流要在这一条之后才开得出来。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_quic_on_alpn_cb(IntPtr user_data, uvcpp_c_quic_connection c, [MarshalAs(UnmanagedType.LPUTF8Str)] string alpn);

    /// <summary>连接结束了（对端关、超时、或本端关完），恰好一次。</summary>
    /// <remarks>`error_code` 按符号分两种意思：大于 0 是要发给对端的应用错误码，小于 0 是本端的失败码。回调返回后不要再碰这条连接上的任何句柄（那时已被毒化，再用是 `E_STALE`）。</remarks>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_quic_on_close_cb(IntPtr user_data, uvcpp_c_quic_connection c, int error_code);

    /// <summary>`uvcpp_c_quic_server_listen()` 的连接回调：每收到一条新连接调一次。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_quic_server_listen_cb(IntPtr user_data, uvcpp_c_quic_connection c);

    /// <summary>QUIC 连接的回调表（以 `uint32_t size` 打头，规则见 `uvcpp_c_common.h` 规矩 3）。</summary>
    /// <remarks>`size` 必须最先填；七条全部在循环线程上调用；没覆盖的格子当"不关心"。</remarks>
    [StructLayout(LayoutKind.Sequential)]
    public struct uvcpp_c_quic_callbacks
    {
        public uint size;
        public uvcpp_c_quic_on_read_cb on_read;
        public uvcpp_c_quic_on_streams_available_cb on_streams_available;
        public uvcpp_c_quic_on_stop_sending_cb on_stop_sending;
        public uvcpp_c_quic_on_stream_open_cb on_stream_open;
        public uvcpp_c_quic_on_write_cb on_write;
        public uvcpp_c_quic_on_alpn_cb on_alpn;
        public uvcpp_c_quic_on_close_cb on_close;
    }

    /// <summary>初始化 ngtcp2 的 OpenSSL crypto 后端；进程内调一次，在任何客户端 / 服务端之前。</summary>
    /// <returns>0 成功；非 0 只可能来自将来更严的版本。</returns>
    /// <remarks>不要调两次（上游不查重）；要重来先 `_free()`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_crypto_init", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_crypto_init();

    /// <summary>释放上面那一批 EVP 对象；可重复调用（本层不在静态析构里替你调）。</summary>
    /// <returns>`UVCPP_C_OK`（C++ 那侧是 void，本层为统一形状返回 int）。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_crypto_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_crypto_free();

    /// <summary>编进来的 ngtcp2 是哪个版本；调用方给缓冲区（约定见 `uvcpp_c_common.h`）。</summary>
    /// <remarks>它证明 ngtcp2 的头路径到得了，不证明链上了（那要用 `_error_string()`）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_ngtcp2_version", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_ngtcp2_version([MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] byte[] buf, nuint cap);

    /// <summary>把 ngtcp2 的错误码翻成它自己的文本（转发上游 `ngtcp2_strerror()`）。</summary>
    /// <remarks>`code = 0` 得到 `"NO_ERROR"`；认不出的码得到上游原文 `"(unknown)"`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_error_string", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_error_string(int code, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>建一个服务端 TLS 上下文，装一段证书链与它的私钥；失败（打不开 / 格式不对 / 不配对）返回 NULL。</summary>
    /// <remarks>调用方建、调用方废；端点不拥有它，生命周期要盖住用到它的每一条连接。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_tls_server_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_quic_tls uvcpp_c_quic_tls_server_new([MarshalAs(UnmanagedType.LPUTF8Str)] string cert_file, [MarshalAs(UnmanagedType.LPUTF8Str)] string key_file);

    /// <summary>建一个服务端 TLS 上下文，现场生成一张自签证书（给测试与本地验证用）；失败返回 NULL。</summary>
    /// <remarks>`common_name` 为空时按 `"localhost"` 走；生产环境请用 `_server_new()`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_tls_server_selfsigned", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_quic_tls uvcpp_c_quic_tls_server_selfsigned([MarshalAs(UnmanagedType.LPUTF8Str)] string common_name);

    /// <summary>建一个客户端 TLS 上下文；默认不校验对端证书。</summary>
    /// <remarks>`ca_file` 可为 NULL（那就是"不校验"，也是默认）；给了它还要 `_set_verify(tls, 1)` 才开始校验。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_tls_client_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_quic_tls uvcpp_c_quic_tls_client_new([MarshalAs(UnmanagedType.LPUTF8Str)] string ca_file);

    /// <summary>废掉 TLS 配置句柄；已废 / 传空分别得到 `E_STALE` / `E_INVALID_ARG`。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_tls_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_tls_free(uvcpp_c_quic_tls tls);

    /// <summary>装信任库（PEM）。</summary>
    /// <returns>0 成功；文件打不开或格式不对返回负值。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_tls_set_ca_file", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_tls_set_ca_file(uvcpp_c_quic_tls tls, [MarshalAs(UnmanagedType.LPUTF8Str)] string ca_file);

    /// <summary>开 / 关对端证书校验；`mode` 非 0 = 校验（PEER），0 = 不校验（NONE）。</summary>
    /// <remarks>主机的名字不在这条路上（本层不提供 PEER_STRICT 那一档）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_tls_set_verify", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_tls_set_verify(uvcpp_c_quic_tls tls, int mode);

    /// <summary>建一个客户端（自带一条循环）；失败返回 NULL。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_client_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_quic_client uvcpp_c_quic_client_new();

    /// <summary>释放客户端（顺手把借出去的连接句柄收回来）。</summary>
    /// <remarks>在回调里调它得到 `E_STATE`；已废 / 传空分别得到 `E_STALE` / `E_INVALID_ARG`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_client_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_client_free(uvcpp_c_quic_client client);

    /// <summary>装 TLS 上下文；不设就是没配（那时 `_connect()` 会失败）。</summary>
    /// <remarks>`tls` 生命周期必须盖住每一条连接，端点不拥有它；清空传 NULL。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_client_set_tls", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_client_set_tls(uvcpp_c_quic_client client, uvcpp_c_quic_tls tls);

    /// <summary>宣告本端支持的 ALPN 协议名，顺序即优先级。</summary>
    /// <remarks>`protos` 的每个元素是指向 NUL 结尾 UTF-8 C 串的指针（缓冲区归调用方所有）；`protos` 为 NULL 或 `proto_count` 为 0 都表示恢复默认（`"h3"`）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_client_set_alpn_protos", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_client_set_alpn_protos(uvcpp_c_quic_client client, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] IntPtr[] protos, nuint proto_count);

    /// <summary>空闲超时（毫秒），默认 30000；0 = 不设超时。</summary>
    /// <remarks>只在 `_connect()` 之前调有效（握手一开始，传输参数就发出去了）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_client_set_idle_timeout", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_client_set_idle_timeout(uvcpp_c_quic_client client, ulong ms);

    /// <summary>发起连接；握手完成时回调一次（`status == 0` 表示成功，非 0 是失败码）。</summary>
    /// <remarks>只能在循环线程调用，回调也在循环线程；`cb` 必须给；`user_data` 原样传回。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_client_connect", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_client_connect(uvcpp_c_quic_client client, [MarshalAs(UnmanagedType.LPUTF8Str)] string host, int port, uvcpp_c_status_cb cb, IntPtr user_data);

    /// <summary>取那条连接句柄（借来的，永远返回同一个指针，可以存下来）。</summary>
    /// <returns>句柄；客户端本身无效时返回 NULL。</returns>
    /// <remarks>还没连上时用它做任何事都得到 `E_STATE`（句柄是活的，只是底下还没有连接）；连接关掉之后得到 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_client_connection", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_quic_connection uvcpp_c_quic_client_connection(uvcpp_c_quic_client client);

    /// <summary>关客户端（连带那条连接），关闭是优雅的。</summary>
    /// <returns>0 = 关掉了（或本来就没东西可关）；`UV_ENOTCONN` = 没连过。</returns>
    /// <remarks>要给对端一个非 0 的应用错误码，用 `uvcpp_c_quic_conn_close(conn, code)`（那是另一条路）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_client_close", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_client_close(uvcpp_c_quic_client client);

    /// <summary>泵客户端的循环，阻塞直到它自然停（没有活句柄了）或有人 `_stop()`。</summary>
    /// <remarks>不要在两个线程上同时泵同一个端点的循环（libuv 的规矩，不是本层的）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_client_run", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_client_run(uvcpp_c_quic_client client);

    /// <summary>只泵一轮（`UV_RUN_NOWAIT`）就返回，不阻塞。</summary>
    /// <returns>`uv_run()` 的返回值：非 0 = 这一轮里还有活句柄。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_client_run_once", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_client_run_once(uvcpp_c_quic_client client);

    /// <summary>停循环，让 `_run()` 返回。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_client_stop", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_client_stop(uvcpp_c_quic_client client);

    /// <summary>建一个服务端（自带一条循环）；失败返回 NULL。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_server_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_quic_server uvcpp_c_quic_server_new();

    /// <summary>释放服务端（顺手把还在手上的借用句柄全部收回来）。</summary>
    /// <remarks>已废 / 传空分别得到 `E_STALE` / `E_INVALID_ARG`；在回调里调它得到 `E_STATE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_server_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_server_free(uvcpp_c_quic_server server);

    /// <summary>装 TLS 上下文；不设就是没配，那时 `listen()` 直接失败（QUIC 没有明文模式）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_server_set_tls", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_server_set_tls(uvcpp_c_quic_server server, uvcpp_c_quic_tls tls);

    /// <summary>给出愿意接受的 ALPN 协议名并装选择回调（顺序即优先级）。</summary>
    /// <remarks>参数与"NULL / 0 = 恢复默认"同客户端那一枚；挑不中的后果是握手仍然成功（ALPN 交回空串）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_server_set_alpn_protos", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_server_set_alpn_protos(uvcpp_c_quic_server server, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] IntPtr[] protos, nuint proto_count);

    /// <summary>空闲超时（毫秒），默认 30000；0 = 不设超时。</summary>
    /// <remarks>它是服务端收掉"对端已经走了但没告别"的连接唯一的手段；只对它 `_listen()` 之后建出来的连接有效。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_server_set_idle_timeout", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_server_set_idle_timeout(uvcpp_c_quic_server server, ulong ms);

    /// <summary>登记要绑的地址与端口（不创建、不绑任何 socket）。</summary>
    /// <returns>0 成功；`UV_EINVAL` 地址或端口不合法（地址经 `uv_inet_pton()` 真校验）。</returns>
    /// <remarks>`ip` 为 NULL = 通配 IPv4；`port` 取 0 到 65535，0 = 由内核挑一个（挑中的在 `_listen()` 之后由 `_configured_port()` 报出）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_server_bind", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_server_bind(uvcpp_c_quic_server server, [MarshalAs(UnmanagedType.LPUTF8Str)] string ip, int port);

    /// <summary>端口：`_listen()` 之前报的是登记值，之后报的是内核实际给的那个（没登记过就是 0）。</summary>
    /// <remarks>名字不叫 `local_port()` 是因为端口被占用要到 `_listen()` 才暴露。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_server_configured_port", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_server_configured_port(uvcpp_c_quic_server server);

    /// <summary>登记的地址字面量（调用方给缓冲区，约定见 `uvcpp_c_common.h`）；没登记过时长度为 0。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_server_configured_ip", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_server_configured_ip(uvcpp_c_quic_server server, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>开始接收连接；每收到一条新连接回调一次。</summary>
    /// <returns>0 = 已经在收；负值 = 没起来（返回负值时 `cb` 一次都不会被调）。</returns>
    /// <remarks>`cb` 在连接刚建出来（首包到达）时跑，不在握手完成时跑；它给出的连接句柄是借来的。`on_alpn` / `on_read` 要在它里面用 `_conn_set_callbacks()` 装上（都排在它之后）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_server_listen", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_server_listen(uvcpp_c_quic_server server, uvcpp_c_quic_server_listen_cb cb, IntPtr user_data);

    /// <summary>跑循环，这个调用会阻塞，直到 `_stop()`。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_server_run", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_server_run(uvcpp_c_quic_server server);

    /// <summary>只泵一轮（`UV_RUN_NOWAIT`）就返回；语义与客户端那一枚逐个对应。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_server_run_once", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_server_run_once(uvcpp_c_quic_server server);

    /// <summary>停循环；不关已有的连接（要收尾就逐个 `_conn_close()`，或者释放服务端）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_server_stop", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_server_stop(uvcpp_c_quic_server server);

    /// <summary>装回调（以 `uint32_t size` 打头，逐格读）；可以在连接活着的任何时刻重装（覆盖）。</summary>
    /// <remarks>底下还没有连接时调它也不是错误（先记表、再 `_connect()` 是预期写法）。装了 h3 之后不要再调它（否则是把 h3 的驱动拆掉一半）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_conn_set_callbacks", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_conn_set_callbacks(uvcpp_c_quic_connection c, ref uvcpp_c_quic_callbacks cbs, IntPtr user_data);

    /// <summary>连接状态，取值见 `UvcppQuicState`。</summary>
    /// <remarks>底下还没有连接时返回 `UVCPP_C_QUIC_IDLE`（那不是错误码）；句柄本身已经废了才返回 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_conn_state", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_conn_state(uvcpp_c_quic_connection c);

    /// <summary>协商出的 ALPN（调用方给缓冲区）；握手完成前长度为 0。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_conn_alpn_selected", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_conn_alpn_selected(uvcpp_c_quic_connection c, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>开一条流；`bidi` 非 0 = 双向流，0 = 单向流（只发不收）。</summary>
    /// <returns>成功返回流号（非负，可为负也可为 0）；失败返回负的错误码（`NGTCP2_ERR_STREAM_ID_BLOCKED` 是其中一种正常结果）。</returns>
    /// <remarks>判断成功要看小于 0 是失败，而不是大于 0 是成功（QUIC 规范里服务端发起的流号是负数）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_conn_open_stream", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern long uvcpp_c_quic_conn_open_stream(uvcpp_c_quic_connection c, int bidi);

    /// <summary>往一条流里写；`end_stream` 非 0 = 这块之后本端的写方向就结束了（FIN）。</summary>
    /// <returns>0 = 已受理（一次 `on_write` 会来）；非 0 = 失败码（那时 `on_write` 不会来）。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_conn_write_stream", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_conn_write_stream(uvcpp_c_quic_connection c, long stream_id, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] data, nuint len, int end_stream);

    /// <summary>重置一条流的写方向（发 RESET_STREAM），带上应用错误码。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_conn_shutdown_stream", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_conn_shutdown_stream(uvcpp_c_quic_connection c, long stream_id, ulong app_error_code);

    /// <summary>掐掉一条流的读方向（发 STOP_SENDING）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_conn_shutdown_stream_read", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_conn_shutdown_stream_read(uvcpp_c_quic_connection c, long stream_id, ulong app_error_code);

    /// <summary>还剩多少条流可以开（`bidi` 非 0 问双向）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_conn_streams_left", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern ulong uvcpp_c_quic_conn_streams_left(uvcpp_c_quic_connection c, int bidi);

    /// <summary>关掉这条连接（把 `error_code` 发给对端）；0 = 干净关闭。</summary>
    /// <returns>0 = 已受理；真正的结束不是这里返回的时候 —— `on_close` 之后才算。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_quic_conn_close", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_quic_conn_close(uvcpp_c_quic_connection c, int error_code);

    /* ======================================================================
     * http3 片（uvcpp_c_http3.h）
     * ====================================================================== */

    /// <summary>一条流关闭时，两个方向各自是怎么收场的（`h3_stream_close_info` 的平整版：bool 在这里是 int，取值 0/1）。</summary>
    /// <remarks>两个方向分开报是必须的（对端还发不发 vs 我还发不发）。`size` 是形状自检，不是回调表那一条规矩。</remarks>
    [StructLayout(LayoutKind.Sequential)]
    public struct uvcpp_c_h3_stream_close_info
    {
        public uint size;
        public long stream_id;
        public int rx_error;
        public int tx_error;
        public ulong rx_app_error_code;
        public ulong tx_app_error_code;
    }

    /// <summary>收到一条完整的请求（服务端侧才有意义）；`req` 是回调期视图，只在这次回调里有效，要留就当场拷走。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_h3_on_request_cb(IntPtr user_data, uvcpp_c_h3_connection h, uvcpp_c_h3_request_view req);

    /// <summary>一条流关掉了（两个方向都收场之后）；客户端侧这条流的响应此刻已经在完成队列里了。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_h3_on_stream_close_cb(IntPtr user_data, uvcpp_c_h3_connection h, ref uvcpp_c_h3_stream_close_info info);

    /// <summary>连接级致命错误（本层已经没法在这条连接上继续了）；本层收到之后会自己把 QUIC 连接关掉。</summary>
    /// <remarks>`error_code` 按符号分两种意思：大于 0 是要发给对端的 h3 应用错误码，小于 0 是本端的失败码；随后一定会收到一次 `on_disconnect`。</remarks>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_h3_on_error_cb(IntPtr user_data, uvcpp_c_h3_connection h, int error_code);

    /// <summary>底层 QUIC 连接结束了，恰好一次。</summary>
    /// <remarks>回调里必须 `uvcpp_c_h3_connection_free(h)`；返回之后不要再碰它。</remarks>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_h3_on_disconnect_cb(IntPtr user_data, uvcpp_c_h3_connection h);

    /// <summary>h3 的回调表（以 `uint32_t size` 打头，规矩见 `uvcpp_c_common.h` 规矩 3）。</summary>
    /// <remarks>四格全部在循环线程上调用，而且很可能在某个内部调用还没返回的时候跑起来；`size` 必须最先填。</remarks>
    [StructLayout(LayoutKind.Sequential)]
    public struct uvcpp_c_h3_callbacks
    {
        public uint size;
        public uvcpp_c_h3_on_request_cb on_request;
        public uvcpp_c_h3_on_stream_close_cb on_stream_close;
        public uvcpp_c_h3_on_error_cb on_error;
        public uvcpp_c_h3_on_disconnect_cb on_disconnect;
    }

    /// <summary>编进来的 nghttp3 是哪个版本；调用方给缓冲区（约定见 `uvcpp_c_common.h`）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_version", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_version([MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] byte[] buf, nuint cap);

    /// <summary>把 h3 接到一条（借来的）QUIC 连接上；失败返回 NULL（`conn` 为空、或它底下还没有连接）。</summary>
    /// <remarks>建出来只是"拿住了"，装回调与建会话是 `_start()`；`conn` 本层不拥有。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_connection_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_h3_connection uvcpp_c_h3_connection_new(uvcpp_c_quic_connection conn, int server_side);

    /// <summary>废掉 h3 连接句柄；这是 `on_disconnect` 里必须要做的事。</summary>
    /// <remarks>已废 / 传空分别得到 `E_STALE` / `E_INVALID_ARG`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_connection_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_connection_free(uvcpp_c_h3_connection h);

    /// <summary>装回调、建会话、装 QUIC 回调，并把首轮待发字节发出去。</summary>
    /// <returns>0 成功；`UV_EINVAL` 没给 QUIC 连接；负值是 nghttp3 的错误码。</returns>
    /// <remarks>三条关键单向流不在这里开（要等 ALPN），所以 `_start()` 之后 `_conn_ready()` 还是 0 是正常的，不是失败。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_start", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_conn_start(uvcpp_c_h3_connection h, ref uvcpp_c_h3_callbacks cbs, IntPtr user_data);

    /// <summary>客户端：提交一次请求（内部开一条双向流、立刻冲一次）。</summary>
    /// <returns>成功返回流号（拿它去和 `_take_completed()` 交回来的 `_response_stream_id()` 对号）；负值是失败码。</returns>
    /// <remarks>`req` 交出去时立刻拷贝；`UV_EAGAIN` 表示关键流还没开出来（等 `_conn_ready()`），`UV_ENOTCONN` 表示连接没了，`NGTCP2_ERR_STREAM_ID_BLOCKED` 是又一种正常结果。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_send_request", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern long uvcpp_c_h3_conn_send_request(uvcpp_c_h3_connection h, uvcpp_c_h3_request req);

    /// <summary>客户端：取一条已经收全的响应（拉到 `@out` 里，一次一条）。</summary>
    /// <returns>1 = 取到了一条（`@out` 被填）；0 = 队列是空的；负值 = 错误码（句柄无效 / `@out` 为空）。</returns>
    /// <remarks>`@out` 会先被重置（连状态码一起清掉），所以同一个 `uvcpp_c_h3_response` 可以循环用。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_take_completed", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_conn_take_completed(uvcpp_c_h3_connection h, uvcpp_c_h3_response @out);

    /// <summary>队列里还积着几条（注意它会被 `_take_completed()` 掏空）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_completed_count", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_conn_completed_count(uvcpp_c_h3_connection h);

    /// <summary>服务端：提交响应并立刻冲出去；`omit_body` 非 0 = 只发头块。</summary>
    /// <returns>0 成功；`UV_EMSGSIZE` 头块超了我们自己的上限（64KB），其余是 nghttp3 的错误码。</returns>
    /// <remarks>`resp` 的流号必须已经设好（`_set_stream_id()`），否则拿到 `UV_EINVAL`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_send_response", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_conn_send_response(uvcpp_c_h3_connection h, uvcpp_c_h3_response resp, int omit_body);

    /// <summary>只发 `:status` 的最小响应 + 冲一次；`body` 可以为空。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_send_status", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_conn_send_status(uvcpp_c_h3_connection h, long stream_id, int status, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 4)] byte[] body, nuint body_len);

    /// <summary>把会话里待发的字节全部写出去；可以重复调（本层在每次收数据之后已经自己冲过一轮）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_flush", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_conn_flush(uvcpp_c_h3_connection h);

    /// <summary>关掉整条 QUIC 连接（把 `error_code` 发给对端）；0 = 干净关闭。</summary>
    /// <remarks>之后 `_conn_closed()` 会变真、`on_disconnect` 会响一次，所以循环还得再转几轮。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_close", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_conn_close(uvcpp_c_h3_connection h, int error_code);

    /// <summary>三条关键单向流都开出来并绑好了 —— 现在可以发请求 / 答复了（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_ready", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_conn_ready(uvcpp_c_h3_connection h);

    /// <summary>底层连接已经结束（`on_disconnect` 之前就为真）（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_closed", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_conn_closed(uvcpp_c_h3_connection h);

    /// <summary>本端是服务端侧（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_server_side", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_conn_server_side(uvcpp_c_h3_connection h);

    /// <summary>协商出来的 ALPN（调用方给缓冲区）；握手完成前长度为 0，握手完成但没有协议名也是 0。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_alpn_selected", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_conn_alpn_selected(uvcpp_c_h3_connection h, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>控制流的流号；还没开出来时返回 -1。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_control_stream_id", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern long uvcpp_c_h3_conn_control_stream_id(uvcpp_c_h3_connection h);

    /// <summary>QPACK 编码器流的流号；还没开出来时返回 -1。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_qpack_encoder_stream_id", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern long uvcpp_c_h3_conn_qpack_encoder_stream_id(uvcpp_c_h3_connection h);

    /// <summary>QPACK 解码器流的流号；还没开出来时返回 -1。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_qpack_decoder_stream_id", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern long uvcpp_c_h3_conn_qpack_decoder_stream_id(uvcpp_c_h3_connection h);

    /// <summary>从 QUIC 收进来的字节数（供测试与诊断用）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_bytes_in", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern ulong uvcpp_c_h3_conn_bytes_in(uvcpp_c_h3_connection h);

    /// <summary>写进 QUIC 的字节数（供测试与诊断用）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_conn_bytes_out", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern ulong uvcpp_c_h3_conn_bytes_out(uvcpp_c_h3_connection h);

    /// <summary>建一个请求构造器；`method` / `path` 都不能为空（任一为空 → 返回 NULL）。</summary>
    /// <remarks>调用方建、调用方废；`scheme` / `authority` 不在这里定（发送路径按 h3 的规矩处理）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_h3_request uvcpp_c_h3_request_new([MarshalAs(UnmanagedType.LPUTF8Str)] string method, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);

    /// <summary>废掉请求构造器；已废 / 传空分别得到 `E_STALE` / `E_INVALID_ARG`。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_request_free(uvcpp_c_h3_request req);

    /// <summary>设 `:scheme`；一般不填（默认 `"https"`）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_set_scheme", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_request_set_scheme(uvcpp_c_h3_request req, [MarshalAs(UnmanagedType.LPUTF8Str)] string scheme);

    /// <summary>设 `:authority`（一般就是 host:port）；为空 = 不发这个伪头。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_set_authority", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_request_set_authority(uvcpp_c_h3_request req, [MarshalAs(UnmanagedType.LPUTF8Str)] string authority);

    /// <summary>设一个普通头（同名覆盖，大小写不敏感）；不要往这里塞伪头（`":method"` 这类）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_set_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_request_set_header(uvcpp_c_h3_request req, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPUTF8Str)] string value);

    /// <summary>设 / 覆盖请求体（可以是二进制，`len` 说了算）；NULL / 0 = 空 body。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_set_body", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_request_set_body(uvcpp_c_h3_request req, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] data, nuint len);

    /// <summary>建一个响应构造器；`status` 为 0 表示"还没定"（客户端侧接 `_take_completed()` 交回来的东西时就是从这个值开始）。</summary>
    /// <remarks>流号没有初值（内部是 -1，不是 0：QUIC 里 0 是一条真的流）；服务端那条路要先 `_set_stream_id()`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_h3_response uvcpp_c_h3_response_new(int status);

    /// <summary>废掉响应构造器。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_response_free(uvcpp_c_h3_response resp);

    /// <summary>改状态码。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_set_status", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_response_set_status(uvcpp_c_h3_response resp, int status);

    /// <summary>指定这一条响应答复的是哪条流（服务端 `_send_response()` 之前必须调）。</summary>
    /// <remarks>流号要非负：h3 的 `send_response()` 拒收 `stream_id` 为负（那是"这条响应还没归属"的哨兵值），那时拿到 `UV_EINVAL`，不会发到 0 号流上。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_set_stream_id", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_response_set_stream_id(uvcpp_c_h3_response resp, long stream_id);

    /// <summary>设一个普通头（同名覆盖）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_set_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_response_set_header(uvcpp_c_h3_response resp, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPUTF8Str)] string value);

    /// <summary>设 / 覆盖 `content-type`（等价于设同名头）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_set_content_type", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_response_set_content_type(uvcpp_c_h3_response resp, [MarshalAs(UnmanagedType.LPUTF8Str)] string content_type);

    /// <summary>设 / 覆盖响应体。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_set_body", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_response_set_body(uvcpp_c_h3_response resp, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] data, nuint len);

    /// <summary>清空（状态码、头列表、body 全清），可以接着重用。</summary>
    /// <remarks>`_take_completed()` 内部已经先做这一步。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_reset", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_response_reset(uvcpp_c_h3_response resp);

    /// <summary>状态码。</summary>
    /// <returns>状态码；没拿到时返回 `E_NOT_FOUND`（不是 0，也不是默认的 200）。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_status", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_response_status(uvcpp_c_h3_response resp);

    /// <summary>这条响应挂在哪条流上（`_send_request()` 交回来的那个号）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_stream_id", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern long uvcpp_c_h3_response_stream_id(uvcpp_c_h3_response resp);

    /// <summary>这次请求的收场：0 = 正常；非 0 = 没能干净收场（`UV_ECANCELED`：对端 reset / 被 STOP_SENDING 掐掉 / 连接在半路没了）。</summary>
    /// <remarks>有它才不会挂死调用方（一条失败的请求也要在 `_take_completed()` 里出现一次）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_error", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_response_error(uvcpp_c_h3_response resp);

    /// <summary>body 的字节数（二进制安全）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_body_size", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern nuint uvcpp_c_h3_response_body_size(uvcpp_c_h3_response resp);

    /// <summary>拷出 body；调用方给缓冲区（返回真实长度，只有 `cap` 够大时才写）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_body", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_response_body(uvcpp_c_h3_response resp, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>取一个响应头（名字大小写不敏感）；查不到返回 `E_NOT_FOUND`。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_response_header(uvcpp_c_h3_response resp, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] buf, nuint cap);

    /// <summary>有没有这个头（1/0）；空值也算"有"（"没给"与"给了个空串"是两件事）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_response_has_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_response_has_header(uvcpp_c_h3_response resp, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    /// <summary>请求视图的方法名（`"GET"` / `"POST"`……）；调用方给缓冲区。</summary>
    /// <remarks>回调期视图：回调一返回，这些函数对那个视图一律返回 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_view_method", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_request_view_method(uvcpp_c_h3_request_view req_view, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>请求视图的 `:scheme`（对端没发就是空串）。</summary>
    /// <remarks>回调期视图，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_view_scheme", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_request_view_scheme(uvcpp_c_h3_request_view req_view, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>请求视图的 `:authority`（对端没发就是空串）。</summary>
    /// <remarks>回调期视图，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_view_authority", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_request_view_authority(uvcpp_c_h3_request_view req_view, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>请求视图的 `:path`。</summary>
    /// <remarks>回调期视图，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_view_path", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_request_view_path(uvcpp_c_h3_request_view req_view, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>这条请求在哪条流上（答复时要用它）。</summary>
    /// <remarks>回调期视图，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_view_stream_id", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern long uvcpp_c_h3_request_view_stream_id(uvcpp_c_h3_request_view req_view);

    /// <summary>请求体的字节数。</summary>
    /// <remarks>回调期视图，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_view_body_size", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern nuint uvcpp_c_h3_request_view_body_size(uvcpp_c_h3_request_view req_view);

    /// <summary>拷出请求体；调用方给缓冲区，规则同 `uvcpp_c_h3_response_body()`。</summary>
    /// <remarks>回调期视图，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_view_body", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_request_view_body(uvcpp_c_h3_request_view req_view, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>取一个普通头（名字大小写不敏感）；查不到返回 `E_NOT_FOUND`。</summary>
    /// <remarks>回调期视图，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_view_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_request_view_header(uvcpp_c_h3_request_view req_view, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] buf, nuint cap);

    /// <summary>有没有这个普通头（1/0）；空值也算"有"。</summary>
    /// <remarks>回调期视图，出了回调一律 `E_STALE`。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_h3_request_view_has_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_h3_request_view_has_header(uvcpp_c_h3_request_view req_view, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
}
