# -*- coding: utf-8 -*-
"""把「就地调用 std::function 成员」的跳板改成「先拷一份再调用」。

形状（本次处理的全部目标）：

    if (reinterpret_cast<T *>(EXPR)->M)
        reinterpret_cast<T *>(EXPR)->M(<self 表达式>, 其余实参...);

就地调用的危险：使用者在回调里 `delete self` 是**合法用法**
（见 uvcpp_handle::callback_close 的说明），那会把**此刻正在执行**的这个
std::function 连同捕获一起销毁，返回后任何一次重载捕获都读到已释放内存。

句柄回调**会重复触发**（timer/check/idle/prepare 每轮循环都来），所以只能
拷、不能搬 —— 搬走等于第一次之后就再也不触发。一次性请求走
uvcpp_req::invoke_completion（那边是 move）。

首个实参必须是 self 表达式，否则不改写、列为"需手工"（uvcpp_loop::
callback_walk 传的是被遍历的句柄，自动拼实参表只会拼错）。

用法：patch_trampolines.py [--apply] 文件...
不带 --apply 只报告匹配到的处数。
"""
import io
import re
import sys

PAT = re.compile(
    r"if \(reinterpret_cast<(?P<T>\w+)\s*\*>\((?P<E>[A-Za-z_][\w>\-]*)\)->(?P<M>\w+)\)\s*"
    r"reinterpret_cast<(?P=T)\s*\*>\((?P=E)\)\s*->(?P=M)\((?P<A>.*?)\);",
    re.DOTALL,
)

COMMENT = (
    "  // 拷一份再调用：回调里 `delete self` 是合法用法（见 uvcpp_handle::\n"
    "  // callback_close 的说明），就地调用等于在正在执行的闭包上删对象。\n"
    "  // 句柄回调会重复触发，所以是拷不是搬。\n"
)

MANUAL = []


def lead_re(m):
    """首参 self 表达式的匹配式。必须忽略空白：源码里 `<uvcpp_poll*>` 和
    `<uvcpp_poll *>` 都出现过，按字面 startswith 会漏判，于是把 self 表达式
    当"其余实参"再接一遍，生成多一个实参的调用（uvcpp_poll / uvcpp_prepare /
    uvcpp_shutdown 三处即如此）。"""
    return re.compile(
        r"reinterpret_cast\s*<\s*%s\s*\*?\s*>\s*\(\s*%s\s*\)"
        % (re.escape(m.group("T")), re.escape(m.group("E")))
    ).match(m.group("A").strip())


def rewrite(m):
    T, E, M = m.group("T"), m.group("E"), m.group("M")
    lead = lead_re(m)
    rest = m.group("A").strip()[lead.end():].lstrip()
    if rest.startswith(","):
        rest = rest[1:].lstrip()
    call_args = "self" + ((", " + rest) if rest else "")
    return (
        "%s *self = reinterpret_cast<%s *>(%s);\n"
        "  if (self == nullptr) {\n    return;\n  }\n" % (T, T, E)
        + COMMENT
        + "  auto cb = self->%s;\n" % M
        + "  if (cb) {\n    cb(%s);\n  }" % call_args
    )


def process(path, apply_):
    with io.open(path, "r", encoding="utf-8") as f:
        src = f.read()
    out, pos, n = [], 0, 0
    for m in PAT.finditer(src):
        if not lead_re(m):
            MANUAL.append(
                "  %s:%d  %s" % (path, src[:m.start()].count("\n") + 1,
                                 " ".join(m.group("A").split()))
            )
            continue
        out.append(src[pos:m.start()])
        out.append(rewrite(m))
        pos = m.end()
        n += 1
    if n and apply_:
        out.append(src[pos:])
        with io.open(path, "w", encoding="utf-8", newline="") as f:
            f.write("".join(out))
    return n


def main():
    apply_ = "--apply" in sys.argv
    files = [a for a in sys.argv[1:] if a != "--apply"]
    total = 0
    for path in files:
        n = process(path, apply_)
        print("%-46s %d" % (path, n))
        total += n
    if MANUAL:
        print("\n首参不是 self 表达式，需手工改写：")
        for line in MANUAL:
            print(line)
    print("合计 %d 处%s" % (total, "" if apply_ else "（演练，未写盘）"))


main()
