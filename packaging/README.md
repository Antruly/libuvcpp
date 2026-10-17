# packaging

发行版包（`.deb` / `.rpm`）、Windows 安装程序（`.exe`），以及把前两者发布出去的两条
自建仓库（apt / dnf）。三个子目录互不依赖，各自有入口脚本：

| 目录 | 入口 | 产出 |
|---|---|---|
| `nfpm/` | `make_packages.py` | `libuvcpp1_*.deb`、`libuvcpp-dev_*.deb`、`libuvcpp1-*.rpm`、`libuvcpp-devel-*.rpm` |
| `windows/` | `libuvcpp.iss`（Inno Setup 6.3+） | `libuvcpp-<ver>-msvc-<arch>-setup.exe` |
| `repo/` | `publish.py` | gh-pages 树：`apt/` + `rpm/` + `index.html` |

CI 里由 `.github/workflows/release.yml` 按顺序调用；本文只讲**为什么是现在这个形状**，
以及在本机怎么单独跑。

## 两条产物线，两种链接方式（这一节是全部设计的前提）

同一个库有两种发法，**它们的依赖形态不一样，不能互换**：

| | 发布 zip（`dist/libuvcpp-<ver>-<leg>.zip`） | 发行版包（`.deb` / `.rpm`） |
|---|---|---|
| libuv / zlib | **静态编进** `libuvcpp.so`，不依赖系统包 | **链系统的** `libuv.so.1` / `libz.so.1` |
| OpenSSL / brotli / llhttp / nghttp2 / ngtcp2 / nghttp3 | 静态编入 | 同左（公开头不暴露它们的类型，而且能避开 `libssl3` 换名的版本地狱） |
| MySQL / PostgreSQL / ODBC | `UVCPP_DB_RUNTIME_LOAD=ON`，运行时 `dlopen` | 同左 |
| 文件名 | 单个 `libuvcpp.so` | `libuvcpp.so.<ver>` → `.so.<major>` → `.so` |
| 构建开关 | （默认） | `-DUVCPP_SET_SOVERSION=ON` |

**为什么包必须链系统 libuv**：公开头里有 `<uv.h>`（`src/net/`、`src/grpc/`），
`uvcpp_loop` 包的也是 `uv_loop_t`。使用者照 `libuv1-dev` 的头编译、调的是系统 libuv，
而库里若还静态带着另一份 libuv 的实现，同一个进程里就有两个 libuv 实例（两份全局状态、
两份 `uv_loop_t` 布局的假设）—— 这是真实的内存破坏风险，不是体积问题。

**为什么 SONAME 只给包这棵树**：`tests/tools/package_release.py` 直接
`shutil.copy2` 那个 `libuvcpp.so`，`check_config_contract.py` 也按不带版本号的名字查。
把 SONAME 变成默认行为会同时改掉发布 zip 的形状，所以它是 `UVCPP_SET_SOVERSION`，
默认 `OFF`，只有包这棵树打开。

**已知的 API 差集（libuv 版本闸门）**：包是对着**构建机自带的 libuv 头**编的
（CI 的 `linux-x64` 钉 Ubuntu 22.04 ⇒ **libuv 1.43.0**：这条不是抄文档，1.7.4 的
`libuvcpp1` 包自己写着 `Depends: libuv1 (>= 1.43.0)`），而 zip 对着仓里钉的 libuv 编。
`uv_available_parallelism` 是 1.44 才有的，本库对 1.43 有亲和性兜底实现。
本库有几处 `#if UV_VERSION_MINOR >= N` 的公开闸门，其中 `N = 45` 的那批在
1.43 上整段编不进去：

| 门槛 | 涉及 |
|---|---|
| ≥ 1.45 | `uvcpp_metrics`（整个类）、`uvcpp_group`（整个类）、`uvcpp_passwd::get_os_passwd(uv_uid_t)` 重载 |

