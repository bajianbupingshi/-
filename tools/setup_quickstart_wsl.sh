#!/usr/bin/env bash
# ============================================================================
# D3：把「Modbus 模拟器 → Neuron → MQTT broker」整条链路**脚本化**跑通并自证
#
# 用法（在 WSL 里执行）：
#   bash "/mnt/e/桌面项目/agent/嵌入式/4/gateway/tools/setup_quickstart_wsl.sh"
#
# 为什么不用 Web 界面点：手点不可复现、出错不可重跑、也进不了 CI。
#
# 所有接口字段名与语义都是**读上游源码确认**的，不是猜的：
#   include/neuron/errcodes.h        错误码表（2002=NODE_EXIST 等）
#   include/neuron/tag.h             NEU_ATTRIBUTE_READ=1 / WRITE=2
#   include/neuron/type.h            NEU_TYPE_INT16=3
#   include/neuron/msg.h             NEU_ADAPTER_CTL_START=0 / STOP=1
#   src/parser/neu_json_login.c      登录字段是 {"name","pass"}（不是 password！）
#   src/parser/neu_json_node.c       节点/设置的请求体
#   src/parser/neu_json_group_config.c  /api/v2/group 及其 GET 查询参数
#   src/parser/neu_json_tag.c        点位字段与类型
#   plugins/mqtt/mqtt.json           driver-topic-prefix 默认 'neuron/${random_str}'
#   plugins/modbus/modbus-tcp.json   host / port / timeout / interval
#
# 设计原则（沿用「只信产物、不信自我报告」）：
#   成功判据是**容错式**的 —— 响应里没有 error 字段、或 error==0，都算成功；
#   建组/建点位之后**读回验证**，而不是相信创建接口的返回码。
#
# 可选环境变量：
#   SKIP_MQTT=1    不起 broker、不验北向（只验南向采集）
#   SKIP_START=1   不自动拉起 neuron / 模拟器 / broker（假设已在跑）
# ============================================================================
set -uo pipefail

NEURON_HOME="${NEURON_HOME:-$HOME/neuron}"
BUILD="${NEURON_HOME}/build"
API="${API:-http://127.0.0.1:7000}"
USER_NAME="${USER_NAME:-admin}"
USER_PASS="${USER_PASS:-0000}"

DRV_NODE="${DRV_NODE:-modbus-tcp-1}"
APP_NODE="${APP_NODE:-mqtt-app}"
GROUP="${GROUP:-group1}"
TAG="${TAG:-tag1}"
SLAVE_PORT="${SLAVE_PORT:-1502}"
BROKER_HOST="${BROKER_HOST:-127.0.0.1}"
BROKER_PORT="${BROKER_PORT:-1883}"
# ★ 必须固定：mqtt.json 里 driver-topic-prefix 默认是 'neuron/${random_str}'，
#   不显式设死，话题名每次都不同，订阅端永远追不到。
TOPIC_PREFIX="${TOPIC_PREFIX:-neuron}"

overall=0
step() { printf '\n\033[1m── %s ──\033[0m\n' "$1"; }
ok()   { printf '  [OK]   %s\n' "$1"; }
bad()  { printf '  [FAIL] %s\n' "$1"; overall=1; }
info() { printf '  %s\n' "$1"; }

TOKEN=""
AUTH=()

api() { # api METHOD PATH [BODY]
    local m="$1" p="$2" b="${3:-}"
    if [ -n "$b" ]; then
        curl -s --max-time 15 -X "$m" "${API}${p}" \
            -H 'Content-Type: application/json' ${AUTH[@]+"${AUTH[@]}"} -d "$b"
    else
        curl -s --max-time 15 -X "$m" "${API}${p}" ${AUTH[@]+"${AUTH[@]}"}
    fi
}

