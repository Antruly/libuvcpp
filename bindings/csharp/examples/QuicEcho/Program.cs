// QUIC 回显服务端 + 一次完整的收发（C# / P-Invoke）
//
// 这个例子要证明的是"拿 UvcppNative*.cs 两个文件 + 一份 libuvcpp 动态库就能写出
// 一个能跑的 QUIC 应用"，所以它**自带客户端**：默认跑一次回环往返（服务端收、
// 服务端回、客户端收到同样的字节），打印结果并给退出码（0 = 通过）。带 `--serve`
// 时它只当服务端，一直跑到 Ctrl+C。
//
//     dotnet run                 # 回环往返，自带判据
//     dotnet run -- --serve      # 只起服务端（监听 4433，可用 --port 改）
//
// 四件在 C# 侧必须知道的事（都是 C ABI 那一层的规矩，不是本绑定的选择）：
//
//   1. **线程**：除了少数 `*_post()`，所有函数都只能在事件循环所在的线程上调用。
//      所以下面的回环把两端的循环**放在同一条线程上交替泵**（`*_run_once()` 是
//      非阻塞的一轮）。真做应用时把每一端放到自己的线程上用 `*_run()` 跑就行，
//      只要别在两个线程上泵同一个端点（libuv 的规矩）。
//   2. **委托要自己保活**。回调是函数指针，托管委托被 GC 回收之后 C 侧那个指针
//      就是悬垂的 —— 症状是随机的访问违例。下面所有委托都放在 static 字段里。
//   3. **回调里的指针只在回调期间有效**（`result.data`）。要留就当场 `Marshal.Copy`
//      拷进托管数组，下面的 `Copy` 就是干这个的。
//   4. **不许让异常逃出回调**。回调是 C 侧调进来的，托管异常要穿过 `uv_run` 那几层
//      native 帧，运行时的处置是打印 + abort（本机实测：`Unhandled exception` 之后
//      进程以 134 退出并留下 core）。所以下面每个回调体都自己 try/catch，把失败记在
//      `_cbError` 里，回到主线程再判 —— 这是唯一稳的形状。
//
// 还有一条 QUIC 自己的、net 那层没有的坑，写在 `OnServerRead` 上面：**一次收尾
// 会来两条回调**。

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using Uvcpp;

internal static class Program
{
    /// <summary>两端要协商的 ALPN 协议名（QUIC 的 ALPN 是 TLS 扩展，必须对上）。</summary>
    private const string Alpn = "uvcpp-echo/1";

    /// <summary>回环用的载荷。</summary>
    private static readonly byte[] Payload =
        System.Text.Encoding.UTF8.GetBytes("hello quic, from C#\n");

    /// <summary>整个回环的等待上限（毫秒）。回环上正常是几毫秒。</summary>
    private const int TimeoutMs = 15000;

    // ------------------------------------------------------------------
    // 委托保活（见文件头第 2 条）：全部 static，进程生命周期内不被回收
    // ------------------------------------------------------------------
    private static UvcppNative.uvcpp_c_quic_server_listen_cb? _onAccept;
    private static UvcppNative.uvcpp_c_quic_on_read_cb? _serverOnRead;
    private static UvcppNative.uvcpp_c_quic_on_write_cb? _serverOnWrite;
    private static UvcppNative.uvcpp_c_quic_on_alpn_cb? _serverOnAlpn;
    private static UvcppNative.uvcpp_c_quic_on_close_cb? _serverOnClose;
    private static UvcppNative.uvcpp_c_status_cb? _onConnect;
    private static UvcppNative.uvcpp_c_quic_on_read_cb? _clientOnRead;
    private static UvcppNative.uvcpp_c_quic_on_close_cb? _clientOnClose;