于是 24.04（libuv 1.48）上的使用者会看到**头文件里有声明、`.so` 里没有符号** ——
闸门读的是**他们**系统的 `uv.h`。用到那三样时是链接期 `undefined reference`，
不是运行期的静默错。要它们就用发布 zip（自带 1.5x 的 libuv），或者等包改成按
发行版分套构建。反过来选 1.45+ 的构建机会把 glibc 底线从 2.35 抬到 2.39，
22.04 与 Debian 12 就装不上了 —— 现在的取舍是覆盖面优先。

## `nfpm/`：.deb 与 .rpm

```sh
# 先把包那棵树装到前缀里（功能开关与发布腿一致，走的却是系统 libuv/zlib）
cmake -S . -B build-pkg -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DUVCPP_BUILD_LIBUV_FROM_SOURCE=OFF -DUVCPP_USE_SYSTEM_LIBUV=ON \
      -DUVCPP_SET_SOVERSION=ON [其余模块开关同 release.yml]
cmake --build build-pkg
cmake --install build-pkg --prefix "$PWD/stage/usr"

python3 packaging/nfpm/make_packages.py --stage "$PWD/stage/usr" --out dist
```

两点值得知道：

* **nfpm 是钉版本 + sha256 下载的**（`nfpm.pin`），缓存到
  `~/.cache/libuvcpp-nfpm/<ver>/nfpm`。要换版本就改 `nfpm.pin` 的两个哈希，
  不要改成"取 latest"—— 打包工具自己漂走会让同一条 tag 重打出不同的包。
  `$NFPM` 环境变量可覆盖（本机已有二进制时）。
* **stage 里属于系统包的头会被剪掉**：`include/nlohmann/`、`include/libuv/`、
  `zlib.h`、`zconf.h`。这些由 `Depends`/`Requires` 里的开发包提供；两边都装
  dpkg 会以文件冲突直接拒绝。剪在打包脚本里、不改 CMake 的 install 规则，
  是因为源码安装（`cmake --install`）仍然应该得到一棵自己能编的树。
* 依赖名在两种格式里**不一样**（`libuv1-dev` vs `libuv-devel`），版本约束语法也不同
  （deb 是 `foo (= 1.2.3)`，rpm 是 `foo = 1.2.3`；写成 deb 那套 nfpm 会报
  `malformed relation`）。所以 `specs_for()` 按格式分开。

## `windows/`：Inno Setup

```powershell
ISCC.exe /DMyVersion=1.7.4 /DMyArch=x64 `
         /DStageDir=D:\...\dist\libuvcpp-1.7.4-msvc-x64 `
         /DOutputDir=D:\...\dist `
         packaging\windows\libuvcpp.iss
```

* 文件清单**不在 `.iss` 里重列**，整棵 stage 打进去。那份 stage 已经过
  `package_release.py` 的自检（导入表覆盖、调试 CRT 泄漏、版本号一致性），
  再抄一遍就是第二个真相来源。
* `AppId` **永远不能改**：改了之后新版本会被当成另一个产品并存安装，旧 dll 留在
  `Program Files` 里被 PATH 先找到，症状是"升级完还是旧库"。
* PATH 写回用 `expandsz`（`Path` 是 `REG_EXPAND_SZ`，写成普通字符串会把别人的
  `%SystemRoot%` 变成死文本），卸载时**逐段**摘掉自己那一段 —— 不用
  `StringReplace`，那会把 `C:\x\bin2` 这种前缀相同的邻居一起改坏。
* 签名由 `/DSignSetup=1` + `/Ssigntool=...` 打开；没给就不签、安装包照出
  （缺证书是配置问题，不是构建失败）。
* arm64 需要 Inno Setup **6.3+**（`ArchitecturesAllowed=arm64` 到那时才有）。

## `repo/`：gh-pages 上的 apt / dnf 仓库

```sh
python3 packaging/repo/publish.py --site /path/to/gh-pages-checkout \
    --packages dist --pages-url https://antruly.github.io/libuvcpp --gpg-key "$GPG_KEY_ID"