# 成功判据（容错）：JSON 对象且没有 error 字段，或 error==0
jok() {
    python3 -c '
import json,sys
raw=sys.stdin.read()
try: d=json.loads(raw)
except Exception: raise SystemExit(1)
if not isinstance(d,dict): raise SystemExit(1)
e=d.get("error")
raise SystemExit(0 if (e is None or e==0) else 1)'
}
# 打印错误码（没有则 <none>）
je() {
    python3 -c '
import json,sys
raw=sys.stdin.read()
try: d=json.loads(raw)
except Exception: print("NON_JSON"); raise SystemExit
if not isinstance(d,dict): print("NON_OBJECT"); raise SystemExit
print(d.get("error","<none>"))'
}
# 通用步骤检查：成功 / 指定的"已存在"错误码也当成功 / 其余报失败
chk() { # chk 描述 响应 [容忍的错误码]
    if printf '%s' "$2" | jok; then ok "$1"; return 0; fi
    if [ -n "${3:-}" ] && [ "$(printf '%s' "$2" | je)" = "$3" ]; then
        ok "$1（幂等：已存在）"; return 0
    fi
    bad "$1 失败：$(printf '%s' "$2" | head -c 200)（错误码 $(printf '%s' "$2" | je)）"
    return 1
}
contains() { printf '%s' "$1" | grep -qF "$2"; }
port_up() { (exec 3<>"/dev/tcp/$1/$2") 2>/dev/null; }

# ── 0) 前置：三个进程都要在跑 ────────────────────────────────────────────────
step "0/8 前置检查（neuron / Modbus 模拟器 / MQTT broker）"
if [ "${SKIP_START:-0}" != "1" ] && ! curl -s --max-time 3 "${API}/api/v2/ping" >/dev/null 2>&1; then
    info "neuron 未运行，拉起 ${BUILD}/neuron"
    ( cd "${BUILD}" && nohup ./neuron --log > "${BUILD}/neuron.log" 2>&1 & )
    for _ in $(seq 1 20); do curl -s --max-time 2 "${API}/api/v2/ping" >/dev/null && break; sleep 1; done
fi
if curl -s --max-time 3 "${API}/api/v2/ping" >/dev/null 2>&1; then
    ok "neuron 在 ${API} 响应"
else
    bad "neuron 未响应 —— 先跑 bootstrap_neuron_wsl.sh"; exit 1
fi

if port_up 127.0.0.1 "${SLAVE_PORT}"; then
    ok "Modbus 模拟器已在 127.0.0.1:${SLAVE_PORT}"
elif [ "${SKIP_START:-0}" != "1" ] && [ -x "${BUILD}/simulator/modbus_simulator" ]; then
    info "拉起模拟器：modbus_simulator tcp ${SLAVE_PORT} ip_v4（Slave ID=1）"
    ( cd "${BUILD}/simulator" && nohup ./modbus_simulator tcp "${SLAVE_PORT}" ip_v4 \
        > "${BUILD}/modbus_sim.log" 2>&1 & )
    sleep 2
    port_up 127.0.0.1 "${SLAVE_PORT}" && ok "模拟器已监听" || bad "模拟器未能监听"
else
    bad "模拟器不可用（缺 ${BUILD}/simulator/modbus_simulator 或端口被占）"
fi

if [ "${SKIP_MQTT:-0}" = "1" ]; then
    info "SKIP_MQTT=1 → 跳过 broker 与北向验证"
elif port_up "${BROKER_HOST}" "${BROKER_PORT}"; then
    ok "MQTT broker 已在 ${BROKER_HOST}:${BROKER_PORT}"
elif [ "${SKIP_START:-0}" != "1" ] && command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    info "用 docker 起 NanoMQ"
    docker rm -f nanomq >/dev/null 2>&1
    docker run -d --name nanomq -p "${BROKER_PORT}:1883" emqx/nanomq:latest >/dev/null 2>&1
    for _ in $(seq 1 20); do port_up "${BROKER_HOST}" "${BROKER_PORT}" && break; sleep 1; done
    port_up "${BROKER_HOST}" "${BROKER_PORT}" && ok "NanoMQ 已监听" || bad "NanoMQ 未监听"
else
    bad "没有可用 broker（docker 不可用且 ${BROKER_PORT} 未监听）→ 只验南向"
    SKIP_MQTT=1
fi

