#!/usr/bin/env bash
# 本机（Windows + MinGW/LLVM-MinGW）自检：两套编译器都必须编译通过，
# 单元测试与端到端自检在「能跑的编译器」上必须全绿。
#
# 用途：把「代码对不对」与「WSL 环境装没装」彻底分开。
#
# 已知工具链限制（已用最小程序隔离确认，不是本项目代码的问题）：
#   llvm-mingw 的 libc++ 与本机这份 asio 组合，**运行期 std::thread 构造会抛
#   system_error（CreateThread 返回"不支持的操作"）**。
#   证据链：纯 std::thread 程序 clang++ 正常；最小 asio+thread 程序 clang++ 崩、g++ 正常。
#   ⇒ 因此 gw_sim 的端到端自检只在本机用 g++ 跑；clang++ 仍参与**编译检查**
#     （它抓到过 g++ 没报的 -Wunused-private-field）。真目标 WSL/Linux 不受此限。
set -uo pipefail
cd "$(dirname "$0")/.."

CMAKE="${CMAKE:-cmake}"
ROOT="$(pwd)"

# 本地 asio 头文件（离线用）。没有的话 gw_sim 走 FetchContent（需要网络）。
ASIO_DIR="${ASIO_DIR:-${ROOT}/../.workbuddy/third_party/asio/include}"
SIM_ARGS=()
if [ -f "${ASIO_DIR}/asio.hpp" ]; then
    SIM_ARGS=(-DGW_BUILD_SIM=ON -DGW_ASIO_INCLUDE_DIR="${ASIO_DIR}")
else
    echo "（未发现本地 asio：${ASIO_DIR}，本次跳过 gw_sim）"
fi

# 本地 Paho（离线用）。注意：**依赖必须放在纯 ASCII 路径下** ——
# ninja 的链接步骤要经 cmd.exe（中文 Windows 用 GBK 代码页），
# 路径里的中文会被打乱，ld 报 "cannot find .../妗岄潰椤圭洰/..."。
# 编译步骤不经 cmd.exe，所以 -isystem 的中文路径一直是好的 —— 只有链接会中招。
PAHO_ROOT="${PAHO_ROOT:-$HOME/.workbuddy/third_party/paho}"
MQTT_ARGS=()
if [ -f "${PAHO_ROOT}/include/MQTTClient.h" ] && [ -f "${PAHO_ROOT}/lib/libpaho-mqtt3c-static.a" ]; then
    MQTT_ARGS=(-DGW_WITH_MQTT=ON
               -DGW_PAHO_INCLUDE_DIR="${PAHO_ROOT}/include"
               -DGW_PAHO_LIBRARY="${PAHO_ROOT}/lib/libpaho-mqtt3c-static.a")
else
    echo "（未发现本地 Paho：${PAHO_ROOT}，本次跳过 mqtt_e2e）"
fi

locate_bin() {
    for cand in "$1" "$1.exe"; do
        [ -f "$cand" ] && { printf '%s' "$cand"; return 0; }
    done
    return 1
}

# 本机是否处在 MinGW/MSYS 环境（决定能否跑 gw_sim）
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;;
    *)                    IS_WINDOWS=0 ;;
esac

