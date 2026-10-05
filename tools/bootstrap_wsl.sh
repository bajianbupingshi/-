#!/usr/bin/env bash
# ============================================================================
# 在 WSL 里一键引导本工程：投递 → 装依赖 → 配置 → 构建 → 测试 → 演示
#
# 用法（在 WSL 的 shell 里执行，路径含中文要加引号）：
#   bash "/mnt/e/桌面项目/agent/嵌入式/4/gateway/tools/bootstrap_wsl.sh"
#
# 为什么不用「Windows 侧把文件写进 /tmp」那套：
#   WSL 的 /tmp 是 tmpfs，发行版空闲回收/重启后**内容全部丢失**。
#   而 /mnt/e 是持久化的，脚本从这里自我复制即可，无需任何预投递。
#
# 可选环境变量：
#   SKIP_APT=1     跳过 apt 安装
#   PRESETS="wsl-debug"   只跑指定 preset（默认跑 debug + release + asan 三套）
# ============================================================================
set -uo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${HOME}/gateway"
SKIP_APT="${SKIP_APT:-0}"
PRESETS="${PRESETS:-wsl-debug wsl-release wsl-asan}"
ITEMS="CMakeLists.txt CMakePresets.json .gitignore README.md common apps plugins tests docs tools"

echo "════════════════════════════════════════════════════════════"
echo " 源目录   : ${SRC}"
echo " 目标目录 : ${DEST}"
echo "════════════════════════════════════════════════════════════"

if [ ! -f "${SRC}/CMakeLists.txt" ]; then
    echo "[FAIL] 源目录里找不到 CMakeLists.txt，说明脚本不在工程内：${SRC}"
    exit 1
fi

# ── 1) 投递到 Linux 文件系统（在 9p 上直接构建会慢很多，所以先拷进 ~）──────
mkdir -p "${DEST}"
for item in ${ITEMS}; do
    if [ -e "${SRC}/${item}" ]; then
        cp -r "${SRC}/${item}" "${DEST}/"
    fi
done
echo "已投递 $(find "${DEST}" -type f -not -path '*/build-*/*' -not -name '*.log' | wc -l) 个源文件"

# ── 2) 依赖 ─────────────────────────────────────────────────────────────────
if [ "${SKIP_APT}" != "1" ]; then
    if ! sudo -n true 2>/dev/null; then
        echo "（接下来可能要求输入 sudo 密码）"
    fi
    sudo apt-get install -y cmake ninja-build g++ libgtest-dev \
        || { echo "首次安装失败，尝试 apt-get update 后重试…"; \
             sudo apt-get update && sudo apt-get install -y cmake ninja-build g++ libgtest-dev; }
fi

for tool in cmake g++ ninja; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "[FAIL] 缺少 ${tool}"
        exit 1
    fi
done