# ── 1) 登录 ──────────────────────────────────────────────────────────────────
step "1/8 登录（字段名是 pass，不是 password）"
resp="$(api POST /api/v2/login "{\"name\":\"${USER_NAME}\",\"pass\":\"${USER_PASS}\"}")"
TOKEN="$(printf '%s' "$resp" | python3 -c '
import json,sys
try: d=json.load(sys.stdin)
except Exception: d={}
print((d or {}).get("token","") if isinstance(d,dict) else "")')"
if [ -n "${TOKEN}" ]; then
    AUTH=(-H "Authorization: Bearer ${TOKEN}")
    ok "拿到 token（长度 ${#TOKEN}）"
else
    bad "登录失败：$(printf '%s' "$resp" | head -c 200)（错误码 $(printf '%s' "$resp" | je)）"
    exit 1
fi

# ── 2) 南向节点 ──────────────────────────────────────────────────────────────
step "2/8 南向节点 ${DRV_NODE}（plugin=\"Modbus TCP\"）"
nodes="$(api GET /api/v2/node)"
if contains "${nodes}" "\"${DRV_NODE}\""; then
    ok "节点已存在（读回确认）"
else
    chk "创建节点" "$(api POST /api/v2/node "{\"plugin\":\"Modbus TCP\",\"name\":\"${DRV_NODE}\"}")" "2002"
fi
chk "写入驱动参数（host=127.0.0.1 port=${SLAVE_PORT}）" \
    "$(api POST /api/v2/node/setting \
        "{\"node\":\"${DRV_NODE}\",\"params\":{\"host\":\"127.0.0.1\",\"port\":${SLAVE_PORT},\"timeout\":3000,\"interval\":10}}")"

# ── 3) 组与点位（建完读回验证）───────────────────────────────────────────────
step "3/8 组 ${GROUP} + 点位 ${TAG}（1!40001，INT16，READ|WRITE）"
api POST /api/v2/group "{\"node\":\"${DRV_NODE}\",\"group\":\"${GROUP}\",\"interval\":1000}" >/dev/null
groups="$(api GET "/api/v2/group?node=${DRV_NODE}")"
contains "${groups}" "\"${GROUP}\"" \
    && ok "组存在（读回确认，interval=1000ms）" \
    || bad "组未建成，GET /api/v2/group 返回：$(printf '%s' "${groups}" | head -c 200)"

# type=3 → NEU_TYPE_INT16；attribute=3 → READ|WRITE
api POST /api/v2/tags \
    "{\"node\":\"${DRV_NODE}\",\"group\":\"${GROUP}\",\"tags\":[{\"type\":3,\"name\":\"${TAG}\",\"attribute\":3,\"address\":\"1!40001\",\"precision\":0,\"decimal\":0,\"bias\":0,\"description\":\"quickstart\",\"value\":0,\"unit\":\"\"}]}" >/dev/null
tags="$(api GET "/api/v2/tags?node=${DRV_NODE}&group=${GROUP}")"
contains "${tags}" "\"${TAG}\"" \
    && ok "点位存在（读回确认）" \
    || bad "点位未建成，GET /api/v2/tags 返回：$(printf '%s' "${tags}" | head -c 200)"

# ── 4) 北向应用 ──────────────────────────────────────────────────────────────
if [ "${SKIP_MQTT:-0}" != "1" ]; then
    step "4/8 北向应用 ${APP_NODE}（plugin=\"MQTT\"）"
    if contains "${nodes}" "\"${APP_NODE}\""; then
        ok "应用已存在（读回确认）"
    else
        chk "创建应用" "$(api POST /api/v2/node "{\"plugin\":\"MQTT\",\"name\":\"${APP_NODE}\"}")" "2002"
    fi
    # 只写必需项；driver-topic-prefix 固定住，其余走 schema 默认
    chk "写入应用参数（${BROKER_HOST}:${BROKER_PORT}，prefix=${TOPIC_PREFIX}）" \
        "$(api POST /api/v2/node/setting \
            "{\"node\":\"${APP_NODE}\",\"params\":{\"client-id\":\"neuron-quickstart\",\"host\":\"${BROKER_HOST}\",\"port\":${BROKER_PORT},\"qos\":0,\"format\":0,\"driver-topic-prefix\":\"${TOPIC_PREFIX}\"}}")"
else
    step "4/8 北向应用（SKIP_MQTT=1 → 跳过）"
fi

