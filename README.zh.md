<!-- badges -->
[![GitHub release](https://img.shields.io/badge/release-1.0.0-blue.svg)](./)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)
[![CI](https://img.shields.io/badge/ci-GitHub%20Actions-blue)](.github/workflows/ci.yml)

# libuvcpp

🔧 libuv 的 C++ 封装 — 轻量、头文件友好的 libuv 封装库。

- 版本：`1.0.0` — 作者：`zhuweiye` — 许可证：`MIT`

- **语言**： [English](./README.md) · [中文](./README.zh.md)

简介
----

libuvcpp 的核心目标是：在保持 libuv 性能与原有语义的前提下，提高在 C++ 环境下的开发效率。库把 libuv 的基本类型整理并封装为对应的 C++ 类，将相关方法合并到合适的类中，使得日常开发更便捷、可读性更高。

设计思想
--------

- 面向对象：采用继承与多态，利用 C++ 特性提供更自然的 API。  
- 不破坏原有语义：保留 libuv 的调用流程与性能特性。  
- 模块化：主要分为 `handle` 与 `req` 两大类，另有 `uvcpp` 辅助工具集合。  
- 兼容性：面向 libuv v1.x.x 系列，兼容多数常见平台。

安装与构建
----------

先决条件：
- CMake（推荐 >= 3.16）。  
- Windows 上需安装 Visual Studio（见下面示例）。

注意：
- 项目自带 CMake 配置，会在缺少 libuv 时自动从源码拉取并构建，无需手动安装 libuv。

构建示例：

- Linux / macOS：

```bash
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
ctest --output-on-failure
```

- Windows x64（Visual Studio 2022）：

```powershell
mkdir build
cmake -S . -B build -A x64 -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -- /m
```

- Windows x86（Visual Studio 2013）：

```powershell
mkdir build
cmake -S . -B build -A Win32 -G "Visual Studio 12 2013" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -- /m
```

快速开始示例
------------

打印版本并运行默认循环：

```cpp
#include "uvcpp.h"
#include <iostream>

int main() {
  std::cout << "libuvcpp version: " << UVCPP_VERSION_STRING << std::endl;
  uvcpp::uvcpp_loop loop;
  loop.init();
  loop.run();
  return 0;
}
```

常用示例（计时器 / 空闲回调）
--------------------------

- Timer（单次或重复计时）：

```cpp
#include "uvcpp.h"
#include <iostream>

int main() {
  uvcpp::uvcpp_loop loop;
  loop.init();

  uvcpp::uvcpp_timer timer(&loop);
  timer.start([](uvcpp::uvcpp_timer* t){
    std::cout << "timer fired\n";
    t->stop();
  }, 1000, 0);

  loop.run();
  return 0;
}
```

- Idle（循环空闲时调用）：

```cpp
#include "uvcpp.h"
#include <iostream>

int main() {
  uvcpp::uvcpp_loop loop;
  loop.init();

  uvcpp::uvcpp_idle idle(&loop);
  idle.start([](uvcpp::uvcpp_idle* i){
    static int count = 0;
    std::cout << "idle callback: " << ++count << std::endl;
    if (count >= 5) {
      i->stop();
    }
  });

  loop.run();
  return 0;
}
```

更多信息
--------

- 查看 `src/handle` 与 `src/req` 下的头文件以获取完整 API 说明。  
- 仓库中的 `tests/functional` 提供了多个功能测试示例，推荐参考。

贡献
----

欢迎贡献 — 请提交 issue 或 PR，保持修改小而专注并遵循现有代码风格。

License
-------

本项目采用 MIT 许可证 — 详情见 `LICENSE`。