# ── 3) 三套构建 + 测试 ──────────────────────────────────────────────────────
cd "${DEST}" || exit 1
overall=0
declare -a hash_list=()
for preset in ${PRESETS}; do
    echo
    echo "──────── preset: ${preset} ────────"
    if ! cmake --preset "${preset}" > "${preset}.configure.log" 2>&1; then
        echo "[FAIL] 配置失败，见 ${preset}.configure.log"; tail -20 "${preset}.configure.log"; overall=1; continue
    fi
    if ! cmake --build --preset "${preset}" -j"$(nproc)" > "${preset}.build.log" 2>&1; then
        echo "[FAIL] 编译失败，见 ${preset}.build.log"
        grep -E "error|warning" "${preset}.build.log" | head -30; overall=1; continue
    fi
    echo "编译     : 通过"

    # 与 CMakePresets.json 的 binaryDir 一一对应（硬编码比解析 cmake 输出可靠）
    case "${preset}" in
        wsl-debug)   builddir="build-debug" ;;
        wsl-release) builddir="build-rel" ;;
        wsl-asan)    builddir="build-asan" ;;
        wsl-tsan)    builddir="build-tsan" ;;
        *)           builddir="build-${preset}" ;;
    esac

    # 单测
    ctest --preset "${preset}" --output-on-failure > "${preset}.ctest.log" 2>&1
    rc_ctest=$?
    if [ ${rc_ctest} -eq 0 ]; then
        echo "ctest    : $(grep -E 'tests passed' "${preset}.ctest.log" | tail -1)"
    else
        echo "ctest    : 失败"; tail -25 "${preset}.ctest.log"; overall=1
    fi

    # 垫片/真实 GTest 都会打印这一行，抓出来看真实通过数
    tests_bin="${builddir}/tests/gw_tests"
    [ -x "${tests_bin}" ] || tests_bin="${builddir}/tests/gw_tests.exe"
    if [ -x "${tests_bin}" ]; then
        "${tests_bin}" > "${preset}.tests.log" 2>&1
        rc_t=$?
        grep -E "^RESULT:" "${preset}.tests.log" || tail -3 "${preset}.tests.log"
        [ ${rc_t} -ne 0 ] && overall=1
    fi

    # 演示自检
    demo_bin="${builddir}/apps/proto_demo/proto_demo"
    [ -x "${demo_bin}" ] || demo_bin="${builddir}/apps/proto_demo/proto_demo.exe"
    if [ -x "${demo_bin}" ]; then
        "${demo_bin}" --selftest > "${preset}.demo.log" 2>&1
        rc_d=$?
        grep -E "^RESULT:" "${preset}.demo.log" || tail -3 "${preset}.demo.log"
        [ ${rc_d} -ne 0 ] && { overall=1; grep -E "FAIL" "${preset}.demo.log" | head -10; }
    fi

    # 确定性哈希：不同 preset 之间必须完全相同 —— 若 -O0 / -O2 / ASan 影响了字节流，
    # 说明有依赖优化级别或未定义行为的代码，那是必须查的。
    hsh="$(sed -n 's/^ *FNV-1a(seed=[0-9][0-9]*) *: *\(0x[0-9A-Fa-f]*\).*$/\1/p' "${preset}.demo.log" | head -1)"
    if [ -n "${hsh}" ]; then
        echo "确定性   : FNV-1a(seed=20261005) = ${hsh}"
        hash_list+=("${preset}=${hsh}")
    fi

    # 用的到底是真 GTest 还是垫片，必须说清楚
    grep -hE "tests:" "${preset}.configure.log" | sed 's/^/           /'
done

# ── 跨 preset 哈希一致性（本脚本的核心自检之一）─────────────────────────────
if [ ${#hash_list[@]} -gt 1 ]; then
    uniq_n="$(printf '%s\n' "${hash_list[@]}" | sed 's/^[^=]*=//' | sort -u | wc -l)"
    if [ "${uniq_n}" -eq 1 ]; then
        echo "✔ ${#hash_list[@]} 套 preset 的确定性哈希完全一致（优化级别与 sanitizer 都不影响字节流）"
    else
        echo "✘ 不同 preset 的哈希不一致 —— 构建参数影响了输出，必须查："
        printf '    %s\n' "${hash_list[@]}"
        overall=1
    fi
fi

echo
echo "════════════════════════════════════════════════════════════"
if [ ${overall} -eq 0 ]; then
    echo " ✔ WSL 侧全部通过"
else
    echo " ✘ 存在失败项，见各 preset 的 *.log"
fi
echo "════════════════════════════════════════════════════════════"

# ── 4) 顺手打印种子确定性证据（debug 构建）─────────────────────────────────
if [ -x "build-debug/apps/proto_demo/proto_demo" ]; then
    echo
    echo "── 确定性重放证据（build-debug）──"
    build-debug/apps/proto_demo/proto_demo | sed -n '/\[5\]/,/RESULT/p'
fi

exit ${overall}