    // ------------------------------------------------------------------
    // 回环用的状态（单连接，所以不需要并发容器）
    // ------------------------------------------------------------------
    private static IntPtr _client;
    private static IntPtr _tlsSrv;
    private static IntPtr _tlsCli;
    private static long _clientStream = -1;
    private static int _connectStatus = int.MinValue;   // MinValue = 回调还没来
    private static readonly List<byte> Echo = new();    // 客户端收到的回显
    private static readonly Dictionary<long, List<byte>> SrvBuf = new(); // 服务端按流攒
    private static bool _clientDone;
    private static bool _serverClosed;
    private static bool _clientClosed;
    private static string _alpnSeen = string.Empty;

    /// <summary>回调里出的错（见文件头第 4 条）。空串 = 没出错。</summary>
    private static string _cbError = string.Empty;

    private static int Main(string[] args)
    {
        try
        {
            if (!CheckAbi()) return 1;
            int port = ArgPort(args);
            return HasFlag(args, "--serve") ? ServeForever(port) : SelfTest();
        }
        catch (UvcppCException ex)
        {
            Console.Error.WriteLine($"C ABI 报错：{ex.Code} — {ex.Message}");
            return 1;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            return 1;
        }
    }

    // ==================================================================
    // 起手式：先核对 ABI 版本
    // ==================================================================
    /// <summary>
    /// 库的 ABI 版本必须等于这份绑定描述的版本。
    ///
    /// 这是 P/Invoke 最常见的故障模式：头（= 这份 .cs）与动态库不是一次编出来的。
    /// 不对上时的症状通常不是一句"版本不对"，而是某个结构体少了一个字段、某次回调
    /// 的栈被写坏。所以第一件事就是把它变成一个明确的失败。
    /// </summary>
    private static bool CheckAbi()
    {
        uint lib = UvcppNative.uvcpp_c_abi_version();
        if (lib != UvcppNative.UVCPP_C_ABI_VERSION)
        {
            Console.Error.WriteLine(
                $"ABI 对不上：库是 {lib}，这份绑定是 {UvcppNative.UVCPP_C_ABI_VERSION}。换库，别硬跑。");
            return false;
        }

        string ver = Marshal.PtrToStringAnsi(UvcppNative.uvcpp_c_version_string()) ?? "?";
        Console.WriteLine($"libuvcpp {ver}（C ABI {lib}）");
        return true;
    }

    // ==================================================================
    // 服务端：建 TLS 上下文 -> 绑地址 -> listen（回调里再装连接的七条回调）
    // ==================================================================
    /// <summary>建一个回显服务端并开始接收（不跑循环）。</summary>
    private static IntPtr BuildServer(int port)
    {
        // QUIC 的 crypto 后端要在**任何**端点之前初始化，进程内一次。
        Check(UvcppNative.uvcpp_c_quic_crypto_init(), "quic_crypto_init");

        // 自签证书：给本地验证用，免得往仓里塞 PEM。生产用
        // uvcpp_c_quic_tls_server_new(cert, key)。
        IntPtr tls = UvcppNative.uvcpp_c_quic_tls_server_selfsigned("localhost");
        if (tls == IntPtr.Zero) throw new InvalidOperationException("自签 TLS 上下文建不出来");
        _tlsSrv = tls;

        IntPtr server = UvcppNative.uvcpp_c_quic_server_new();
        if (server == IntPtr.Zero) throw new InvalidOperationException("quic_server_new 返回 NULL");

        Check(UvcppNative.uvcpp_c_quic_server_set_tls(server, tls), "server_set_tls");
        SetAlpn(server, asServer: true);

        // 端口 0 = 由内核挑一个；挑中的在 listen() 之后用 configured_port() 读。
        Check(UvcppNative.uvcpp_c_quic_server_bind(server, "127.0.0.1", port), "server_bind");

        // 回调必须自己保活（文件头第 2 条）。
        _onAccept = OnAccept;
        Check(UvcppNative.uvcpp_c_quic_server_listen(server, _onAccept, IntPtr.Zero), "server_listen");

        int bound = UvcppNative.uvcpp_c_quic_server_configured_port(server);
        Console.WriteLine($"服务端在 127.0.0.1:{(port == 0 ? bound : port)} 上收 QUIC（ALPN {Alpn}）");
        return server;
    }