# ── 5) 启动节点（cmd: 0=START 1=STOP）────────────────────────────────────────
step "5/8 启动节点"
for n in ${DRV_NODE} ${APP_NODE}; do
    if [ "${SKIP_MQTT:-0}" = "1" ] && [ "${n}" = "${APP_NODE}" ]; then continue; fi
    chk "启动 ${n}" "$(api POST /api/v2/node/ctl "{\"node\":\"${n}\",\"cmd\":0}")"
done
sleep 2
api GET /api/v2/node/state | python3 -c '
import json,sys
try: d=json.load(sys.stdin)
except Exception: d={}
for s in ((d or {}).get("states") or []):
    print("  %-16s running=%-4s link=%-4s rtt=%s" % (s.get("node"), s.get("running"), s.get("link"), s.get("rtt")))' 2>/dev/null || true

# ── 6) 订阅：组 → 应用 ───────────────────────────────────────────────────────
if [ "${SKIP_MQTT:-0}" != "1" ]; then
    step "6/8 订阅 ${DRV_NODE}/${GROUP} → ${APP_NODE}"
    chk "建立订阅" "$(api POST /api/v2/subscribe \
        "{\"app\":\"${APP_NODE}\",\"driver\":\"${DRV_NODE}\",\"group\":\"${GROUP}\"}")"
fi

# ── 7) 自证：南向采到数据 ────────────────────────────────────────────────────
step "7/8 验证南向采集（POST /api/v2/read，最多重试 15 次）"
got=""
for i in $(seq 1 15); do
    r="$(api POST /api/v2/read "{\"node\":\"${DRV_NODE}\",\"group\":\"${GROUP}\"}")"
    if ! printf '%s' "$r" | jok; then
        info "读取返回错误码 $(printf '%s' "$r" | je)，重试 ${i}/15"; sleep 2; continue
    fi
    val="$(printf '%s' "$r" | python3 -c '
import json,sys
try: d=json.load(sys.stdin)
except Exception: d={}
out=[]
for t in ((d or {}).get("tags") or [])[:3]:
    out.append("%s=%s" % (t.get("name"), t.get("value")))
print(" ".join(out))' 2>/dev/null)"
    if [ -n "${val}" ]; then ok "采到数据：${val}"; got=yes; break; fi
    sleep 2
done
[ -n "${got}" ] || bad "15 次重试仍未采到有效值 —— 看 ${BUILD}/neuron.log"

# ── 8) 自证：北向发出 MQTT ───────────────────────────────────────────────────
if [ "${SKIP_MQTT:-0}" != "1" ]; then
    step "8/8 验证北向 MQTT（订阅 ${TOPIC_PREFIX}/# 收一条）"
    if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
        out="$(docker run --rm --network host eclipse-mosquitto:2 \
               mosquitto_sub -h "${BROKER_HOST}" -p "${BROKER_PORT}" \
               -t "${TOPIC_PREFIX}/#" -C 1 -W 20 -v 2>&1)"
        if [ -n "${out}" ]; then
            ok "收到 MQTT 报文："
            printf '%s\n' "${out}" | head -3 | cut -c1-200 | sed 's/^/         /'
        else
            bad "20 秒内没收到报文（检查订阅是否建立 / broker 是否通）"
        fi
    else
        info "docker 不可用，跳过订阅验证；可用 MQTTX 连 ${BROKER_HOST}:${BROKER_PORT} 订阅 ${TOPIC_PREFIX}/#"
    fi
else
    step "8/8 北向验证（SKIP_MQTT=1 → 跳过）"
fi

echo
echo "════════════════════════════════════════════════════════════"
if [ ${overall} -eq 0 ]; then
    echo " ✔ D3 全链路跑通"
else
    echo " ✘ 有失败项（见上方 [FAIL]）"
fi
echo "════════════════════════════════════════════════════════════"
echo "  Web 面板 : ${API}   （${USER_NAME} / ${USER_PASS}）"
echo "  话题     : 前缀 ${TOPIC_PREFIX}，订阅 ${TOPIC_PREFIX}/# 最保险"
echo "  日志     : ${BUILD}/neuron.log"
echo "  停止进程 : pkill -f '${BUILD}/neuron'; pkill -f modbus_simulator; docker rm -f nanomq"
exit ${overall}
