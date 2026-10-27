#!/usr/bin/env python3
"""Regenerate the consolidated pitfall index (规划/v3.1/02-...).

Source of truth is HANDOFF.md §6 (the numbered list). This script only groups
and trims; it never rewrites the wording, so every row still maps 1:1 to a
HANDOFF number.
"""

import argparse
import re

THEMES = [
    ("解码 / 格式 / 色彩", ["解码", "格式", "WIC", "FreeImage", "LibRaw", "JPEG", "PNG", "RAW",
                          "HEIF", "AVIF", "PSD", "SVG", "色彩", "ICC", "CMYK", "EXIF", "旋转", "方向"]),
    ("缓存 / 预取 / 盘", ["缓存", "预取", "LRU", "磁盘", "disk", "cache", "缩略图缓存"]),
    ("渲染 / 绘制 / 动画", ["D2D", "D3D", "渲染", "绘制", "动画", "帧", "位图", "纹理", "交换链", "Present"]),
    ("窗口 / DPI / 布局 / 命中", ["窗口", "DPI", "布局", "命中", "菜单", "全屏", "标题栏", "浮层", "overlay"]),
    ("线程 / 生命周期 / 句柄", ["线程", "退出", "关闭", "句柄", "泄漏", "崩溃", "join", "单实例"]),
    ("测试 / 构建 / 工具链", ["测试", "构建", "脚本", "ASan", "编译", "BOM", "PowerShell", "cmake", "MSBuild"]),
    ("打包 / 发布 / 签名", ["安装", "签名", "打包", "发布", "SBOM", "便携"]),
    ("性能 / 内存 / 启动", ["内存", "峰值", "启动", "毫秒", "ms", "慢", "private", "工作集"]),
    ("文件 / Shell / 拖放", ["关联", "拖放", "回收站", "另存", "Shell", "缩略图处理器"]),
    ("设置 / 持久化", ["设置", "持久化", "ini", "注册表"]),
]


def classify(text):
    best = None
    for index, (name, words) in enumerate(THEMES):
        hits = sum(1 for w in words if w in text)
        if hits and (best is None or hits > best[0]):
            best = (hits, name)
    return best[1] if best else "其它"


def version_of(number):
    if number <= 138:
        return "v1.0"
    if number <= 300:
        return "v2.0"
    return "v3.0"