    /// <summary>
    /// 每来一条新连接调一次。
    ///
    /// **它在连接刚建出来（首包到达）时跑，不在握手完成时跑** —— 所以 ALPN 与
    /// 数据那几条回调必须在**这里**装上，它们都排在这一条之后。
    /// </summary>
    private static void OnAccept(IntPtr userData, IntPtr conn)
    {
        try
        {
            _serverOnRead = OnServerRead;
            _serverOnWrite = OnServerWrite;
            _serverOnAlpn = OnServerAlpn;
            _serverOnClose = OnServerClose;

            // size 必须最先填（回调表的版本约定）：C 侧逐字段按它决定读到哪一格。
            var cbs = new UvcppNative.uvcpp_c_quic_callbacks
            {
                size = (uint)Marshal.SizeOf<UvcppNative.uvcpp_c_quic_callbacks>(),
                on_read = _serverOnRead,
                on_write = _serverOnWrite,
                on_alpn = _serverOnAlpn,
                on_close = _serverOnClose,
            };
            Check(UvcppNative.uvcpp_c_quic_conn_set_callbacks(conn, ref cbs, IntPtr.Zero),
                  "conn_set_callbacks");
            Console.WriteLine("  服务端：新连接进来了");
        }
        catch (Exception ex)
        {
            Note("OnAccept", ex);
        }
    }

    private static void OnServerWrite(IntPtr userData, IntPtr conn, long streamId, int status)
    {
        if (status != 0) Console.Error.WriteLine($"  服务端写失败：{status} {UvcppErrorInfo.Strerror(status)}");
    }

    private static void OnServerAlpn(IntPtr userData, IntPtr conn, string alpn)
    {
        _alpnSeen = alpn ?? string.Empty;
    }

    private static void OnServerClose(IntPtr userData, IntPtr conn, int errorCode)
    {
        _serverClosed = true;
        Console.WriteLine($"  服务端：连接结束（code={errorCode}）");
    }

    /// <summary>
    /// 收到一条流上的数据。
    ///
    /// **两个 QUIC 特有的形状，都在这里：**
    ///
    /// `stream_id` —— 一条连接上并行跑着很多条流，所以服务端要按流号分流（TCP
    /// 那层没有这个参数）。
    ///
    /// **一次收尾 = 两条回调。** C++ 侧收到一个"带数据和 FIN 的 STREAM 帧"时，
    /// 会先报 `DATA`、再报 `PEER_CLOSED`（`uvcpp_quic_connection.cpp` 的
    /// `ev.on_stream_data` 里写着为什么：`net_read_result` 一次只装一个事件，
    /// 而 QUIC 的帧把两者放在一起）。注意那条 `DATA` 的 `fin` **也是 1** ——
    /// 它是"这块就是最后一块"的信息位，留给 HTTP/3 那种需要当场分帧的调用方，
    /// **不是**"事件报完了"的意思。
    ///
    /// 所以正确的结束判据是 `event == PEER_CLOSED`：它恰好来一次，也是 net 那层
    /// 同一个事件。拿 `fin` 当结束判据的话会**回显两次** —— 这个例子的第一版就是
    /// 那么写的，第二次回显时写方向已经关了，C 侧如实返回了 ngtcp2 的错误码
    /// （-219），异常又逃出了回调，进程直接 abort。三件事叠在一起才暴露出来。
    /// </summary>
    private static void OnServerRead(IntPtr userData, IntPtr conn, long streamId,
                                     ref UvcppNative.uvcpp_c_read_result result)
    {
        try
        {
            if (result.@event == (int)UvcppNative.uvcpp_c_read_event.UVCPP_C_READ_ERROR)
            {
                throw new InvalidOperationException(
                    $"读错误：{result.error} {UvcppErrorInfo.Strerror(result.error)}");
            }

            if (result.@event == (int)UvcppNative.uvcpp_c_read_event.UVCPP_C_READ_DATA)
            {
                if (!SrvBuf.TryGetValue(streamId, out List<byte>? acc))
                {
                    acc = new List<byte>();
                    SrvBuf[streamId] = acc;
                }
                Copy(result.data, result.size, acc);
                return;   // 数据先落地；收尾由下面那条 PEER_CLOSED 管
            }

            // PEER_CLOSED：对端在这条流上发完了（fin）或被 reset（那会走
            // on_stop_sending / READ_ERROR，不在这里）。把攒到的整块回显回去，
            // 并**结束本端的写方向**。
            byte[] body = SrvBuf.TryGetValue(streamId, out List<byte>? got)
                          ? got.ToArray() : Array.Empty<byte>();
            SrvBuf.Remove(streamId);
            Console.WriteLine($"  服务端：流 {streamId} 收到 {body.Length} 字节，回显并 FIN");

            Check(UvcppNative.uvcpp_c_quic_conn_write_stream(
                      conn, streamId, body, (nuint)body.Length, end_stream: 1),
                  "conn_write_stream");
        }
        catch (Exception ex)
        {
            Note("OnServerRead", ex);
        }
    }

