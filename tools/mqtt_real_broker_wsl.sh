#!/usr/bin/env bash
# 对着**真实 broker**（NanoMQ，docker）跑「断网续传」，并在接收端对账
#
# 用法（WSL 里执行）：
#   bash "/mnt/e/桌面项目/agent/嵌入式/4/gateway/tools/mqtt_real_broker_wsl.sh"
#
# 前置：
#   1) 项目已用 GW_WITH_MQTT=ON 构建过（见下方 BUILD_HINT）
#   2) docker 可用（NanoMQ 跑在容器里）
#
# 分工：
#   A) docker 起 NanoMQ（1883）      = 真 broker
#   B) mqtt_e2e --sub                = 接收端，自己数「唯一/缺失/重复」
#   C) mqtt_e2e --pub --outage-sec   = 发布端，含一次断网（网卡 down 事件语义）
# 结束打印两端数字，必须一致：丢失 0、重复 0。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${MQTT_E2E_BIN:-${ROOT}/build-debug/apps/mqtt_e2e/mqtt_e2e}"
COUNT="${COUNT:-2000}"
PERIOD_MS="${PERIOD_MS:-10}"
OUTAGE_SEC="${OUTAGE_SEC:-5}"
PORT="${BROKER_PORT:-1883}"
TOPIC="${TOPIC:-neuron/gateway/data}"

echo "================================================================"
echo " 真 broker 端到端：NanoMQ(docker) + 发布侧断网 + 接收端对账"
echo "================================================================"

if [ ! -x "${BIN}" ]; then
    cat <<EOF
找不到可执行文件：${BIN}

先在 WSL 里构建（需要 Paho 与 asio）：

  cd "${ROOT}"
  sudo apt-get install -y cmake ninja-build g++ libpaho-mqtt-dev git
  cmake -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \\
        -DGW_WITH_MQTT=ON -DGW_BUILD_SIM=ON
  cmake --build build-debug -j"\$(nproc)"

（asio 会由 CMake 通过 FetchContent 自动拉取；若网络受限，可用
  -DGW_ASIO_INCLUDE_DIR=<本地 asio>/include 指定。）
EOF
    exit 1
fi

if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
    echo "docker 不可用 —— 本脚本需要一个真实 broker。"
    echo "可选替代：sudo apt-get install -y mosquitto && sudo systemctl start mosquitto"
    exit 1
fi

echo "── 1) 起 NanoMQ（真 broker）──"
docker rm -f nanomq >/dev/null 2>&1
docker run -d --name nanomq -p "${PORT}:1883" emqx/nanomq:latest >/dev/null
for _ in $(seq 1 20); do
    (exec 3<>"/dev/tcp/127.0.0.1/${PORT}") 2>/dev/null && break
    sleep 1
done
if (exec 3<>"/dev/tcp/127.0.0.1/${PORT}") 2>/dev/null; then
    echo "   NanoMQ 已监听 127.0.0.1:${PORT}"
else
    echo "   NanoMQ 未能监听"; docker logs --tail 20 nanomq; exit 1
fi

LOG_SUB="${ROOT}/mqtt_sub.log"
rm -f "${LOG_SUB}"

echo "── 2) 起接收端（订阅并统计）──"
"${BIN}" --sub --host 127.0.0.1 --port "${PORT}" --topic "${TOPIC}" --expect "${COUNT}" \
    > "${LOG_SUB}" 2>&1 &
SUB_PID=$!
sleep 1

echo "── 3) 发布侧：${COUNT} 条，第 1/4 处断网 ${OUTAGE_SEC} 秒 ──"
"${BIN}" --pub --host 127.0.0.1 --port "${PORT}" --topic "${TOPIC}" \
    --count "${COUNT}" --period-ms "${PERIOD_MS}" --outage-sec "${OUTAGE_SEC}"
PUB_RC=$?

echo "── 4) 等接收端对账 ──"
for _ in $(seq 1 60); do
    grep -q "RESULT" "${LOG_SUB}" && break
    sleep 1
done
kill "${SUB_PID}" 2>/dev/null

echo
echo "接收端输出："
tail -12 "${LOG_SUB}"

echo
echo "================================================================"
if [ ${PUB_RC} -eq 0 ] && grep -q "0 failed" "${LOG_SUB}"; then
    echo " ✔ 真 broker 上：断网续传丢失 0、重复 0"
else
    echo " ✘ 存在失败项（发布侧 exit=${PUB_RC}，详见上方）"
fi
echo "================================================================"
echo "  清理：docker rm -f nanomq"
exit $([ ${PUB_RC} -eq 0 ] && grep -q "0 failed" "${LOG_SUB}" && echo 0 || echo 1)
