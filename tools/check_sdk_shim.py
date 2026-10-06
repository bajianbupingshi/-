#!/usr/bin/env python3
"""Neuron SDK 垫片漂移检查。

为什么要这个脚本：垫片（plugins/driver/sdk_shim）是为了「在没有 Neuron 环境的机器上
也能编译并驱动插件」而写的。但它有个致命的自证盲区 —— **如果垫片把常量写错了，
插件的自检两侧用的是同一个错值，照样全绿**。本项目实测踩过三次：

  · NEU_NA_TYPE_DRIVER    垫片 0 / 真实 1
  · NEU_PLUGIN_KIND_SYSTEM 垫片 0 / 真实 1
  · NEU_EVENT_TIMER_ALWAYS / NEU_TAG_CACHE_TYPE_NONE  —— 真实 SDK 里**根本没有这两个常量**

前两条会让插件在真 Neuron 里被归错类型；第三条会让 plugin_module.c 对着真头文件
**直接编译失败**。而「垫片自检 22/22 通过」完全没提示这些问题。

所以：拿真实头文件当基准，逐常量比对。

用法：
  python tools/check_sdk_shim.py --real <neuron>/include/neuron \
                                 --shim plugins/driver/sdk_shim/neuron

退出码：0 = 一致；1 = 有漂移（详见输出）。
"""
import argparse
import os
import re
import sys

CONST_RE = re.compile(r"\bNEU_[A-Z0-9_]+\b")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def parse_constants(root: str) -> dict:
    """提取 root 下所有头文件里的 NEU_* 常量名 -> 值。

    只处理枚举体与 `#define NAME 数字`。枚举里没写 `= n` 的按顺序自增。
    """
    out = {}
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            if not name.endswith((".h", ".hpp")):
                continue
            path = os.path.join(dirpath, name)
            try:
                raw = open(path, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            txt = strip_comments(raw)

            for m in re.finditer(r"^\s*#define\s+(NEU_[A-Z0-9_]+)\s+([0-9]+)\s*$", txt, re.M):
                out[m.group(1)] = int(m.group(2))

            for em in re.finditer(r"\benum\b[^{]*\{(.*?)\}", txt, re.S):
                body = em.group(1)
                if "{" in body:  # 嵌套（union 之类）跳过，避免解析错位
                    continue
                running = -1
                for item in body.split(","):
                    item = item.strip()
                    if not item:
                        continue
                    if "=" in item:
                        nm, _, val = item.partition("=")
                        nm, val = nm.strip(), val.strip()
                        try:
                            running = int(val, 0)
                        except ValueError:
                            continue
                    else:
                        nm = item
                        running += 1
                    if re.fullmatch(r"NEU_[A-Z0-9_]+", nm):
                        out[nm] = running
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--real", required=True, help="真实 Neuron SDK 的 include/neuron 目录")
    ap.add_argument("--shim", required=True, help="垫片目录（应与真实头文件同名同层）")
    ap.add_argument("--used-by", default="", help="可选：只报告这些文件里用到的常量（逗号分隔）")
    args = ap.parse_args()

    if not os.path.isdir(args.real):
        print(f"真实 SDK 目录不存在：{args.real}")
        return 2
    if not os.path.isdir(args.shim):
        print(f"垫片目录不存在：{args.shim}")
        return 2

    real = parse_constants(args.real)
    shim = parse_constants(args.shim)

    print(f"真实 SDK 常量 {len(real)} 个 / 垫片常量 {len(shim)} 个")

    used = set()
    if args.used_by:
        for f in args.used_by.split(","):
            f = f.strip()
            if os.path.isfile(f):
                used |= set(CONST_RE.findall(strip_comments(open(f, encoding="utf-8").read())))

    def interesting(n):
        return (not used) or (n in used)

    mismatch, missing, invented = [], [], []
    for name, val in sorted(shim.items()):
        if not interesting(name):
            continue
        if name not in real:
            invented.append(name)
        elif real[name] != val:
            mismatch.append((name, val, real[name]))

    if used:
        for name in sorted(used):
            if name.startswith("NEU_") and name not in shim and name in real:
                missing.append(name)

    ok = True
    if mismatch:
        ok = False
        print("\n✘ 取值不一致（垫片 / 真实）：")
        for name, sv, rv in mismatch:
            print(f"    {name:38s} 垫片={sv}  真实={rv}")
    if invented:
        ok = False
        print("\n✘ 垫片里有、真实 SDK 里没有的常量（对着真头文件会编译失败）：")
        for name in invented:
            print(f"    {name}")
    if missing:
        ok = False
        print("\n✘ 代码用到、但垫片没定义（垫片不完整）：")
        for name in missing:
            print(f"    {name}")

    if ok:
        print("\n✔ 垫片常量与真实 SDK 一致" + ("（已按 used-by 过滤）" if used else ""))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
