/**
 * @file bindings/csharp/UvcppNative.cs
 * @brief C ABI 的 C# P/Invoke 绑定：common + net + web + webapp 四片。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一份对应哪些头
 * ----------------
 *   - `src/capi/uvcpp_c_common.h` —— 导出宏 / 错误码 / ABI 版本 / 字符串返回约定；
 *   - `src/capi/uvcpp_c_net.h`    —— `tcp_client` 与 `tcp_server`；
 *   - `src/capi/uvcpp_c_web.h`    —— `http_client` / `http_response` / `ws_client`；
 *   - `src/capi/uvcpp_c_webapp.h` —— app / 路由 / 中间件 / req / resp / next /
 *                                   延迟应答 / 静态目录 / WebSocket 路由。
 * 剩下三片（http2 / quic / http3）在 `UvcppNative.Protocols.cs` 里；两份文件共用
 * 下面这段前导（`UvcppError`、`UvcppCException`、`UvcppErrorInfo`，以及
 * `UvcppNative` 这个 partial 类），所以 `Lib` / ABI 常数只在这里声明一次。
 *
 * 五条承重规矩（与 `uvcpp_c_common.h` 开头逐条对应）
 * ------------------------------------------------
 * 1. **句柄 = 不透明指针 + 魔数。** C# 侧一律 `IntPtr`（见文件顶的 using 别名），
 *    **不要解引用**（布局没有承诺）。`*_free()` 之后再用是 `UVCPP_C_E_STALE`
 *    —— 不是崩溃。
 * 2. **异常绝不越过边界。** C 层每个函数都 `try/catch` 收口，失败返回
 *    `UVCPP_C_E_EXCEPTION`；托管侧 `UvcppErrorInfo.Check()` 把负数返回码翻成
 *    `UvcppCException`。
 * 3. **要扩展的结构体以 `uint32_t size` 打头。** `*_events` /
 *    `uvcpp_c_static_options` / `uvcpp_c_sent_info` 的第一格 `size` 必须写
 *    `(uint)Marshal.SizeOf<T>()`；C 层**逐字段**看 `size` 覆盖到哪一格。
 * 4. **数据所有权三类。** 入参 `(const void*, size_t)` / `(const char*, size_t)`
 *    在调用返回后即可释放；回调里的指针（body、读到的帧）**只在那一次回调里
 *    有效**；返回的 `const char*`（`uvcpp_c_strerror` 那几枚）是静态或线程局部的
 *    常量，**不要释放**。
 * 5. **线程亲和。** 除 `*_post()` / `*_stop()` / `uvcpp_c_deferred_resume()` 等
 *    少数例外，所有函数都只能在**事件循环所在线程**上调用，否则
 *    `UVCPP_C_E_WRONG_THREAD`。
 *
 * 初始化时先比一次 ABI 版本
 * -------------------------
 * `UvcppNative.uvcpp_c_abi_version()` 必须等于 `UvcppNative.UVCPP_C_ABI_VERSION`
 * （头里那个宏）。不等就是这份绑定与 `.so` / `.dll` 不是一次编出来的 —— 这是
 * P/Invoke 最常见的故障模式，不比对的话它通常表现为某处毫无线索的访问违例。
 */

#nullable enable

using System;
using System.Runtime.InteropServices;

/* 不透明句柄别名。C# 的 using 别名是**文件作用域**的，所以
 * `UvcppNative.Protocols.cs` 会在它自己那份里另行声明它用到的句柄别名；
 * 两份文件互不冲突。别名与 C 头的 typedef 逐字同名，下面的签名因此读起来
 * 仍然像 C 头。 */
using uvcpp_c_tcp_client    = System.IntPtr;
using uvcpp_c_tcp_server    = System.IntPtr;
using uvcpp_c_http_client   = System.IntPtr;
using uvcpp_c_http_response = System.IntPtr;
using uvcpp_c_ws_client     = System.IntPtr;
using uvcpp_c_app           = System.IntPtr;
using uvcpp_c_req           = System.IntPtr;
using uvcpp_c_resp          = System.IntPtr;
using uvcpp_c_next          = System.IntPtr;
using uvcpp_c_deferred      = System.IntPtr;
using uvcpp_c_ws_req        = System.IntPtr;
using uvcpp_c_ws_conn       = System.IntPtr;

namespace Uvcpp;

/// <summary>本层错误码，镜像 <c>enum uvcpp_c_error</c>（见 <c>uvcpp_c_common.h</c>）。</summary>
public enum UvcppError : int
{
    /// <summary>成功。</summary>
    UVCPP_C_OK = 0,

    /// <summary>参数不合法（空句柄、空指针、端口越界……）。</summary>
    UVCPP_C_E_INVALID_ARG = -20001,

    /// <summary>句柄已被释放，或从来不是本层交出去的句柄。不是崩溃，是错误码。</summary>
    UVCPP_C_E_STALE = -20002,

    /// <summary>边界内捕获到一个 C++ 异常；详情看 <c>uvcpp_c_last_error_string()</c>。</summary>
    UVCPP_C_E_EXCEPTION = -20003,

    /// <summary>状态不对（没 listen 就 run、已经启动过、被框架持有的对象却要 free……）。</summary>
    UVCPP_C_E_STATE = -20004,

    /// <summary>分配失败。</summary>
    UVCPP_C_E_NO_MEMORY = -20005,

    /// <summary>这个能力在 C 层刻意不提供。</summary>
    UVCPP_C_E_UNSUPPORTED = -20006,

    /// <summary>本地这份库里没编进这个模块。</summary>
    UVCPP_C_E_NOT_BUILT = -20007,

    /// <summary>线程不对：本函数只能在事件循环线程上调用。</summary>
    UVCPP_C_E_WRONG_THREAD = -20008,

    /// <summary>缓冲区太小（“调用方给缓冲区”那类函数用）。</summary>
    UVCPP_C_E_BUFFER_TOO_SMALL = -20009,

    /// <summary>问的那样东西不存在（与“存在但值是空串”分得开）。</summary>
    UVCPP_C_E_NOT_FOUND = -20010
}

/// <summary>把一个负数 C 返回码翻成的托管异常，携带原始 <see cref="Code"/> 与可读文案。</summary>
public sealed class UvcppCException : Exception
{
    /// <summary>原始错误码（<see cref="UvcppError"/> 之一，或 libuv 错误码）。</summary>
    public int Code { get; }

    /// <summary>可读文案（<c>uvcpp_c_strerror()</c> 给的静态串的快照）；可能为 null。</summary>
    public string? Detail { get; }

    /// <summary>用错误码（与可选文案）构造。</summary>
    public UvcppCException(int code, string? detail = null)
        : base(detail ?? ("uvcpp C ABI error " + code))
    {
        Code = code;
        Detail = detail;
    }
}

/// <summary>错误码的托管侧小工具：文案与“检查返回码”那一步。</summary>
public static class UvcppErrorInfo
{
    /// <summary>调用原生 <c>uvcpp_c_strerror()</c>，返回错误码的可读名字。</summary>
    /// <remarks>返回的是库里的静态字符串，本方法当场拷成托管串；调用方不需要释放任何东西。</remarks>
    public static string Strerror(int err)
    {
        IntPtr p = UvcppNative.uvcpp_c_strerror(err);
        return p == IntPtr.Zero ? string.Empty : (Marshal.PtrToStringAnsi(p) ?? string.Empty);
    }