run_one() {
    local name="$1" cxx="$2" dir="$3"
    local can_run_sim="$4"
    echo "════════════════════════════════════════════════════════"
    echo " 编译器: ${name}   (${cxx})"
    echo "════════════════════════════════════════════════════════"
    rm -rf "$dir"
    if ! "$CMAKE" -S . -B "$dir" -G Ninja \
            -DCMAKE_CXX_COMPILER="$cxx" \
            -DCMAKE_BUILD_TYPE=Debug \
            -DGW_WERROR=ON ${SIM_ARGS[@]+"${SIM_ARGS[@]}"} ${MQTT_ARGS[@]+"${MQTT_ARGS[@]}"} \
            > "$dir.configure.log" 2>&1; then
        echo "  [FAIL] 配置失败，日志: $dir.configure.log"
        tail -25 "$dir.configure.log"
        return 1
    fi
    if ! "$CMAKE" --build "$dir" -j > "$dir.build.log" 2>&1; then
        echo "  [FAIL] 编译失败，日志: $dir.build.log"
        grep -E "error|warning" "$dir.build.log" | head -30
        return 1
    fi
    echo "  编译: 通过（无告警，-Werror 生效）"

    local rc=0 tests_bin demo_bin proxy_bin sim_bin mqtt_bin

    tests_bin="$(locate_bin "$dir/tests/gw_tests")" || { echo "  [FAIL] 找不到 gw_tests"; return 1; }
    echo "  ── 单元测试 ──"
    "$tests_bin" > "$dir.tests.log" 2>&1 || rc=1
    grep -E "^RESULT:" "$dir.tests.log" || tail -4 "$dir.tests.log"

    if demo_bin="$(locate_bin "$dir/apps/proto_demo/proto_demo")"; then
        echo "  ── 协议层演示 ──"
        "$demo_bin" --selftest > "$dir.demo.log" 2>&1 || rc=1
        grep -E "^RESULT:" "$dir.demo.log" || tail -2 "$dir.demo.log"
    fi

    if proxy_bin="$(locate_bin "$dir/apps/proxy_demo/proxy_demo")"; then
        echo "  ── 断网续传场景（默认：断网 10 分钟）──"
        "$proxy_bin" > "$dir.proxy.log" 2>&1 || rc=1
        grep -E "★ 端到端丢失|接收去重|积压峰值" "$dir.proxy.log" | sed 's/^/     /'
        grep -E "^RESULT:" "$dir.proxy.log" || tail -2 "$dir.proxy.log"
    fi

    if sim_bin="$(locate_bin "$dir/apps/sim/gw_sim")"; then
        if [ "$can_run_sim" = "1" ]; then
            echo "  ── 设备模拟器端到端自检 ──"
            "$sim_bin" --selftest > "$dir.sim.log" 2>&1 || rc=1
            grep -E "^RESULT:" "$dir.sim.log" || tail -4 "$dir.sim.log"
        else
            echo "  ── 设备模拟器 ──"
            echo "     编译通过；本机跳过运行（llvm-mingw/libc++ 与本机 asio 的线程交互问题，见文件头说明）"
        fi
    fi

    if mqtt_bin="$(locate_bin "$dir/apps/mqtt_e2e/mqtt_e2e")"; then
        if [ "$can_run_sim" = "1" ]; then
            echo "  ── MQTT 端到端（真 TCP + 真 MQTT 帧 + 断网续传）──"
            "$mqtt_bin" --selftest > "$dir.mqtt.log" 2>&1 || rc=1
            grep -E "唯一接收|★ 端到端丢失|重复投递|积压峰值|^RESULT:" "$dir.mqtt.log" | sed 's/^/     /'
        else
            echo "  ── MQTT 端到端 ──"
            echo "     编译通过；本机跳过运行（同 llvm-mingw 线程限制）"
        fi
    fi

    if [ $rc -ne 0 ]; then
        echo "  [FAIL] 有测试未通过"
        grep -E "FAIL|:[0-9]+:" "$dir.tests.log" "$dir.demo.log" "$dir.sim.log" 2>/dev/null | head -20
    fi
    return $rc
}
overall=0
# g++：编译 + 运行全部自检
run_one "g++  (MinGW-W64 UCRT)" g++   build-host-gcc   1 || overall=1
# clang++：编译检查（它抓到过 g++ 漏掉的告警）；运行自检在本机受限
if [ "$IS_WINDOWS" = "1" ]; then
    run_one "clang++ (LLVM-MinGW)" clang++ build-host-clang 0 || overall=1
else
    run_one "clang++" clang++ build-host-clang 1 || overall=1
fi

echo
if [ $overall -eq 0 ]; then
    echo "✔ 全部通过"
else
    echo "✘ 存在失败项，见上方输出"
fi
exit $overall
