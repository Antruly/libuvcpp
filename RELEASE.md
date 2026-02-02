# libuvcpp v1.0.0 Release Notes

## 简介 (Introduction)

**libuvcpp** 是一个基于 libuv 的现代 C++ 封装库，提供简洁的面向对象接口来使用 libuv 的异步 I/O 功能。

## 主要特性 (Key Features)

### Handles（句柄）
- `uvcpp_loop` - 事件循环核心
- `uvcpp_tcp` - TCP 客户端/服务端
- `uvcpp_pipe` - 管道通信（支持 IPC）
- `uvcpp_udp` - UDP 通信
- `uvcpp_tty` - 终端设备
- `uvcpp_poll` - 文件描述符轮询
- `uvcpp_timer` - 定时器
- `uvcpp_signal` - 信号处理
- `uvcpp_fs_event` / `uvcpp_fs_poll` - 文件系统监控
- `uvcpp_async` - 异步通知
- `uvcpp_process` - 进程管理
- `uvcpp_idle` / `uvcpp_prepare` / `uvcpp_check` - 事件循环钩子

### Requests（请求）
- `uvcpp_write` - 写请求
- `uvcpp_connect` - 连接请求
- `uvcpp_shutdown` - 关闭请求
- `uvcpp_fs` - 文件系统操作
- `uvcpp_work` - 工作请求（线程池）
- `uvcpp_getaddrinfo` / `uvcpp_getnameinfo` - DNS 查询
- `uvcpp_udp_send` - UDP 发送请求

### 缓冲区管理 (Buffer Management)
- `uvcpp_buf` - C++ 封装 `uv_buf_t`，提供安全内存管理

### 实用工具 (Utilities)
- 线程、互斥锁、条件变量
- 线程安全的环境变量、用户/组信息
- 目录遍历、文件描述符操作
- 随机数生成、CPU 信息、网络接口

## 构建要求 (Requirements)

- C++17 或更高版本
- CMake 3.16+
- libuv 1.0.0+
- 支持 Windows / Linux / macOS

## 使用示例 (Example)

```cpp
#include "uvcpp/uvcpp.h"
using namespace uvcpp;

int main() {
    uvcpp_loop loop;
    loop.init();

    uvcpp_tcp server(&loop);
    server.bindIpv4("127.0.0.1", 8080);
    
    server.listen([&](uvcpp_stream* s, int status) {
        auto client = new uvcpp_tcp(&loop);
        s->accept(client);
        
        client->read_start(
            [](uvcpp_handle*, size_t, uv_buf_t* buf) {
                uvcpp_buf::alloc_buf(buf, 1024);
            },
            [client](uvcpp_stream*, ssize_t nread, const uv_buf_t* buf) {
                if (nread > 0) {
                    std::cout << "Received: " << std::string(buf->base, nread) << std::endl;
                }
                uvcpp_buf::free_buf(const_cast<uv_buf_t*>(buf));
                if (nread <= 0) {
                    client->close([client](uvcpp_handle*) { delete client; });
                }
            }
        );
    }, 128);

    loop.run(UV_RUN_DEFAULT);
    return 0;
}
```

## 变更日志 (Changelog)

### v1.0.0 (2026-02-02)

**首发版本 (Initial Release)**

- ✅ 所有 14 个功能测试通过
- ✅ 支持 Windows / Linux / macOS
- ✅ 完整的 libuv API C++ 封装
- ✅ 现代 C++ 接口设计
- ✅ 智能内存管理
- ✅ 线程池支持
- ✅ 单元测试覆盖

## 下载 (Download)

- Source code
- prebuilt binaries

## 感谢 (Credits)

感谢所有为这个项目做出贡献的人！

## 许可证 (License)

MIT License

