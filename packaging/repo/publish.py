#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 .deb / .rpm 摆成一个可以用 apt / dnf 直接装的仓库树。

输出就是 gh-pages 分支的内容（根目录另有 `index.html`）：

    apt/pool/main/l/libuvcpp/*.deb
    apt/dists/stable/InRelease, Release, Release.gpg
    apt/dists/stable/main/binary-{amd64,arm64}/Packages{,.gz}
    apt/key.asc
    rpm/{x86_64,aarch64}/*.rpm
    rpm/repodata/*（createrepo_c 产）
    rpm/key.asc
    rpm/libuvcpp.repo

用法（CI 里由 `publish-repo` job 调）：
    python3 packaging/repo/publish.py --site _site --packages dist \\
        --pages-url https://antruly.github.io/libuvcpp --gpg-key <KEYID>

设计上的几条：

* **仓库是增量维护的**：`--site` 是上一次的 gh-pages 检出，新包只是加进去，
  索引按整棵目录重算。所以同一个脚本既能首建也能发新版。
* **只留最近 `--keep` 个版本**（按包名×架构）：GitHub Pages 站点上限 1 GB，
  每个版本两包×两架构，攒几十版就会顶到。
* **apt 用 InRelease/Release.gpg 签元数据**，这是 Debian 侧信任的锚点，包本身
  不另签（Debian 也不用 debsigs）。**dnf 侧签 `repomd.xml`**：`.rpm` 文件本身
  不签，所以 `.repo` 里是 `gpgcheck=0` + `repo_gpgcheck=1`；要改成逐包签名，
  得在**上传 Release 之前**签，否则同一个版本在两处的 sha256 会不一样。
* 缺 GPG 默认**直接失败**，不产出未签名仓库。本地调试用 `--no-sign`（会大声抱怨）。
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

DEB_RE = re.compile(r"^(?P<name>[^_]+)_(?P<ver>[^_]+)_(?P<arch>[^_]+)\.deb$")
RPM_RE = re.compile(r"^(?P<name>.+)-(?P<ver>\d[^-]*)-(?P<rel>[^-]+)\.(?P<arch>[^.]+)\.rpm$")

DEB_ARCHES = ("amd64", "arm64")
RPM_ARCHES = ("x86_64", "aarch64")


def die(msg):
    sys.stderr.write("错误：%s\n" % msg)
    raise SystemExit(2)


def run(cmd, cwd=None, stdin=None):
    subprocess.check_call(cmd, cwd=cwd, stdin=stdin)


def vkey(v):
    """把 `1.7.3-1` 这类版本排成可比较的键。只取数字段，非数字段按字符串比。"""
    key = []
    for part in re.split(r"[.\-+]", v):
        if part.isdigit():
            key.append((1, int(part), ""))
        else:
            key.append((0, 0, part))
    return key


def copy_packages(pkg_dir, site):
    """把新包拷进仓库的固定位置，返回 (deb 数, rpm 数)。"""
    pool = os.path.join(site, "apt", "pool", "main", "l", "libuvcpp")
    os.makedirs(pool, exist_ok=True)
    n_deb = n_rpm = 0
    for name in sorted(os.listdir(pkg_dir)):
        src = os.path.join(pkg_dir, name)
        if not os.path.isfile(src):
            continue
        if DEB_RE.match(name):
            shutil.copy2(src, os.path.join(pool, name))
            n_deb += 1
        elif RPM_RE.match(name):
            arch = RPM_RE.match(name).group("arch")
            if arch not in RPM_ARCHES:
                die("%s 的架构 %r 不认识（只认 %s）"
                    % (name, arch, ", ".join(RPM_ARCHES)))
            d = os.path.join(site, "rpm", arch)
            os.makedirs(d, exist_ok=True)
            shutil.copy2(src, os.path.join(d, name))
            n_rpm += 1
    return n_deb, n_rpm


def prune(directory, pattern, keep):
    """每个 (包名, 架构) 只留 version 最大的 keep 个文件。"""
    groups = {}
    for name in os.listdir(directory):
        m = pattern.match(name)
        if not m:
            continue
        groups.setdefault((m.group("name"), m.group("arch")), []).append(
            (m.group("ver"), name))
    removed = 0
    for (pkg, arch), items in groups.items():
        items.sort(key=lambda t: vkey(t[0]), reverse=True)
        for _v, name in items[keep:]:
            os.unlink(os.path.join(directory, name))
            removed += 1
            print("  删除旧包 %s（%s/%s 只留 %d 个版本）" % (name, pkg, arch, keep))
    return removed


def split_stanzas(text):
    out, cur = [], []
    for line in text.splitlines():
        if line.strip() == "":
            if cur:
                out.append("\n".join(cur))
                cur = []
        else:
            cur.append(line)
    if cur:
        out.append("\n".join(cur))
    return out


def apt_metadata(site, gpg, key):
    apt = os.path.join(site, "apt")
    dist = os.path.join(apt, "dists", "stable")

    raw = subprocess.check_output(["apt-ftparchive", "packages", "pool"],
                                  cwd=apt, text=True)
    stanzas = split_stanzas(raw)

    # apt-ftparchive 会把所有架构的包放进同一份输出；按 Architecture 分拣。
    # `all` 的包两个索引里都要有。
    by_arch = {a: [] for a in DEB_ARCHES}
    for st in stanzas:
        m = re.search(r"^Architecture:\s*(\S+)\s*$", st, re.M)
        if not m:
            die("apt-ftparchive 的输出里有一段没有 Architecture：\n%s" % st[:200])
        arch = m.group(1)
        if arch == "all":
            for a in DEB_ARCHES:
                by_arch[a].append(st)
        elif arch in by_arch:
            by_arch[arch].append(st)
        else:
            die("不认识的 deb 架构 %r" % arch)

    present = []
    for arch in DEB_ARCHES:
        d = os.path.join(dist, "main", "binary-" + arch)
        os.makedirs(d, exist_ok=True)
        body = "".join(st + "\n\n" for st in by_arch[arch])
        p = os.path.join(d, "Packages")
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(body)
        # -n：不写时间戳，同样的输入得到同样的字节（Pages 上不会每次全变）。
        run(["gzip", "-9", "-n", "-k", "-f", p])
        if by_arch[arch]:
            present.append(arch)
        print("  Packages[%s]：%d 个包" % (arch, len(by_arch[arch])))

    if not present:
        die("一个 .deb 都没有，不生成空仓库")

    conf = os.path.join(apt, "release.conf")
    with open(conf, "w", encoding="utf-8", newline="\n") as f:
        f.write('APT::FTPArchive::Release::Origin "libuvcpp";\n')
        f.write('APT::FTPArchive::Release::Label "libuvcpp";\n')
        f.write('APT::FTPArchive::Release::Suite "stable";\n')
        f.write('APT::FTPArchive::Release::Codename "stable";\n')
        f.write('APT::FTPArchive::Release::Architectures "%s";\n'
                % " ".join(present))
        f.write('APT::FTPArchive::Release::Components "main";\n')
        f.write('APT::FTPArchive::Release::Description '
                '"libuvcpp — Modern C++ wrapper for libuv";\n')
    # conf 只在这一步用，别留在站点里（Pages 会把它当文件发出去）。
    try:
        rel = subprocess.check_output(
            ["apt-ftparchive", "-c", conf, "release", "dists/stable"],
            cwd=apt, text=True)
    finally:
        os.unlink(conf)
    with open(os.path.join(dist, "Release"), "w", encoding="utf-8", newline="\n") as f:
        f.write(rel)

    if gpg:
        sign_clear(gpg, key, os.path.join(dist, "Release"),
                   os.path.join(dist, "InRelease"))
        sign_detach(gpg, key, os.path.join(dist, "Release"),
                    os.path.join(dist, "Release.gpg"))


def rpm_metadata(site, gpg, key):
    tool = shutil.which("createrepo_c") or shutil.which("createrepo")
    if not tool:
        die("没有 createrepo_c（Ubuntu 上是 `apt-get install createrepo-c`）")
    for arch in RPM_ARCHES:
        d = os.path.join(site, "rpm", arch)
        if not os.path.isdir(d) or not any(n.endswith(".rpm") for n in os.listdir(d)):
            continue
        # gz 是 dnf 兼容性最好的形式（新版本默认 zstd，老发行版读不了）。
        run([tool, "--quiet", "--general-compress-type=gz", "."], cwd=d)
        print("  repodata[%s] 已生成" % arch)
        if gpg:
            sign_detach(gpg, key, os.path.join(d, "repodata", "repomd.xml"),
                        os.path.join(d, "repodata", "repomd.xml.asc"))


def sign_clear(gpg, key, src, dst):
    run([gpg, "--batch", "--yes", "--digest-algo", "SHA256",
         "--local-user", key, "--clearsign", "--output", dst, src])


def sign_detach(gpg, key, src, dst):
    run([gpg, "--batch", "--yes", "--digest-algo", "SHA256",
         "--local-user", key, "--armor", "--detach-sign",
         "--output", dst, src])


def export_key(gpg, key, site):
    blob = subprocess.check_output(
        [gpg, "--batch", "--armor", "--export", key])
    if not blob.strip():
        die("导不出公钥 —— --gpg-key %s 在本机 keyring 里吗？" % key)
    for p in (os.path.join(site, "apt", "key.asc"),
              os.path.join(site, "rpm", "key.asc")):
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(blob)


REPO_FILE = """\
[libuvcpp]
name=libuvcpp - Modern C++ wrapper for libuv
baseurl={url}/rpm/$basearch
enabled=1
# .rpm 本身没有逐包签名（签名链在 repomd.xml 上），所以 gpgcheck=0；
# 元数据签名照样校验，包被换掉时会因为 repomd 的校验和不符而失败。
gpgcheck=0
repo_gpgcheck=1
gpgkey={url}/rpm/key.asc
"""

COMMON_STEPS = """\
# libuvcpp — Modern C++ wrapper for libuv

## Debian / Ubuntu

    sudo install -d -m 0755 /etc/apt/keyrings
    curl -fsSL {url}/apt/key.asc |
      sudo tee /etc/apt/keyrings/libuvcpp.asc >/dev/null
    echo "deb [signed-by=/etc/apt/keyrings/libuvcpp.asc] {url}/apt stable main" |
      sudo tee /etc/apt/sources.list.d/libuvcpp.list
    sudo apt update
    sudo apt install libuvcpp-dev

## Fedora / RHEL / openSUSE（dnf / zypper）

    sudo curl -fsSL -o /etc/yum.repos.d/libuvcpp.repo {url}/rpm/libuvcpp.repo
    sudo dnf install libuvcpp-devel

## 文件

- `apt/` — Debian 仓库（`InRelease` 已签名）
- `rpm/` — RPM 仓库（`repomd.xml.asc`）
- 源码与发布包：<https://github.com/Antruly/libuvcpp>

## 注意

这些包链接**系统的** `libuv` 与 `zlib`；GitHub Release 里的 zip 是自包含的，
两者不要混用（同一个进程里出现两份 libuv 是内存破坏风险）。
"""


def write_index(site, url):
    p = os.path.join(site, "index.html")
    body = COMMON_STEPS.format(url=url)
    # 极简 HTML：这段文字同时是给 `curl` 看的最短说明，页面只用 <pre> 保格式。
    esc = (body.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))
    html = ("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>libuvcpp packages</title></head>\n"
            "<body><pre>%s</pre></body></html>\n" % esc)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(html)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--site", required=True, help="gh-pages 检出的根目录")
    ap.add_argument("--packages", required=True, help="放着新 .deb/.rpm 的目录")
    ap.add_argument("--pages-url", default="https://antruly.github.io/libuvcpp")
    ap.add_argument("--gpg-key", default=os.environ.get("GPG_KEY_ID"),
                    help="签名用的 key id / 指纹（或环境变量 GPG_KEY_ID）")
    ap.add_argument("--gpg-homedir", default=None,
                    help="给 gpg 的 --homedir（本地用临时 GNUPGHOME）")
    ap.add_argument("--keep", type=int, default=5, help="每个包每架构保留几个版本")
    ap.add_argument("--no-sign", action="store_true",
                    help="本地调试用：不签元数据（**不要**用它发布）")
    args = ap.parse_args()

    site = os.path.abspath(args.site)
    pkgs = os.path.abspath(args.packages)
    if not os.path.isdir(pkgs):
        die("--packages %s 不存在" % pkgs)
    os.makedirs(site, exist_ok=True)

    gpg = None
    key = args.gpg_key
    if not args.no_sign:
        gpg = shutil.which("gpg")
        if not gpg:
            die("没有 gpg：不发布未签名的仓库（本地调试请显式 --no-sign）")
        if not key:
            die("没给 --gpg-key / GPG_KEY_ID：不发布未签名的仓库")
        # 私钥必须在场；`--list-secret-keys` 对不存在的 id 返回 0 但输出空，
        # 所以要自己判空，别等签的时候才炸。
        out = subprocess.run(
            [gpg, "--batch", "--list-secret-keys", "--with-colons", key],
            stdout=subprocess.PIPE, universal_newlines=True).stdout
        if "sec:" not in out:
            die("keyring 里没有 %s 的私钥" % key)
    else:
        sys.stderr.write("**警告：--no-sign，产出的是未签名仓库，不能发布**\n")

    n_deb, n_rpm = copy_packages(pkgs, site)
    print("新包：%d 个 .deb，%d 个 .rpm" % (n_deb, n_rpm))
    if n_deb == 0 and n_rpm == 0:
        die("--packages 里没有可识别的 .deb/.rpm")

    pool = os.path.join(site, "apt", "pool", "main", "l", "libuvcpp")
    prune(pool, DEB_RE, args.keep)
    for arch in RPM_ARCHES:
        d = os.path.join(site, "rpm", arch)
        if os.path.isdir(d):
            prune(d, RPM_RE, args.keep)

    print("apt 元数据：")
    apt_metadata(site, gpg if not args.no_sign else None, key)

    print("rpm 元数据：")
    rpm_metadata(site, gpg if not args.no_sign else None, key)

    if gpg:
        export_key(gpg, key, site)
    os.makedirs(os.path.join(site, "rpm"), exist_ok=True)
    with open(os.path.join(site, "rpm", "libuvcpp.repo"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(REPO_FILE.format(url=args.pages_url.rstrip("/")))
    write_index(site, args.pages_url.rstrip("/"))
    print("\n仓库树已写到 %s" % site)


if __name__ == "__main__":
    main()