```

`--site` 是**上一次的 gh-pages 检出**：仓库增量维护（旧版本的包要留着），索引按整棵
目录重算，所以同一个脚本既能首建也能发新版。每包每架构只留最近 `--keep`（默认 5）
个版本 —— GitHub Pages 站点上限 1 GB。

签名形态：

| 渠道 | 签什么 | 客户端从哪读 |
|---|---|---|
| apt | `Release` → `InRelease`（clearsign）+ `Release.gpg` | `signed-by=` 指定的 `apt/key.asc` |
| dnf | `repodata/repomd.xml` → `repomd.xml.asc` | `.repo` 里的 `gpgkey=` |

**`.rpm` 文件本身不签**，所以 `libuvcpp.repo` 里是 `gpgcheck=0` +
`repo_gpgcheck=1`：元数据签名照样校验，包被换掉时 dnf 会因为 repomd 里的校验和不符
而失败。要改成逐包签名，签名必须发生在**上传 Release 之前** —— 否则同一个版本的
`.rpm` 在 Release 资产与仓库里 sha256 不同，两边总有一边对不上。

两层防线，别混淆：

* `publish.py` 自己缺密钥就**直接失败**（`--no-sign` 之外没有旁路），本地调试用它
  才会产出未签名仓库，而且会大声警告；
* CI 的 `publish-repo` 在**调用脚本之前**先看 `GPG_PRIVATE_KEY` 在不在，不在就整个
  job 跳过并 `::warning`（job 仍绿）。安装包与 zip 照发，apt/dnf 通道在密钥就位前
  不存在 —— 配上以后下一次真发布自动建出 `gh-pages`。

### 首次发布前的一次性准备

1. **建签名密钥**（私钥只进 GitHub Secrets，不进仓库）：

   ```sh
   # 必须 %no-protection —— 别用 `--quick-generate-key`。它在 batch 下生成的是
   # "空口令、但仍带 S2K 保护"的密钥：本机 gpg 会拿空口令试一次就签了，runner 的
   # gpg-agent 却老老实实要 pinentry，报
   # `gpg: signing failed: Inappropriate ioctl for device`，而 `--passphrase ''`
   # 在 batch 下被当成"没给口令"（`No passphrase given`）。2026-10-18 踩过。
   cat > /tmp/uvcpp-keyparams <<'EOF'
   %no-protection
   Key-Type: RSA
   Key-Length: 3072
   Key-Usage: sign
   Name-Real: libuvcpp repository
   Name-Email: antruly@users.noreply.github.com
   Expire-Date: 0
   %commit
   EOF
   gpg --batch --generate-key /tmp/uvcpp-keyparams
   gpg --list-secret-keys --with-colons <邮箱>        # 记下 fpr
   gpg --batch --armor --export-secret-keys <fpr>     # → secret GPG_PRIVATE_KEY
   gpg --batch --armor --export <fpr>                 # → 仓库里的 key.asc（脚本自动导出）
   gpg --list-packets <导出的 .asc> | grep -E 'protect|s2k'   # 必须一条都搜不到
   ```

   两个 secret：`GPG_PRIVATE_KEY`（ASCII-armored 私钥）与 `GPG_KEY_ID`（指纹）。
   `publish-repo` 的「Import the signing key」会当场盲签一次，密钥带保护就直接红，
   不用等到建仓库树那一步才现形。

   吊销证书在 `openpgp-revocs.d/<fpr>.rev`，**与私钥一起备份**（丢了就再也作废不了
   这把 key）；备份别只放 GitHub —— Secrets 是只写不读的。

2. **Windows 证书**：PFX 转 base64 存 `WINDOWS_CERT_PFX_BASE64`，
   密码存 `WINDOWS_CERT_PASSWORD`。两个都缺就跳过签名并 `::warning`。

3. **GitHub Pages**：第一次 `publish-repo` 会 push 一个 `gh-pages` 分支；之后在
   Settings → Pages 里把来源选成那个分支（或 `gh api` 设一次）。在此之前
   `https://antruly.github.io/libuvcpp/` 返回 404，`publish-repo` 本身照样绿。

4. 密钥丢了就等于信任链断了：老用户装过的仓库配置会开始报 `NO_PUBKEY`。
   轮换要发新 key 并让人重导 —— 不在脚本能力范围内，别把私钥只放在一台机器上。
