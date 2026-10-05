#!/usr/bin/env bash
# Windows 侧：打印「进 WSL 之后该跑什么」。
#
# 历史教训（不要再犯）：早先这个脚本把工程从 Windows 侧写进
# //wsl.localhost/<distro>/tmp/gateway。**WSL 的 /tmp 是 tmpfs**，
# 发行版空闲回收（默认 60s）或重启后内容全部丢失，
# 于是就出现了「投递完 30 个文件、用户一分钟后进去发现目录不存在」。
#
# 现在的做法：不做任何预投递。工程本来就在 Windows 磁盘上，
# WSL 通过 /mnt/<drive> 就能读到，由 WSL 侧脚本自己拷进 ~/gateway。
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd -P)"

# git bash 的 pwd 有两种形态，都要认：
#   /e/桌面项目/...     (MSYS 型)
#   E:/桌面项目/... 或 E:\桌面项目\...  (Win32 型)
norm="${SRC}"
case "${norm}" in
    /?/*)            drive="${norm:1:1}"; rest="${norm:3}" ;;
    [A-Za-z]:[\\/]*) drive="${norm:0:1}"; rest="${norm:3}" ;;
    *) echo "无法把这个路径转换成 WSL 路径：${SRC}"; ;;
esac
if [ -n "${drive:-}" ]; then
    drive="$(printf '%s' "${drive}" | tr 'A-Z' 'a-z')"
    rest="$(printf '%s' "${rest}" | tr '\\' '/')"
    wsl_src="/mnt/${drive}/${rest}"
else
    wsl_src="${SRC}"
fi

cat <<EOF
工程在 Windows 磁盘上，WSL 直接就能读到，不需要预投递。

请打开 WSL，然后执行这一条：

  bash "${wsl_src}/tools/bootstrap_wsl.sh"

它会完成：投递到 ~/gateway → 装依赖 → 跑 wsl-debug / wsl-release / wsl-asan
三套配置并各自执行单元测试与演示自检 → 打印真实通过数与确定性重放证据。

（若只想跳过 apt 安装：SKIP_APT=1 bash "${wsl_src}/tools/bootstrap_wsl.sh"）
EOF
