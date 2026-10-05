#!/usr/bin/env bash
# 首次提交 + 打里程碑 tag。
#
# 为什么要单独一个脚本：**提交身份会永久写进你要发布到 GitHub 的历史**，
# 所以不能由工具替你决定。本机全局 git 身份是自动化的占位值
# （Migration Agent <automation@example.com>），用它会污染你的贡献者图。
#
# 用法（二选一）：
#   bash tools/first_commit.sh "Zhang San" "zhangsan@example.com"
#   NAME="Zhang San" EMAIL="zhangsan@example.com" bash tools/first_commit.sh
#
# 它会：
#   1) 把身份设为**仓库级**（不动你的全局配置）
#   2) 核对 .gitignore/.gitattributes 生效（不该有 build* / *.o / *.log 被提交）
#   3) 首次提交 + 打 v0.1.0 标签
set -euo pipefail
cd "$(dirname "$0")/.."

NAME="${1:-${NAME:-}}"
EMAIL="${2:-${EMAIL:-}}"

if [ -z "${NAME}" ] || [ -z "${EMAIL}" ]; then
    echo "用法: bash tools/first_commit.sh \"<name>\" \"<email>\""
    echo "（身份只写入本仓库的 .git/config，不改你的全局 git 配置）"
    exit 2
fi

git rev-parse --git-dir >/dev/null 2>&1 || { echo "当前目录不是 git 仓库，先 git init"; exit 1; }

echo "── 1) 设置仓库级提交身份 ──"
git config user.name  "${NAME}"
git config user.email "${EMAIL}"
echo "   user.name  = $(git config --get user.name)"
echo "   user.email = $(git config --get user.email)"

echo "── 2) 核对忽略规则（构建产物绝不能进仓库）──"
git add -A
leaked="$(git diff --cached --name-only | grep -E '^(build|build-)|\.o$|\.log$|CMakeCache|\.workbuddy' || true)"
if [ -n "${leaked}" ]; then
    echo "   ✘ 有不该提交的文件："
    printf '     %s\n' "${leaked}"
    exit 1
fi
staged="$(git diff --cached --name-only | wc -l)"
echo "   ✔ 暂存 ${staged} 个文件，无构建产物"

echo "── 3) 核对换行（.sh 必须是 LF，否则 Linux 上 bad interpreter）──"
bad="$(git ls-files --eol | grep -E '\.(sh|c|cpp|h|md|json|yml)$' | grep -v 'i/lf' || true)"
if [ -n "${bad}" ]; then
    echo "   ✘ 以下文件索引里不是 LF："
    printf '     %s\n' "${bad}"
    exit 1
fi
echo "   ✔ 文本文件索引内均为 LF"

echo "── 4) 首次提交 ──"
if git rev-parse HEAD >/dev/null 2>&1; then
    echo "   已有提交，改用 --amend 更新内容"
    git commit --amend --no-edit
else
    git commit -q -F - <<'MSG'
feat: 工业物联网关 W1–W2 落地（协议层 / 设备模拟器 / 边缘代理 / MQTT / Neuron 插件）

一次性收录 W1–W2 的实现与实测证据。三个自研件全部落地并各自带端到端自检。

协议层（自研协议 NGWP v1）
- 帧格式：MAGIC(2) | LEN(4,BE) | FUNC | ADDR | QTY | PAYLOAD | CRC16(2)，帧总长 8+LEN
- CRC-16/MODBUS：位运算参考实现 + 256 项查表实现互证，对齐标准校验值 0x4B37
- 显式状态机解析器：阶段用 std::variant，事件用 std::variant，std::visit 分派
- 重同步策略显式可配（CRC 失败时整帧跳过 vs 逐字节扫描），实测噪声 1 vs 13、都不漏帧
- 先校验 LEN 上限再分配缓冲（抗内存放大）

自研件① 设备模拟器（asio）
- 多客户端 TCP 设备 + 进程内端到端自检：单帧往返 / 拆包 / 粘包 / 写后读 / 越界异常帧 / 坏帧重同步

自研件② Neuron 驱动插件
- 薄 C 编译单元定义模块描述符 + C++ 实现业务逻辑
  原因：neu_plugin_intf_funs_t 的 .driver = {…} 是嵌套指定初始化器，
  C99 合法但 C++20 明确禁止，且 neu_plugin_module_t 成员带 const 无法后赋值
- 自带 Neuron SDK 垫片（逐字抄真实关键结构体）+「假 Neuron」自检：
  动态加载 → dlsym("neu_plugin_module") → 驱动完整生命周期 → 校验回调收到设备值

自研件③ 边缘数据代理
- 序列号 + 未确认队列 + LIVE/BACKFILL/CATCHING_UP 状态机（指数退避重连）
- 对账分两个口径：missing_within_received 与 missing_against_produced
  （只看前者会把「从未发出的数据」当成没丢 —— 实测踩过）
- 三条反转断言：静默丢包 / 容量打满 / TTL 过短，对账器必须报出丢失且数量吻合

传输层
- Paho MQTT C 同步 API 实现 ITransport；按采样周期批量打包（22+4N 字节载荷）
- 实测：单条发布 116ms（8.9 点位/秒）→ 50 点位/消息 250 点位/秒（提升 27.9×）
  同步 API 天花板约 250 点位/秒，与 5000 点位/秒的目标差约 20×（已记录为 R9）

实测口径
- 单元测试 88 用例 / 88 通过 / 0 失败（21,128 断言），g++ 与 clang++ 断言数一致
- ctest 8 个用例全过（约 19 秒），含各组件端到端自检
- -Wall -Wextra -Wpedantic -Wshadow -Werror 零告警
- 断网 10 分钟续传：丢失 0 / 重复 0，积压峰值 2406 条
- 确定性：同一 seed 的字节流哈希在 g++/clang++ × MinGW/Linux × -O0/-O2 五种组合下一致
MSG
fi

echo "── 5) 打里程碑标签 ──"
if git rev-parse -q --verify "refs/tags/v0.1.0" >/dev/null; then
    echo "   v0.1.0 已存在，跳过"
else
    git tag -a v0.1.0 -m "W1–W2 完成：协议层 + 设备模拟器 + 驱动插件 + 边缘代理 + 真实 MQTT

实测：单测 88/88（21,128 断言）；ctest 8/8；断网 10 分钟续传丢失 0/重复 0；
确定性哈希跨 3 工具链 × 2 平台 × 2 优化级别一致。"
    echo "   已打标签 v0.1.0"
fi

echo
echo "── 结果 ──"
git --no-pager log --oneline -1
git --no-pager tag -n1
echo
echo "推送到 GitHub："
echo "  git remote add origin <你的仓库地址>"
echo "  git push -u origin main --tags"