    // ==================================================================
    // 客户端：connect（回调里开流、写、然后等回显）
    // ==================================================================
    private static IntPtr BuildClient(int port)
    {
        IntPtr client = UvcppNative.uvcpp_c_quic_client_new();
        if (client == IntPtr.Zero) throw new InvalidOperationException("quic_client_new 返回 NULL");
        _client = client;

        // 不校验对端证书（自签）：传 NULL 就是"不校验"，也是这一层的默认。
        IntPtr tls = UvcppNative.uvcpp_c_quic_tls_client_new(null!);
        if (tls == IntPtr.Zero) throw new InvalidOperationException("客户端 TLS 上下文建不出来");
        _tlsCli = tls;
        Check(UvcppNative.uvcpp_c_quic_client_set_tls(client, tls), "client_set_tls");
        SetAlpn(client, asServer: false);

        // 只有 `_onConnect` 在这里就交出去了，所以只有它必须现在保活 ——
        // 连接的读/关那两枚要到 `OnConnect` 里才装（在那之前没有东西会调它们）。
        _onConnect = OnConnect;
        Check(UvcppNative.uvcpp_c_quic_client_connect(
                  client, "127.0.0.1", port, _onConnect, IntPtr.Zero), "client_connect");
        return client;
    }

    /// <summary>握手完成（status == 0）时调一次；在这里开流并发数据。</summary>
    private static void OnConnect(IntPtr userData, int status)
    {
        try
        {
            _connectStatus = status;
            if (status != 0)
            {
                Console.Error.WriteLine($"  客户端：握手失败 — {status} {UvcppErrorInfo.Strerror(status)}");
                return;
            }

            IntPtr conn = UvcppNative.uvcpp_c_quic_client_connection(_client);

            // 就在这里保活（与 `OnAccept` 同一形状）：委托在装了之后必须一直
            // 有人引用，否则 GC 一收，C 侧那个函数指针就悬垂了。
            _clientOnRead = OnClientRead;
            _clientOnClose = OnClientClose;

            var cbs = new UvcppNative.uvcpp_c_quic_callbacks
            {
                size = (uint)Marshal.SizeOf<UvcppNative.uvcpp_c_quic_callbacks>(),
                on_read = _clientOnRead,
                on_close = _clientOnClose,
            };
            Check(UvcppNative.uvcpp_c_quic_conn_set_callbacks(conn, ref cbs, IntPtr.Zero),
                  "conn_set_callbacks");

            // 双向流（bidi = 1）：本端要能写也能读。流号可以是负数（服务端发起的流），
            // 所以判失败看 `< 0`，不看 `> 0`。
            long sid = UvcppNative.uvcpp_c_quic_conn_open_stream(conn, bidi: 1);
            if (sid < 0)
            {
                throw new InvalidOperationException(
                    $"开流失败：{sid} {UvcppErrorInfo.Strerror((int)sid)}");
            }
            _clientStream = sid;

            // end_stream = 1：这块之后本端的写方向就结束了。服务端据此知道"发完了"。
            Check(UvcppNative.uvcpp_c_quic_conn_write_stream(
                      conn, sid, Payload, (nuint)Payload.Length, end_stream: 1), "write_stream");
            Console.WriteLine($"  客户端：握手完成，流 {sid} 上发了 {Payload.Length} 字节");
        }
        catch (Exception ex)
        {
            Note("OnConnect", ex);
        }
    }