def clean(text):
    text = re.sub(r"\s+", " ", text).strip()
    return text.replace("|", "\\|")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--handoff", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    lines = open(args.handoff, "r", encoding="utf-8").read().splitlines()
    start = next(i for i, l in enumerate(lines) if l.startswith("## 6. "))
    end = next(i for i, l in enumerate(lines) if l.startswith("## 7. "))
    entries = []
    cur = None
    for line in lines[start:end]:
        m = re.match(r"^(\d+)\. \*\*(.+?)\*\*(.*)$", line)
        if m:
            if cur:
                entries.append(cur)
            cur = {"n": int(m.group(1)), "head": m.group(2).strip(),
                   "rest": m.group(3).strip().lstrip("：: "), "body": []}
        elif cur is not None and line.strip():
            cur["body"].append(line.strip())
    if cur:
        entries.append(cur)

    for e in entries:
        text = e["head"] + " " + e["rest"] + " " + " ".join(e["body"])
        e["text"] = text
        e["theme"] = classify(text)
        e["ver"] = version_of(e["n"])
        e["done"] = any(k in text for k in ("已修", "已改", "已完成", "已关闭", "已不再", "已修正"))

    groups = {}
    for e in entries:
        groups.setdefault(e["theme"], []).append(e)

    out = []
    out.append("# 踩坑总表 · 1.0 / 2.0 / 3.0 合并版")
    out.append("")
    out.append("> **用途**：v3.1 开工前一页看完历史坑。**唯一事实源**是 `HANDOFF.md` §6 的编号清单")
    out.append("> （1–328，缺 160）；本文件只是按主题重排 + 每条给一句摘要，**不改写原文**。")
    out.append("> 查细节请按编号回 `HANDOFF.md` §6；v3.0 相关的精炼版见 `规划/27-v3.0-踩坑与硬约束.md`。")
    out.append(">")
    out.append(f"> 条目数：**{len(entries)}**（v1.0 {sum(1 for e in entries if e['ver']=='v1.0')} / "
               f"v2.0 {sum(1 for e in entries if e['ver']=='v2.0')} / "
               f"v3.0 {sum(1 for e in entries if e['ver']=='v3.0')}）；"
               f"其中正文自述“已修/已改”的 {sum(1 for e in entries if e['done'])} 条，保留作“别再犯”的证据。")
    out.append(">")
    out.append("> **编号 ≠ 版本**：1–138 多为 v1.0 基础栈，139–300 多为 v2.0 UI/交互/测试，301 以后是 v3.0。")
    out.append("> 本表按**主题**重排，所以同一主题里会混着不同版本；`版本` 列只用于回查时间线。")
    out.append("")
    out.append("## 今天（v3.1）最该先看的 12 条")
    out.append("")
    picks = [20, 12, 13, 78, 146, 219, 271, 290, 299, 303, 306, 322]
    by_n = {e["n"]: e for e in entries}
    out.append("| # | 标题 | 为什么现在看 |")
    out.append("| --- | --- | --- |")
    why = {
        20: "大图不进 LRU / >64 MB 不落盘 —— v3.1 的档位与缓存容量都要重新划",
        12: "缩略图按 edge 分目录 —— 档位缓存别和它混",
        13: "缩略图队列上限 128 —— 预取也要有上限",
        78: "动画/多帧与缓存守卫 —— HEIC/多页 TIFF 的坑",
        146: "全屏隐藏缩略图已在 v3.0 反转 —— 别再按旧文档改",
        219: "独立窗口的 Esc 主人 —— 设置/信息窗",
        271: "测试钩子时间窗 —— 新增连点钩子要排在这之外",
        290: "定时器 id 归渲染器（30）—— 过渡帧定时器别撞号",
        299: "没有 Ctrl+0 重置旋转 —— 文档已更正",
        303: "`--screenshot` 会关窗 —— 过渡帧验收别和 dump 同进程",
        306: "带中文的 .ps1 必须保 BOM —— 新脚本照做",
        322: "PSD CMYK 参考图是同一近似公式 —— 别拿 max diff=0 当色准",
    }
    for n in picks:
        e = by_n.get(n)
        if not e:
            continue
        out.append(f"| {n} | {clean(e['head'])} | {why[n]} |")
    out.append("")
    out.append("## 按主题")
    out.append("")
    order = [name for name, _ in THEMES] + ["其它"]
    for theme in order:
        items = groups.get(theme, [])
        if not items:
            continue
        out.append(f"### {theme}（{len(items)} 条）")
        out.append("")
        out.append("| # | 版本 | 一句话 | 状态 |")
        out.append("| --- | --- | --- | --- |")
        for e in sorted(items, key=lambda x: x["n"]):
            summary = clean((e["rest"] or " ".join(e["body"]))[:110])
            if len((e["rest"] or " ".join(e["body"]))) > 110:
                summary += "…"
            state = "自述已修" if e["done"] else ""
            out.append(f"| {e['n']} | {e['ver']} | **{clean(e['head'])}**：{summary} | {state} |")
        out.append("")

    out.append("## v3.1 新增（本轮测速发现，按同格式追加）")
    out.append("")
    out.append("| # | 一句话 | 证据 |")
    out.append("| --- | --- | --- |")
    out.append("| V31-1 | 预取只在命中路径上排（`main.cpp:4789/4826/6986`），一旦未命中就断链；22 MB×200 ms 时 18/28 次取消、只显示 1–2 张 | §7.5/§7.6，`20261027-full` |")
    out.append("| V31-2 | current 撞上在途同键预取不合并（`decode_service.cpp:78-95`），同键解两遍 | 同上 |")
    out.append("| V31-3 | 预取投递门 `pending_current_id_ != 0` 在连点时恒关（`decode_service.cpp:120`） | 同上 |")
    out.append("| V31-4 | 我们只显示完成的 1280 档；2345 连点先顶低分辨率过渡帧（同画面梯度 0.38→1.00） | §7.4 末段 |")
    out.append("| V31-5 | 首图必全解 → 峰值 private 405–466 MB，是 B 的 2.1× | §7.2/§7.6-7 |")
    out.append("")
    out.append("> 维护：HANDOFF §6 新增条目后，重跑 `new/tests/tools/make_pitfall_index.py` 即可刷新本文件。")

    open(args.out, "w", encoding="utf-8").write("\n".join(out) + "\n")
    print(f"wrote {args.out}: {len(entries)} entries, {len(groups)} themes")


if __name__ == "__main__":
    main()