    /// <summary>返回码 &lt; 0 时抛 <see cref="UvcppCException"/>；否则原样返回。</summary>
    /// <remarks>这是“0 或正数 = 成功、负数 = 失败”那条约定在托管侧的落点。</remarks>
    public static void Check(int rc)
    {
        if (rc < 0)
        {
            throw new UvcppCException(rc, Strerror(rc));
        }
    }
}

/// <summary>libuvcpp 的 C ABI 绑定（common + net + web + webapp 四片）。</summary>
public static partial class UvcppNative
{
    /// <summary>原生库名；DllImport 自己会按平台补上 <c>lib</c> / <c>.so</c> / <c>.dll</c>。</summary>
    private const string Lib = "uvcpp";

    /// <summary>本绑定描述的 ABI 版本，镜像头里的 <c>UVCPP_C_ABI_VERSION</c>。</summary>
    /// <remarks>启动时应断言 <c>uvcpp_c_abi_version() == UVCPP_C_ABI_VERSION</c>。</remarks>
    public const int UVCPP_C_ABI_VERSION = 1;

    /* ====================================================================
     * common（uvcpp_c_common.h）
     * ==================================================================== */

    /// <summary>返回本库编出来的 C ABI 版本。</summary>
    /// <returns>库里的 ABI 版本号；应与 <see cref="UVCPP_C_ABI_VERSION"/> 相等，不等就换库。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_abi_version", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uint uvcpp_c_abi_version();

    /// <summary>返回本库的版本字符串（形如 <c>"1.4.4-dev"</c>）。</summary>
    /// <returns>静态字符串的裸指针，进程生命周期内有效，**不要释放**。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_version_string", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern IntPtr uvcpp_c_version_string();

    /// <summary>返回当前活着的 C 句柄个数（建过、还没废掉的）。</summary>
    /// <returns>活句柄数；跨线程读安全，但它是全局的瞬时值，不是同步原语。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_live_handle_count", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern nuint uvcpp_c_live_handle_count();

    /// <summary>返回错误码的可读名字。</summary>
    /// <returns>静态字符串的裸指针，**不要释放**。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_strerror", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern IntPtr uvcpp_c_strerror(int err);

    /// <summary>返回最近一次 C++ 异常 <c>what()</c>（线程局部）。</summary>
    /// <returns>线程局部缓冲的裸指针；先看返回码再看这句话，要留就当场拷走。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_last_error_string", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern IntPtr uvcpp_c_last_error_string();

    /* ====================================================================
     * net（uvcpp_c_net.h）
     * ==================================================================== */

    /// <summary>一次读事件的性质；值与 C++ 侧 <c>net_read_event</c> 一一对应。</summary>
    public enum uvcpp_c_read_event
    {
        /// <summary>有数据，<c>data</c> / <c>size</c> 有效。</summary>
        UVCPP_C_READ_DATA = 0,
        /// <summary>对端正常关闭（TCP FIN）。</summary>
        UVCPP_C_READ_PEER_CLOSED = 1,
        /// <summary>读出错，<c>error</c> 是 libuv 错误码（负值）。</summary>
        UVCPP_C_READ_ERROR = 2
    }

    /// <summary>读回调；在循环线程上调用，`result->data` 只在那次回调里有效。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_read_cb(IntPtr user_data, uvcpp_c_tcp_client client, IntPtr result);

    /// <summary>异步操作（connect / write）的完成回调。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_status_cb(IntPtr user_data, int status);

    /// <summary>无参数通知（连接关闭）。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_notify_cb(IntPtr user_data);

    /// <summary>服务端接受了一条连接。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_connect_cb(IntPtr user_data, uvcpp_c_tcp_client client);

    /// <summary>一次读事件的内容；与 C++ 的 <c>net_read_result</c> 同一套字段。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct uvcpp_c_read_result
    {
        /// <summary><see cref="uvcpp_c_read_event"/>。</summary>
        public int @event;
        /// <summary>DATA 时有效，其余为 NULL；只在那次回调里有效，二进制安全。</summary>
        public IntPtr data;
        /// <summary>DATA 时的字节数（可能含 NUL）。</summary>
        public nuint size;
        /// <summary>READ_ERROR 时的 libuv 错误码；其余为 0。</summary>
        public int error;
        /// <summary>非 0 表示对端在这块数据之后干净收尾（TCP 恒 0）。</summary>
        public int fin;
    }

    /// <summary>客户端事件表；以 <c>size</c> 打头，第一格必须写结构体字节数。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct uvcpp_c_tcp_client_events
    {
        /// <summary>**必须**是 <c>sizeof(uvcpp_c_tcp_client_events)</c> 或你那份结构体的字节数。</summary>
        public uint size;

        /// <summary>读事件（数据 / 对端关闭 / 读错误）。</summary>
        public uvcpp_c_read_cb? on_read;
        /// <summary>传给 <c>on_read</c> 的第一个参数。</summary>
        public IntPtr on_read_user_data;

        /// <summary>连接关闭的通知。</summary>
        public uvcpp_c_notify_cb? on_close;
        /// <summary>传给 <c>on_close</c> 的第一个参数。</summary>
        public IntPtr on_close_user_data;
    }

    /// <summary>服务端事件表；以 <c>size</c> 打头，逐字段看。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct uvcpp_c_tcp_server_events
    {
        /// <summary>**必须**是 <c>sizeof(uvcpp_c_tcp_server_events)</c> 或你那份结构体的字节数。</summary>
        public uint size;

        /// <summary>每接受一条连接回调一次。</summary>
        public uvcpp_c_connect_cb? on_connection;
        /// <summary>传给 <c>on_connection</c> 的第一个参数。</summary>
        public IntPtr on_connection_user_data;

        /// <summary>所有连接共用的一份读回调。</summary>
        public uvcpp_c_read_cb? on_read;
        /// <summary>传给 <c>on_read</c> 的第一个参数。</summary>
        public IntPtr on_read_user_data;
    }

    /// <summary>建一个自带事件循环的 TCP 客户端。</summary>
    /// <returns>句柄；失败返回 <see cref="IntPtr.Zero"/>（内存不足）。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_tcp_client uvcpp_c_tcp_client_new();

    /// <summary>释放客户端（连带它的循环）。</summary>
    /// <returns>0；服务端交出来的连接句柄返回 <see cref="UvcppError.UVCPP_C_E_STATE"/>，二次 free 返回 <see cref="UvcppError.UVCPP_C_E_STALE"/>。</returns>
    /// <remarks>调用方自己保证此时没有别的线程正在用它。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_free(uvcpp_c_tcp_client client);

    /// <summary>装事件表；<paramref name="table"/> 是 <c>uvcpp_c_tcp_client_events</c> 的指针（用 <c>Marshal.StructureToPtr</c> 造），传 <see cref="IntPtr.Zero"/> 把这一组回调全部摘掉。</summary>
    /// <returns>0 或负数错误码。</returns>
    /// <remarks>被包装的委托要自己保活（见 <c>doc/capi-guide.md</c> §5）；回调里不许 free 任何句柄。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_set_events", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_set_events(uvcpp_c_tcp_client client, IntPtr table);