    /// <summary>
    /// 客户端收回显。结束判据同样是 `PEER_CLOSED`（理由见 `OnServerRead` 上面那段）——
    /// 而且这是**唯一**稳妥的写法：空回显时根本不会有 `DATA` 那条回调。
    /// </summary>
    private static void OnClientRead(IntPtr userData, IntPtr conn, long streamId,
                                     ref UvcppNative.uvcpp_c_read_result result)
    {
        try
        {
            if (result.@event == (int)UvcppNative.uvcpp_c_read_event.UVCPP_C_READ_DATA)
            {
                // data 只在本次回调里有效，当场拷走。
                Copy(result.data, result.size, Echo);
                return;
            }
            if (result.@event == (int)UvcppNative.uvcpp_c_read_event.UVCPP_C_READ_ERROR)
            {
                throw new InvalidOperationException(
                    $"读错误：{result.error} {UvcppErrorInfo.Strerror(result.error)}");
            }

            Console.WriteLine($"  客户端：流 {streamId} 收到 {Echo.Count} 字节并收到 PEER_CLOSED");
            _clientDone = true;
        }
        catch (Exception ex)
        {
            Note("OnClientRead", ex);
        }
    }

    private static void OnClientClose(IntPtr userData, IntPtr conn, int errorCode)
    {
        _clientClosed = true;
    }

    // ==================================================================
    // 回环：两端交替泵（同一条线程），跑完比对字节
    // ==================================================================
    private static int SelfTest()
    {
        IntPtr server = BuildServer(port: 0);
        int port = UvcppNative.uvcpp_c_quic_server_configured_port(server);
        IntPtr client = BuildClient(port);

        var sw = Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < TimeoutMs && !_clientDone && _cbError.Length == 0)
        {
            // 各泵一轮（UV_RUN_NOWAIT，不阻塞）。服务端在前：先让它把连接收下来。
            UvcppNative.uvcpp_c_quic_server_run_once(server);
            UvcppNative.uvcpp_c_quic_client_run_once(client);
            Thread.Sleep(1);   // 纯回环上没必要空转
        }

        // 再给两边几轮，让 CONNECTION_CLOSE 的收尾跑完（on_close 才会来）。
        for (int i = 0; i < 50 && _cbError.Length == 0; i++)
        {
            UvcppNative.uvcpp_c_quic_server_run_once(server);
            UvcppNative.uvcpp_c_quic_client_run_once(client);
            Thread.Sleep(1);
        }

        Console.WriteLine($"  协商出的 ALPN：{(string.IsNullOrEmpty(_alpnSeen) ? "(空)" : _alpnSeen)}");
        Console.WriteLine($"  服务端 on_close 来过：{_serverClosed}；客户端 on_close 来过：{_clientClosed}");

        if (_cbError.Length != 0)
        {
            Console.Error.WriteLine($"回环失败（回调里出的错）：{_cbError}");
            return 1;
        }

        bool ok = _connectStatus == 0
                  && _clientDone
                  && Echo.Count == Payload.Length
                  && ByteEq(Echo, Payload);

        if (!ok)
        {
            Console.Error.WriteLine(
                $"回环失败：等待 {sw.ElapsedMilliseconds} ms，收到 {Echo.Count}/{Payload.Length} 字节，" +
                $"握手 status={(_connectStatus == int.MinValue ? "回调没来" : _connectStatus.ToString())}");
            return 1;
        }

        Console.WriteLine($"回环通过：{Echo.Count} 字节逐字节相等（{sw.ElapsedMilliseconds} ms）");
        Cleanup(server, client);
        return 0;
    }

    /// <summary>只当服务端：`_run()` 阻塞，Ctrl+C 结束。</summary>
    private static int ServeForever(int port)
    {
        IntPtr server = BuildServer(port);
        Console.WriteLine("Ctrl+C 结束。");
        Check(UvcppNative.uvcpp_c_quic_server_run(server), "server_run");
        return 0;
    }

    private static void Cleanup(IntPtr server, IntPtr client)
    {
        // 顺序有讲究：**端点在前、TLS 在后**（TLS 的生命周期必须盖住用它建的
        // 那些连接），crypto 那一层不用管（进程要退了，而且它可以被重复释放）。
        // 全都 free 掉之后 `live_handle_count()` 必须回到 0 —— 这是"没有泄漏"
        // 这条判据的量化形式，也是本例子自己带的一条断言。
        Check(UvcppNative.uvcpp_c_quic_client_free(client), "client_free");
        Check(UvcppNative.uvcpp_c_quic_server_free(server), "server_free");
        Check(UvcppNative.uvcpp_c_quic_tls_free(_tlsCli), "tls_free(client)");
        Check(UvcppNative.uvcpp_c_quic_tls_free(_tlsSrv), "tls_free(server)");
        long live = (long)UvcppNative.uvcpp_c_live_handle_count();
        Console.WriteLine($"  收尾后活句柄数：{live}");
        if (live != 0) throw new InvalidOperationException($"收尾后有 {live} 个句柄没回收");
    }

    // ==================================================================
    // 小工具
    // ==================================================================
    /// <summary>装 ALPN 协议名。C 侧**当场拷**进自己的容器（`uvcpp_c_quic.cpp` 里
    /// 那句 `to_protos()`），所以这里的临时内存出了函数就能放。</summary>
    private static void SetAlpn(IntPtr endpoint, bool asServer)
    {
        IntPtr p = Marshal.StringToCoTaskMemUTF8(Alpn);
        try
        {
            var one = new[] { p };
            int rc = asServer
                ? UvcppNative.uvcpp_c_quic_server_set_alpn_protos(endpoint, one, 1)
                : UvcppNative.uvcpp_c_quic_client_set_alpn_protos(endpoint, one, 1);
            Check(rc, "set_alpn_protos");
        }
        finally
        {
            Marshal.FreeCoTaskMem(p);
        }
    }

    /// <summary>把回调里那个"只在此刻有效"的缓冲区拷进托管列表。</summary>
    private static void Copy(IntPtr data, nuint size, List<byte> into)
    {
        int n = checked((int)size);
        if (data == IntPtr.Zero || n <= 0) return;
        byte[] tmp = new byte[n];
        Marshal.Copy(data, tmp, 0, n);
        into.AddRange(tmp);
    }

    private static bool ByteEq(List<byte> a, byte[] b)
    {
        if (a.Count != b.Length) return false;
        for (int i = 0; i < b.Length; i++) if (a[i] != b[i]) return false;
        return true;
    }

    /// <summary>回调里出的错：记下来，别让它穿过 native 帧（文件头第 4 条）。</summary>
    private static void Note(string where, Exception ex)
    {
        if (_cbError.Length == 0) _cbError = $"{where}: {ex.Message}";
    }

    /// <summary>`0 或正数 = 成功、负数 = 失败` —— 这条约定在托管侧的落点。</summary>
    private static void Check(int rc, string what)
    {
        if (rc < 0) throw new UvcppCException(rc, $"{what}: {UvcppErrorInfo.Strerror(rc)}");
    }

    private static bool HasFlag(string[] args, string flag)
    {
        foreach (string a in args) if (a == flag) return true;
        return false;
    }

    private static int ArgPort(string[] args)
    {
        for (int i = 0; i < args.Length - 1; i++)
        {
            if (args[i] == "--port" && int.TryParse(args[i + 1], out int p)) return p;
        }
        return 4433;
    }
}