    /// <summary>异步连接；立刻返回，<paramref name="cb"/> 在循环线程上被调用一次。</summary>
    /// <returns><see cref="UvcppError.UVCPP_C_OK"/> 表示已经开始，结果在回调里。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_connect", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_connect(uvcpp_c_tcp_client client,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string ip, int port,
        uvcpp_c_status_cb cb, IntPtr user_data);

    /// <summary>同步连接，最长等 <paramref name="timeout_ms"/>（会阻塞当前线程）。</summary>
    /// <returns>0 成功；否则 libuv 错误码。只能在循环还没跑的时候用。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_connect_wait", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_connect_wait(uvcpp_c_tcp_client client,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string ip, int port, int timeout_ms);

    /// <summary>异步写；<paramref name="data"/> 的内容立刻被拷走。</summary>
    /// <returns>0 已入队；上一笔在途时返回 <c>UV_EALREADY</c>（拒收，不排队）。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_write", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_write(uvcpp_c_tcp_client client,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] data, nuint len,
        uvcpp_c_status_cb cb, IntPtr user_data);

    /// <summary>同步写，最长等 <paramref name="timeout_ms"/>。</summary>
    /// <returns>0 成功；否则 libuv 错误码。只能在循环还没跑的时候用。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_write_wait", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_write_wait(uvcpp_c_tcp_client client,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] data, nuint len,
        int timeout_ms);

    /// <summary>暂停读（背压）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_read_pause", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_read_pause(uvcpp_c_tcp_client client);

    /// <summary>恢复读。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_read_resume", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_read_resume(uvcpp_c_tcp_client client);

    /// <summary>停掉读；之后对端断开也看不见。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_read_stop", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_read_stop(uvcpp_c_tcp_client client);

    /// <summary>关掉连接（异步；可重复调，第二次及以后什么也不做）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_close", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_close(uvcpp_c_tcp_client client);

    /// <summary>跑循环，直到 <c>stop()</c> 或别的路径让它停下。</summary>
    /// <remarks>必须在建这个客户端的那条线程上跑；本层在这里记下“循环线程”。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_run", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_run(uvcpp_c_tcp_client client);

    /// <summary>让 <c>run()</c> 停下来；可以从别的线程调。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_stop", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_stop(uvcpp_c_tcp_client client);

    /// <summary>是不是已连上（1/0）；负数错误码。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_is_connected", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_is_connected(uvcpp_c_tcp_client client);

    /// <summary>上次出错的 libuv 错误码（0 = 没出过错）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_last_error", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_last_error(uvcpp_c_tcp_client client);

    /// <summary>这条连接走的是不是 TLS（1/0）。本批恒为 0。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_is_tls", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_is_tls(uvcpp_c_tcp_client client);

    /// <summary>ALPN 协议名，走“调用方给缓冲区”那套约定。</summary>
    /// <returns>字符串真实长度（不含 NUL）、0 表空串，或负数错误码。本批恒为 0。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_client_alpn_selected", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_client_alpn_selected(uvcpp_c_tcp_client client,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>建一个自带事件循环的服务端。</summary>
    /// <returns>句柄；失败返回 <see cref="IntPtr.Zero"/>（内存不足）。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_server_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_tcp_server uvcpp_c_tcp_server_new();

    /// <summary>释放服务端（连带关掉循环与所有连接）。必须先 <c>stop()</c> 过。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_server_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_server_free(uvcpp_c_tcp_server server);

    /// <summary>装事件表；<paramref name="table"/> 是 <c>uvcpp_c_tcp_server_events</c> 的指针，传 <see cref="IntPtr.Zero"/> 全部摘掉。可在 <c>listen</c> 之前或之后调。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_server_set_events", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_server_set_events(uvcpp_c_tcp_server server, IntPtr table);

    /// <summary>绑定地址；<paramref name="port"/> 为 0 表示让系统挑一个。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_server_bind", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_server_bind(uvcpp_c_tcp_server server,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string ip, int port);

    /// <summary>绑定后真正的端口（&gt; 0），或错误码；没绑上时返回 <see cref="UvcppError.UVCPP_C_E_STATE"/>。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_server_local_port", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_server_local_port(uvcpp_c_tcp_server server);

    /// <summary>开始监听；<paramref name="backlog"/> &lt;= 0 时用 128。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_server_listen", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_server_listen(uvcpp_c_tcp_server server, int backlog);

    /// <summary>设成 n 条循环（SO_REUSEPORT 扇出）；只能在 <c>listen</c> 之前调。</summary>
    /// <remarks>n &gt; 1 之后回调会在 n 条不同线程上跑；跨连接共享状态要靠调用方自己加锁。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_server_set_loops", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_server_set_loops(uvcpp_c_tcp_server server, int n);

    /// <summary>当前设了几条循环（没设过就是 1）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_server_loop_count", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_server_loop_count(uvcpp_c_tcp_server server);

    /// <summary>跑循环，直到 <c>stop()</c>；多循环时一起跑。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_server_run", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_server_run(uvcpp_c_tcp_server server);

    /// <summary>停：关监听、关所有连接、让 <c>run()</c> 返回。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_server_stop", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_server_stop(uvcpp_c_tcp_server server);

    /// <summary>当前挂着的连接数（&gt;= 0），或错误码。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_server_client_count", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_server_client_count(uvcpp_c_tcp_server server);

    /// <summary>关掉所有连接，返回关掉的条数（&gt;= 0），或错误码。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_server_close_all_clients", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_server_close_all_clients(uvcpp_c_tcp_server server);

    /// <summary>上次出错的 libuv 错误码（0 = 没出过错）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_tcp_server_last_error", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_tcp_server_last_error(uvcpp_c_tcp_server server);

    /* ====================================================================
     * web（uvcpp_c_web.h）
     * ==================================================================== */

    /// <summary>一条响应到齐了（或这次请求失败了）；在循环线程上调用。</summary>
    /// <remarks><c>resp</c> 是回调期句柄，回调返回后失效；<c>error != 0</c> 时它可能是 0。</remarks>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_http_response_cb(IntPtr user_data, uvcpp_c_http_response resp, int error);

    /// <summary>异步连接的结果；0 = 成功，否则 libuv 错误码。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_web_status_cb(IntPtr user_data, int status);

    /// <summary>WS 客户端收到一条文本消息（二进制安全，按 <paramref name="len"/> 走）。</summary>
    /// <remarks>C 头把这一枚写成内联函数指针，没有 typedef；这个名字是本绑定起的。</remarks>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_ws_client_text_cb(IntPtr user_data,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] data, nuint len);

    /// <summary>WS 客户端收到一条二进制消息。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_ws_client_binary_cb(IntPtr user_data,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] data, nuint len);

    /// <summary>WS 客户端连接关了；<paramref name="code"/> 见 <c>uvcpp_c_ws_close_code</c>。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_ws_client_close_cb(IntPtr user_data, int code,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] reason, nuint reason_len);

    /// <summary>WS 客户端出错；<paramref name="status"/> 是错误码，<paramref name="what"/> 是一句话。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_ws_client_error_cb(IntPtr user_data, int status,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] what, nuint what_len);

    /// <summary>WS 客户端事件表；以 <c>size</c> 打头，且**没有连接参数**。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct uvcpp_c_ws_client_events
    {
        /// <summary>**必须**是 <c>sizeof(uvcpp_c_ws_client_events)</c> 或你那份结构体的字节数。</summary>
        public uint size;

        /// <summary>收到一条文本消息。</summary>
        public uvcpp_c_ws_client_text_cb? on_text;
        /// <summary>收到一条二进制消息。</summary>
        public uvcpp_c_ws_client_binary_cb? on_binary;
        /// <summary>连接关了。</summary>
        public uvcpp_c_ws_client_close_cb? on_close;
        /// <summary>出错。</summary>
        public uvcpp_c_ws_client_error_cb? on_error;
    }

    /// <summary>建一个自带事件循环的 HTTP/1.1 客户端。</summary>
    /// <returns>句柄；失败返回 <see cref="IntPtr.Zero"/>（内存不足）。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_client_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_http_client uvcpp_c_http_client_new();

    /// <summary>放掉它（连带它的循环与连接）。</summary>
    /// <returns>0；二次 free 返回 <see cref="UvcppError.UVCPP_C_E_STALE"/>，回调里 free 返回 <see cref="UvcppError.UVCPP_C_E_STATE"/>。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_client_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_client_free(uvcpp_c_http_client client);

    /// <summary>要不要在响应之后留着连接（默认要）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_client_set_keep_alive", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_client_set_keep_alive(uvcpp_c_http_client client, int enable);

    /// <summary>异步连接；立刻返回，<paramref name="cb"/> 在循环线程上被调用一次。</summary>
    /// <returns><see cref="UvcppError.UVCPP_C_OK"/> 表示已经开始，结果在回调里；本批不做 DNS 解析，给 IPv4 点分串最稳。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_client_connect", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_client_connect(uvcpp_c_http_client client,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string host, int port,
        uvcpp_c_web_status_cb cb, IntPtr user_data);

    /// <summary>发一条 GET；必须先连接（没连上返回 <c>UV_ENOTCONN</c>）。</summary>
    /// <returns>0 已发出（结果在回调里）；负数 = 没发出去。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_client_get", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_client_get(uvcpp_c_http_client client,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        uvcpp_c_http_response_cb cb, IntPtr user_data);

    /// <summary>发一条 POST；body 立刻被拷走。</summary>
    /// <returns>0 已发出；负数 = 没发出去。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_client_post", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_client_post(uvcpp_c_http_client client,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] body, nuint len,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? content_type,
        uvcpp_c_http_response_cb cb, IntPtr user_data);

    /// <summary>跑循环，直到 <c>stop()</c> 或（关掉 keep-alive 之后）所有请求都答完且连接自然断开。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_client_run", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_client_run(uvcpp_c_http_client client);

    /// <summary>让 <c>run()</c> 停下来；可以从别的线程调。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_client_stop", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_client_stop(uvcpp_c_http_client client);

    /// <summary>主动断开底下的连接；已经关了或还没连过是无操作。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_client_close", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_client_close(uvcpp_c_http_client client);

    /// <summary>TCP 连上了没有（1/0）；负数 = 错误码。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_client_is_connected", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_client_is_connected(uvcpp_c_http_client client);

    /// <summary>上次出错的错误码（0 = 没出过错）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_client_last_error", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_client_last_error(uvcpp_c_http_client client);

    /// <summary>状态码（200 / 404 …）；负数 = 错误码（句柄已失效）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_response_status_code", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_response_status_code(uvcpp_c_http_response resp);

    /// <summary>状态短语（<c>"OK"</c>），走“调用方给缓冲区”那套约定。</summary>
    /// <returns>字符串真实长度（不含 NUL），或负数错误码。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_response_status_message", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_response_status_message(uvcpp_c_http_response resp,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>取一个响应头。</summary>
    /// <returns>值的长度（&gt;= 0）；<see cref="UvcppError.UVCPP_C_E_NOT_FOUND"/> = 没有这个头。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_response_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_response_header(uvcpp_c_http_response resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] buf, nuint cap);

    /// <summary>有没有这个头（1/0）；负数 = 错误码。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_response_has_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_response_has_header(uvcpp_c_http_response resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    /// <summary><c>Content-Type</c>，走“调用方给缓冲区”那套约定。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_response_content_type", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_response_content_type(uvcpp_c_http_response resp,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>响应体（二进制安全，可能含 NUL）。</summary>
    /// <returns><see cref="UvcppError.UVCPP_C_OK"/>；负数 = 错误码。</returns>
    /// <remarks><paramref name="data"/> 只在本次回调里有效，要留就自己拷走。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_http_response_body", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_http_response_body(uvcpp_c_http_response resp,
        out IntPtr data, out nuint len);

    /// <summary>建一个自带事件循环的 WS 客户端。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_client_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_ws_client uvcpp_c_ws_client_new();

    /// <summary>放掉它（连带它的循环与会话）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_client_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_client_free(uvcpp_c_ws_client client);

    /// <summary>装事件表；<paramref name="table"/> 是 <c>uvcpp_c_ws_client_events</c> 的指针，传 <see cref="IntPtr.Zero"/> 全部摘掉。可在 <c>connect</c> 之前或之后调。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_client_set_events", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_client_set_events(uvcpp_c_ws_client client, IntPtr table, IntPtr user_data);

    /// <summary>连一个 <c>ws://host:port/path</c>；握手结果在 <paramref name="cb"/> 里。</summary>
    /// <returns><see cref="UvcppError.UVCPP_C_OK"/> 表示已经开始；非法 URL 当场返回 <see cref="UvcppError.UVCPP_C_E_INVALID_ARG"/>。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_client_connect", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_client_connect(uvcpp_c_ws_client client,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string url,
        uvcpp_c_web_status_cb cb, IntPtr user_data);

    /// <summary>发一条文本消息。</summary>
    /// <returns>0 = 已入队；<c>UV_ENOTCONN</c> = 还没有会话（不静默丢）。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_client_send_text", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_client_send_text(uvcpp_c_ws_client client,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] data, nuint len);

    /// <summary>发一条二进制消息；语义与 <c>send_text</c> 相同。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_client_send_binary", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_client_send_binary(uvcpp_c_ws_client client,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] data, nuint len);

    /// <summary>优雅关闭：发 Close 帧；可重复调（只生效一次）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_client_close", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_client_close(uvcpp_c_ws_client client, int code,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string reason);

    /// <summary>跑循环，直到 <c>stop()</c> 或所有会话都结束。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_client_run", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_client_run(uvcpp_c_ws_client client);

    /// <summary>让 <c>run()</c> 停下来；可以从别的线程调。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_client_stop", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_client_stop(uvcpp_c_ws_client client);

    /// <summary>当前活动的会话数（0 或 1）；负数 = 错误码（句柄已失效）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_client_session_count", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_client_session_count(uvcpp_c_ws_client client);

    /// <summary>握手完成了没有（1/0）；负数 = 错误码。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_client_is_open", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_client_is_open(uvcpp_c_ws_client client);

    /// <summary>上次出错的错误码（0 = 没出过错）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_client_last_error", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_client_last_error(uvcpp_c_ws_client client);

    /* ====================================================================
     * webapp（uvcpp_c_webapp.h）
     * ==================================================================== */

    /// <summary>一条路由（或一条中间件）的处理函数；三个句柄只在本次回调里有效。</summary>
    /// <remarks>不调 <c>next</c> 也不发响应就是“链到此为止”；带出去要走 <c>uvcpp_c_defer</c>。在循环线程上调用，不要做重活。</remarks>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_route_cb(IntPtr user_data, uvcpp_c_req req,
        uvcpp_c_resp resp, uvcpp_c_next next);

    /// <summary>一条 WebSocket 路由的处理函数。</summary>
    /// <remarks>回调返回后 <c>req</c> 失效，但 <c>uvcpp_c_ws_req_conn()</c> 拿到的连接继续有效。</remarks>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_ws_cb(IntPtr user_data, uvcpp_c_ws_req req);

    /// <summary>无参数回调（<c>uvcpp_c_app_post_task</c> 用）。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_void_cb(IntPtr user_data);

    /// <summary>延迟应答的“可以答了”回调；在请求所属循环线程上被调用。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_deferred_cb(IntPtr user_data, uvcpp_c_deferred deferred);

    /// <summary>“响应真的发出去了”的通知；<paramref name="info"/> 只在本次回调里有效。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_sent_cb(IntPtr user_data, IntPtr info);

    /// <summary>WS 连接收到一条文本消息。</summary>
    /// <remarks>C 头把这一枚写成内联函数指针，没有 typedef；这个名字是本绑定起的。</remarks>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_ws_text_cb(IntPtr user_data, uvcpp_c_ws_conn conn,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] data, nuint len);

    /// <summary>WS 连接收到一条二进制消息。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_ws_binary_cb(IntPtr user_data, uvcpp_c_ws_conn conn,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] data, nuint len);

    /// <summary>WS 连接关了；<paramref name="code"/> 见 <c>uvcpp_c_ws_close_code</c>。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_ws_close_cb(IntPtr user_data, uvcpp_c_ws_conn conn, int code,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 4)] byte[] reason, nuint reason_len);

    /// <summary>WS 连接出错；<paramref name="status"/> 是错误码，<paramref name="what"/> 是一句话。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_ws_error_cb(IntPtr user_data, uvcpp_c_ws_conn conn, int status,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 4)] byte[] what, nuint what_len);

    /// <summary><c>uvcpp_c_resp_send_file</c> / <c>_range</c> 的完成回调（C 头里内联，无 typedef）。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void uvcpp_c_send_file_done_cb(IntPtr user_data, int status, ulong bytes_sent);

    /// <summary>WebSocket 关闭码；值与 C++ 侧 <c>ws_close_code</c> 逐条相同。</summary>
    public enum uvcpp_c_ws_close_code
    {
        /// <summary>正常关闭。</summary>
        UVCPP_C_WS_NORMAL = 1000,
        /// <summary>对端离开（服务端停机 / 浏览器跳走）。</summary>
        UVCPP_C_WS_GOING_AWAY = 1001,
        /// <summary>协议错误。</summary>
        UVCPP_C_WS_PROTOCOL_ERROR = 1002,
        /// <summary>收到不能接受的数据类型。</summary>
        UVCPP_C_WS_UNSUPPORTED_DATA = 1003,
        /// <summary>没有状态码。</summary>
        UVCPP_C_WS_NO_STATUS = 1005,
        /// <summary>异常关闭（非正常的底层断开）。</summary>
        UVCPP_C_WS_ABNORMAL_CLOSE = 1006,
        /// <summary>payload 不合法。</summary>
        UVCPP_C_WS_INVALID_PAYLOAD = 1007,
        /// <summary>违反策略。</summary>
        UVCPP_C_WS_POLICY_VIOLATION = 1008,
        /// <summary>消息太大。</summary>
        UVCPP_C_WS_MESSAGE_TOO_BIG = 1009,
        /// <summary>需要扩展。</summary>
        UVCPP_C_WS_EXTENSION_NEEDED = 1010,
        /// <summary>内部错误。</summary>
        UVCPP_C_WS_INTERNAL_ERROR = 1011
    }

    /// <summary>以点开头的文件（<c>.env</c>、<c>.git/...</c>）怎么办；值与 C++ 侧相同。</summary>
    public enum uvcpp_c_dotfile_policy
    {
        /// <summary>当作不存在（404）。默认。</summary>
        UVCPP_C_DOTFILE_HIDE = 0,
        /// <summary>明确拒绝（403）。</summary>
        UVCPP_C_DOTFILE_DENY = 1,
        /// <summary>照常提供。</summary>
        UVCPP_C_DOTFILE_ALLOW = 2
    }

    /// <summary>响应发送结果；与 C++ 的 <c>uvcpp_web_sent_info</c> 同一套字段。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct uvcpp_c_sent_info
    {
        /// <summary>= <c>sizeof(uvcpp_c_sent_info)</c>。</summary>
        public uint size;
        /// <summary>最终状态码。</summary>
        public int status_code;
        /// <summary>实际写出去的 body 字节数（HEAD 时为 0）。</summary>
        public nuint body_bytes;
        /// <summary>所属连接 id（0 = 未知）。</summary>
        public ulong connection_id;
        /// <summary>写成功 1 / 失败 0（连接已断等）。</summary>
        public int ok;
        /// <summary>这次是不是流式（chunked）发的。</summary>
        public int streamed;
    }

    /// <summary>一条 WebSocket 连接上会发生的四件事；以 <c>size</c> 打头，逐字段看。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct uvcpp_c_ws_events
    {
        /// <summary>**必须**是 <c>sizeof(uvcpp_c_ws_events)</c> 或你那份结构体的字节数。</summary>
        public uint size;

        /// <summary>收到一条文本消息（按 <c>len</c> 走，可能含 NUL）。</summary>
        public uvcpp_c_ws_text_cb? on_text;

        /// <summary>收到一条二进制消息。</summary>
        public uvcpp_c_ws_binary_cb? on_binary;

        /// <summary>连接关了。</summary>
        public uvcpp_c_ws_close_cb? on_close;

        /// <summary>出错。</summary>
        public uvcpp_c_ws_error_cb? on_error;
    }

    /// <summary><c>uvcpp_c_app_serve_static()</c> 的选项；以 <c>size</c> 打头，传 NULL 等价全默认。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct uvcpp_c_static_options
    {
        /// <summary>**必须**是 <c>sizeof(uvcpp_c_static_options)</c> 或你那份结构体的字节数。</summary>
        public uint size;

        /// <summary>目录请求时按顺序试的文件名（逗号分隔）；NULL = 默认 <c>index.html</c>，空串 = 不试。</summary>
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? index_files;

        /// <summary>非 0：找不到的路径回落到 <c>spa_file</c>。</summary>
        public int spa_fallback;
        /// <summary>SPA 兜底用的文件（相对根目录）。</summary>
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? spa_file;

        /// <summary>单个文件的大小上限（字节）。0 = 用默认值。</summary>
        public nuint max_file_size;

        /// <summary>要不要发 <c>ETag</c>（1/0）。默认 1。</summary>
        public int etag;
        /// <summary>要不要发 <c>Last-Modified</c>（1/0）。默认 1。</summary>
        public int last_modified;
        /// <summary>要不要支持 <c>Range</c>（1/0）。默认 1。</summary>
        public int range;

        /// <summary><c>Cache-Control</c> 的值；NULL = 不发这个头。</summary>
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? cache_control;

        /// <summary>元数据缓存的格数与字节上限；0 = 用默认值。</summary>
        public nuint cache_max_entries;
        /// <summary>元数据缓存的字节上限；0 = 用默认值。</summary>
        public nuint cache_max_bytes;

        /// <summary><see cref="uvcpp_c_dotfile_policy"/>。</summary>
        public int dotfiles;

        /// <summary>要不要跟随符号链接（1/0）。默认 0。</summary>
        public int follow_symlinks;

        /// <summary>要不要给文本类型补 <c>; charset=utf-8</c>（1/0）。默认 1。</summary>
        public int add_charset;
    }

    /// <summary>建一个 app；默认是“什么都没配”的默认值。</summary>
    /// <returns>新句柄；失败返回 <see cref="IntPtr.Zero"/>（只有内存不够会走到）。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_new", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_app uvcpp_c_app_new();

    /// <summary>废掉它（含它内部那些循环与连接）。</summary>
    /// <remarks>必须在回调之外调（回调里调返回 <see cref="UvcppError.UVCPP_C_E_STATE"/>）；还没 stop+join 会先替你收尾一次。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_free(uvcpp_c_app app);

    /// <summary>监听地址；NULL = 默认 <c>127.0.0.1</c>。都要在 <c>start()</c> 之前调。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_host", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_host(uvcpp_c_app app,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? host);

    /// <summary>监听端口；0 表示让内核挑，之后用 <c>uvcpp_c_app_bound_port()</c> 问。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_port", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_port(uvcpp_c_app app, int port);

    /// <summary><c>listen()</c> 的 backlog。默认 128。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_backlog", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_backlog(uvcpp_c_app app, int backlog);

    /// <summary>请求 body 上限（字节）。超了给 413。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_max_body_size", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_max_body_size(uvcpp_c_app app, nuint bytes);

    /// <summary>请求头总字节上限。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_max_header_bytes", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_max_header_bytes(uvcpp_c_app app, nuint bytes);

    /// <summary>请求行（URL）上限。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_max_url_bytes", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_max_url_bytes(uvcpp_c_app app, nuint bytes);

    /// <summary>响应压缩开关（1/0）。没编进 zlib 时开着也不报错，只是不压。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_compression", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_compression(uvcpp_c_app app, int enable);

    /// <summary>访问日志开关（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_access_log", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_access_log(uvcpp_c_app app, int enable);

    /// <summary><c>Server:</c> 头的值；NULL = 不发这个头。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_server_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_server_header(uvcpp_c_app app,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? value);

    /// <summary>停机优雅窗口（毫秒）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_shutdown_grace_ms", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_shutdown_grace_ms(uvcpp_c_app app, int ms);

    /// <summary>空闲连接超时（毫秒）；0 = 不超时。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_idle_timeout_ms", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_idle_timeout_ms(uvcpp_c_app app, int ms);

    /// <summary>自动应答 <c>OPTIONS</c>（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_auto_options", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_auto_options(uvcpp_c_app app, int enable);

    /// <summary><c>HEAD</c> 按 <c>GET</c> 处理再丢掉 body（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_head_as_get", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_head_as_get(uvcpp_c_app app, int enable);

    /// <summary>同一条连接上最多流水线几个请求。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_max_pipelined_requests", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_max_pipelined_requests(uvcpp_c_app app, nuint n);

    /// <summary>上传落盘的目录；目录必须已经存在（本层不替你 mkdir）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_upload_dir", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_upload_dir(uvcpp_c_app app,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string dir);

    /// <summary>单次请求上传总字节上限。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_max_upload_size", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_max_upload_size(uvcpp_c_app app, ulong bytes);

    /// <summary>单个上传文件的大小上限。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_max_file_size", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_max_file_size(uvcpp_c_app app, ulong bytes);

    /// <summary>事件循环条数（1..64）；必须在 start* 之前调。</summary>
    /// <remarks>它是本层唯一一处“多循环会影响 C 面能力”的地方：多循环下 <c>uvcpp_c_deferred_post()</c> 返回 <see cref="UvcppError.UVCPP_C_E_UNSUPPORTED"/>。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_set_loops", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_set_loops(uvcpp_c_app app, int n);

    /// <summary>当前配了多少条循环（默认 1）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_loop_count", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_loop_count(uvcpp_c_app app);

    /// <summary>注册一条 GET 路由。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_get", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_get(uvcpp_c_app app,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string pattern, uvcpp_c_route_cb cb, IntPtr user_data);

    /// <summary>注册一条 POST 路由。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_post", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_post(uvcpp_c_app app,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string pattern, uvcpp_c_route_cb cb, IntPtr user_data);

    /// <summary>注册一条 PUT 路由。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_put", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_put(uvcpp_c_app app,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string pattern, uvcpp_c_route_cb cb, IntPtr user_data);

    /// <summary>注册一条 DELETE 路由。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_del", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_del(uvcpp_c_app app,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string pattern, uvcpp_c_route_cb cb, IntPtr user_data);

    /// <summary>注册一条 PATCH 路由。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_patch", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_patch(uvcpp_c_app app,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string pattern, uvcpp_c_route_cb cb, IntPtr user_data);

    /// <summary>注册一条 HEAD 路由。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_head", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_head(uvcpp_c_app app,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string pattern, uvcpp_c_route_cb cb, IntPtr user_data);

    /// <summary>注册一条 OPTIONS 路由。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_options", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_options(uvcpp_c_app app,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string pattern, uvcpp_c_route_cb cb, IntPtr user_data);

    /// <summary>注册一条匹配任意方法的路由。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_any", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_any(uvcpp_c_app app,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string pattern, uvcpp_c_route_cb cb, IntPtr user_data);

    /// <summary>挂一条中间件（与路由同一个回调形状）；按注册顺序排在路由之前。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_use", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_use(uvcpp_c_app app, uvcpp_c_route_cb cb, IntPtr user_data);

    /// <summary>注册一条上传路由（<c>multipart/form-data</c>）；本批读不到上传结果。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_post_upload", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_post_upload(uvcpp_c_app app,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string pattern, uvcpp_c_route_cb cb, IntPtr user_data);

    /// <summary>把 <paramref name="root_dir"/> 挂在 URL 前缀 <paramref name="prefix"/> 上。</summary>
    /// <remarks><paramref name="options"/> 是 <c>uvcpp_c_static_options</c> 的指针（见 <c>Marshal.StructureToPtr</c>）；传 <see cref="IntPtr.Zero"/> = 全默认。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_serve_static", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_serve_static(uvcpp_c_app app,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string prefix,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string root_dir, IntPtr options);

    /// <summary>挂一条 WebSocket 路由（GET + Upgrade）；握手成功后按 <paramref name="cb"/> 调你。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_websocket", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_websocket(uvcpp_c_app app,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string pattern, uvcpp_c_ws_cb cb, IntPtr user_data);

    /// <summary>在当前线程跑事件循环（阻塞，直到 <c>uvcpp_c_app_stop()</c>）。</summary>
    /// <returns>0；负数错误码（配置不对、端口被占等）。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_start", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_start(uvcpp_c_app app);

    /// <summary>起后台线程跑事件循环，立刻返回。</summary>
    /// <remarks>返回 0 之后循环可能还没起来；要确定它开始服务，请轮询 <c>uvcpp_c_app_bound_port()</c> 或直接打一次。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_start_background", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_start_background(uvcpp_c_app app);

    /// <summary>让循环停下来；可以从别的线程调。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_stop", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_stop(uvcpp_c_app app);

    /// <summary>等后台线程退出；配合 <c>uvcpp_c_app_start_background()</c> 用。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_join", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_join(uvcpp_c_app app);

    /// <summary>实际绑到的端口；还没 start* 时返回 0。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_bound_port", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_bound_port(uvcpp_c_app app);

    /// <summary>循环是否在跑（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_running", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_running(uvcpp_c_app app);

    /// <summary>当前连着的连接数（所有循环加起来）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_connection_count", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern nuint uvcpp_c_app_connection_count(uvcpp_c_app app);

    /// <summary>第 <paramref name="loop_index"/> 条循环上的连接数（越界返回 0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_connection_count_at", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern nuint uvcpp_c_app_connection_count_at(uvcpp_c_app app, int loop_index);

    /// <summary>正在处理（已收下、还没写完响应）的请求数。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_inflight_count", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern nuint uvcpp_c_app_inflight_count(uvcpp_c_app app);

    /// <summary>活着的 WebSocket 会话数。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_ws_session_count", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern nuint uvcpp_c_app_ws_session_count(uvcpp_c_app app);

    /// <summary>把 <paramref name="cb"/> 投到 app 的循环上；可以从别的线程调。</summary>
    /// <returns><see cref="UvcppError.UVCPP_C_OK"/>；多循环下返回 <see cref="UvcppError.UVCPP_C_E_UNSUPPORTED"/>（见 <c>set_loops</c>）。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_app_post_task", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_app_post_task(uvcpp_c_app app, uvcpp_c_void_cb cb, IntPtr user_data);

    /// <summary>方法名（<c>"GET"</c> / <c>"POST"</c> …），走“调用方给缓冲区”那套约定。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_method_name", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_method_name(uvcpp_c_req req,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>解码后的路径（不含 query）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_path", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_path(uvcpp_c_req req,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>原始（未解码）路径。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_raw_path", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_raw_path(uvcpp_c_req req,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>原始 query 串（<c>a=1&amp;b=2</c>）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_query_string", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_query_string(uvcpp_c_req req,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>一个 query 参数。</summary>
    /// <returns>值的长度；<see cref="UvcppError.UVCPP_C_E_NOT_FOUND"/> = 查不到。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_query", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_query(uvcpp_c_req req,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] buf, nuint cap);

    /// <summary>一个请求头（大小写不敏感）。</summary>
    /// <returns>值的长度；<see cref="UvcppError.UVCPP_C_E_NOT_FOUND"/> = 查不到。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_header(uvcpp_c_req req,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] buf, nuint cap);

    /// <summary>有没有这个请求头（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_has_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_has_header(uvcpp_c_req req,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    /// <summary><c>Content-Type</c>。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_content_type", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_content_type(uvcpp_c_req req,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary><c>Content-Length</c>（没有这个头时为 0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_content_length", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern nuint uvcpp_c_req_content_length(uvcpp_c_req req);

    /// <summary><c>Host</c> 头。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_host", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_host(uvcpp_c_req req,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>对端 IP（点分十进制 / 冒号十六进制）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_peer_ip", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_peer_ip(uvcpp_c_req req,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>对端端口。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_peer_port", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uint uvcpp_c_req_peer_port(uvcpp_c_req req);

    /// <summary>路由参数（<c>/users/:id</c> 里的 <c>id</c>）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_param", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_param(uvcpp_c_req req,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] buf, nuint cap);

    /// <summary>一个 cookie。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_cookie", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_cookie(uvcpp_c_req req,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] buf, nuint cap);

    /// <summary>这条连接想不想 keep-alive（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_is_keep_alive", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_is_keep_alive(uvcpp_c_req req);

    /// <summary>body 的原始字节（二进制安全）。</summary>
    /// <returns><see cref="UvcppError.UVCPP_C_OK"/>，或负数错误码（句柄失效）。</returns>
    /// <remarks><paramref name="data"/> 只在本次回调里有效，要留就自己拷走。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_body", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_body(uvcpp_c_req req, out IntPtr data, out nuint len);

    /// <summary>body 是不是空（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_req_body_empty", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_req_body_empty(uvcpp_c_req req);

    /// <summary>设状态码。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_status", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_status(uvcpp_c_resp resp, int code);

    /// <summary>设状态行里的原因短语（默认由状态码推）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_status_message", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_status_message(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string msg);

    /// <summary>当前状态码。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_status_code", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_status_code(uvcpp_c_resp resp);

    /// <summary>设一个响应头（同名覆盖）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_set_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_set_header(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string value);

    /// <summary>追加一个响应头（同名不覆盖，发两个）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_add_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_add_header(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string value);

    /// <summary>删掉一个响应头。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_remove_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_remove_header(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    /// <summary>有没有这个响应头（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_has_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_has_header(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    /// <summary>取一个响应头的值。</summary>
    /// <returns>值的长度；<see cref="UvcppError.UVCPP_C_E_NOT_FOUND"/> = 没有这个头。</returns>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_get_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_get_header(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] buf, nuint cap);

    /// <summary>设 <c>Content-Type</c>（等价于 set_header）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_set_content_type", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_set_content_type(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string ct);

    /// <summary>取 <c>Content-Type</c>。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_content_type", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_content_type(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>写一个 cookie（<c>Set-Cookie</c>）。</summary>
    /// <remarks><paramref name="max_age"/> &lt; 0 表示会话 cookie；<paramref name="same_site"/> NULL = <c>"Lax"</c>。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_set_cookie", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_set_cookie(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string value,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, long max_age,
        int http_only, int secure, [MarshalAs(UnmanagedType.LPUTF8Str)] string? same_site);

    /// <summary>设 body（原始字节，立刻被拷走）；<paramref name="content_type"/> NULL = 不动这个头。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_body", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_body(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] data, nuint len,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? content_type);

    /// <summary>设 body 为文本，并设 <c>Content-Type: text/plain; charset=utf-8</c>。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_text", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_text(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string s);

    /// <summary>同上，<c>text/html</c>。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_html", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_html(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string s);

    /// <summary>设 body 为一段已经是 JSON 文本的字节（本层不解析、不生成）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_json_str", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_json_str(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string json);

    /// <summary>设 body 为二进制，并设 <c>Content-Type</c>（NULL = <c>application/octet-stream</c>）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_binary", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_binary(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] data, nuint len,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? content_type);

    /// <summary>当前 body 的字节数。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_body_size", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern nuint uvcpp_c_resp_body_size(uvcpp_c_resp resp);

    /// <summary>清空 body。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_clear_body", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_clear_body(uvcpp_c_resp resp);

    /// <summary>302（或 <paramref name="code"/>）重定向到 <paramref name="url"/>。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_redirect", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_redirect(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string url, int code);

    /// <summary>404，body 是 <paramref name="what"/>（NULL = 空）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_not_found", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_not_found(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? what);

    /// <summary>400。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_bad_request", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_bad_request(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? what);

    /// <summary>403。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_forbidden", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_forbidden(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? what);

    /// <summary>413。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_payload_too_large", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_payload_too_large(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? what);

    /// <summary>500。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_server_error", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_server_error(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? what);

    /// <summary>把响应发出去；每次响应必须恰好调一次。</summary>
    /// <remarks>之后 <c>uvcpp_c_resp_*</c> 的写口一律 <see cref="UvcppError.UVCPP_C_E_STATE"/>；延迟应答那条路上还要 <c>uvcpp_c_deferred_resume()</c>。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_end", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_end(uvcpp_c_resp resp);

    /// <summary>已经 <c>end()</c> 过没有（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_ended", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_ended(uvcpp_c_resp resp);

    /// <summary>响应真的写完之后通知一次。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_on_sent", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_on_sent(uvcpp_c_resp resp, uvcpp_c_sent_cb cb, IntPtr user_data);

    /// <summary>开始一段 chunked 响应（状态行与响应头当场发出去）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_begin_chunked", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_begin_chunked(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? content_type);

    /// <summary>写一块（长度 0 合法：那是一块空的 chunk）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_write_chunk", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_write_chunk(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] data, nuint len);

    /// <summary>写对端消化得动了再通知一次（背压用）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_on_drain", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_on_drain(uvcpp_c_resp resp, uvcpp_c_void_cb cb, IntPtr user_data);

    /// <summary>是不是已经在流式发送（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_streaming", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_streaming(uvcpp_c_resp resp);

    /// <summary>流式已写出的字节数。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_stream_bytes_written", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern ulong uvcpp_c_resp_stream_bytes_written(uvcpp_c_resp resp);

    /// <summary>把 <paramref name="path"/> 这个文件当 body 发出去；它异步，返回 0 只表示开始发了。</summary>
    /// <remarks><paramref name="done"/> 可为 NULL（<c>(user, status, bytes_sent)</c>）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_send_file", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_send_file(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        uvcpp_c_send_file_done_cb? done, IntPtr user_data);

    /// <summary>只发文件的 <c>[first, last]</c> 字节（闭区间，Range 请求用）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_resp_send_file_range", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_resp_send_file_range(uvcpp_c_resp resp,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, ulong first, ulong last,
        uvcpp_c_send_file_done_cb? done, IntPtr user_data);

    /// <summary>继续走链上的下一个；同一枚 next 只能 run 一次（第二次 <see cref="UvcppError.UVCPP_C_E_STATE"/>）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_next_run", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_next_run(uvcpp_c_next next);

    /// <summary>拿走 <c>next</c>，把这枚请求挂起；失败返回 <see cref="IntPtr.Zero"/>。</summary>
    /// <remarks>拿了就一定要还：每条 deferred 最终必须走上 <c>_resume()</c>（继续）或 <c>_free()</c>（放弃）之一。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_defer", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_deferred uvcpp_c_defer(uvcpp_c_next next);

    /// <summary>这个请求自己的 req 句柄；只在请求所属循环线程上碰，**不要 free**。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_deferred_req", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_req uvcpp_c_deferred_req(uvcpp_c_deferred deferred);

    /// <summary>同 <c>_req()</c>，给的是 resp 句柄。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_deferred_resp", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_resp uvcpp_c_deferred_resp(uvcpp_c_deferred deferred);

    /// <summary>把 <paramref name="cb"/> 投到这次请求所属那条循环上；可从别的线程调。</summary>
    /// <returns><see cref="UvcppError.UVCPP_C_OK"/>；多循环 <see cref="UvcppError.UVCPP_C_E_UNSUPPORTED"/>；循环已停 <see cref="UvcppError.UVCPP_C_E_STATE"/>。</returns>
    /// <remarks><paramref name="cb"/> 跑起来之前 deferred 必须还活着（它是这条投递的凭据）。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_deferred_post", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_deferred_post(uvcpp_c_deferred deferred,
        uvcpp_c_deferred_cb cb, IntPtr user_data);

    /// <summary>继续走链（next()）；可以从任何线程调。</summary>
    /// <remarks>调它之前响应应该已经写好并 end 过；调完之后 deferred 还可以 _free()。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_deferred_resume", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_deferred_resume(uvcpp_c_deferred deferred);

    /// <summary>放掉这个句柄（不继续链）；在 resume 之前调它就是“放弃这次请求”。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_deferred_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_deferred_free(uvcpp_c_deferred deferred);

    /// <summary>升级请求的路径（已解码）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_req_path", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_req_path(uvcpp_c_ws_req req,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>命中的路由模式（<c>uvcpp_c_app_websocket()</c> 注册的那个）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_req_route", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_req_route(uvcpp_c_ws_req req,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>路由参数。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_req_param", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_req_param(uvcpp_c_ws_req req,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] buf, nuint cap);

    /// <summary>query 参数。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_req_query", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_req_query(uvcpp_c_ws_req req,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] buf, nuint cap);

    /// <summary>cookie。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_req_cookie", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_req_cookie(uvcpp_c_ws_req req,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] buf, nuint cap);

    /// <summary>升级请求的请求头。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_req_header", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_req_header(uvcpp_c_ws_req req,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] buf, nuint cap);

    /// <summary>对端 IP。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_req_peer_ip", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_req_peer_ip(uvcpp_c_ws_req req,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] buf, nuint cap);

    /// <summary>这条升级请求背后的连接；它比本次回调活得久，没有 <c>_free()</c>。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_req_conn", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern uvcpp_c_ws_conn uvcpp_c_ws_req_conn(uvcpp_c_ws_req req);

    /// <summary>给连接挂事件表（<paramref name="events"/> 是 <c>uvcpp_c_ws_events</c> 的指针；同一枚连接重复挂 = 覆盖）。</summary>
    /// <remarks>表按值拷进框架，返回后你可以随便释放 <paramref name="events"/> 指向的内存；<paramref name="user_data"/> 原样传回。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_conn_set_events", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_conn_set_events(uvcpp_c_ws_conn conn, IntPtr events, IntPtr user_data);

    /// <summary>发一条文本消息（二进制安全，按 <paramref name="len"/> 走）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_conn_send_text", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_conn_send_text(uvcpp_c_ws_conn conn,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] data, nuint len);

    /// <summary>发一条二进制消息。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_conn_send_binary", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_conn_send_binary(uvcpp_c_ws_conn conn,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] data, nuint len);

    /// <summary>发一个 ping。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_conn_send_ping", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_conn_send_ping(uvcpp_c_ws_conn conn);

    /// <summary>发一个 pong。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_conn_send_pong", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_conn_send_pong(uvcpp_c_ws_conn conn);

    /// <summary>发一条 close 帧并开始关闭握手。</summary>
    /// <remarks><paramref name="code"/> 见 <c>uvcpp_c_ws_close_code</c>（0 = 不发码）；<paramref name="reason"/> 可以不 NUL 结尾。</remarks>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_conn_close", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_conn_close(uvcpp_c_ws_conn conn, int code,
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] byte[] reason, nuint reason_len);

    /// <summary>立刻断链（不发 close 帧）；对端只会看到 TCP 断。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_conn_terminate", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_conn_terminate(uvcpp_c_ws_conn conn);

    /// <summary>这条连接还在开着吗（1/0）。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_conn_is_open", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_conn_is_open(uvcpp_c_ws_conn conn);

    /// <summary>单条消息的大小上限。</summary>
    [DllImport(Lib, EntryPoint = "uvcpp_c_ws_conn_set_max_message_size", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int uvcpp_c_ws_conn_set_max_message_size(uvcpp_c_ws_conn conn, nuint bytes);
}
