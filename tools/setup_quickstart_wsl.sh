#!/usr/bin/env bash
# ============================================================================
# D3：把「Modbus 模拟器 → Neuron → MQTT broker」整条链路**脚本化**跑通并自证
#
# 用法（在 WSL 里执行）：
#   bash "/mnt/e/桌面项目/agent/嵌入式/4/gateway/tools/setup_quickstart_wsl.sh"
#
# ★ 本版修复的三个真 bug（都在真环境上逐个定位并验证过，2026-10-06）：
#   1) 写节点参数 2004 NODE_SETTING_INVALID —— 硬编码的参数集**缺必填字段**。
#      Neuron 的 schema 里 modbus-tcp 有 14 个必填、mqtt 有 13 个必填
#      （连 offline-cache=false 时 cache-mem-size 也是必填，default 还是 null）。
#      ⇒ 修法：从 GET /api/v2/schema 自动铺满全部字段的 default，再覆盖 host/port。
#   2) 建点位 1002 BODY_IS_WRONG —— 带 decimal/bias 等可选字段反而解码失败
#      （decimal 声明为 DOUBLE，传整数 0 会被拒）。
#      ⇒ 修法：只发 4 个必填字段（type/name/attribute/address），缺省由 Neuron 补。
#   3) 启动 2006 NODE_NOT_READY —— 节点已用空设置在运行，写完参数不会自动生效。
#      ⇒ 修法：写参数前先 stop（容忍"本来就没在跑"），写完再 start。
#   另：北向真实 topic 是 /neuron/<app>/<driver>/<group>（带前导斜杠），
#       所以订阅一律用 '#' 通配；节点存在性检查用 ?type=1（驱动）/2（应用）。
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
TOPIC_PREFIX="${TOPIC_PREFIX:-neuron}"
SKIP_MQTT="${SKIP_MQTT:-0}"
SKIP_START="${SKIP_START:-0}"

overall=0
step() { printf '\n\033[1m── %s ──\033[0m\n' "$1"; }
ok()   { printf '  [OK]   %s\n' "$1"; }
bad()  { printf '  [FAIL] %s\n' "$1"; overall=1; }
info() { printf '  %s\n' "$1"; }

TOKEN=""
AUTH=()

urlencode() { printf '%s' "$1" | sed 's/ /%20/g'; }

api() { # api METHOD PATH [BODY]
    local m="$1" p="$2" b="${3:-}"
    if [ -n "$b" ]; then
        curl -s --max-time 15 -X "$m" "${API}${p}" \
            -H 'Content-Type: application/json' ${AUTH[@]+"${AUTH[@]}"} -d "$b"
    else
        curl -s --max-time 15 -X "$m" "${API}${p}" ${AUTH[@]+"${AUTH[@]}"}
    fi
}

# 容错判据：无 error 字段或 error==0 都算成功（有些成功响应根本不带 error）
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
je() {
    python3 -c '
import json,sys
raw=sys.stdin.read()
try: d=json.loads(raw)
except Exception: print("NON_JSON"); raise SystemExit
if not isinstance(d,dict): print("NON_OBJECT"); raise SystemExit
print(d.get("error","<none>"))'
}
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

# ★ schema 驱动的参数生成：取该插件 schema 里**每个**字段的 default 铺满，
#   default 为 null 时按类型给兜底值，最后套上我们的覆盖项。
#   实测：modbus-tcp 14 必填 / mqtt 13 必填，缺一个就是 2004。
schema_params() { # $1=plugin 名  $2=覆盖项 JSON
    api GET "/api/v2/schema?plugin_name=$(urlencode "$1")" | python3 - "$2" <<'PYEOF'
import json, sys
sch = json.load(sys.stdin)
overrides = json.loads(sys.argv[1])
SKIP = {"tag_regex", "group_interval"}
def fallback(t):
    return {"string": "", "file": "", "int": 0, "map": 0, "bool": False, "double": 0.0}.get(t, "")
out = {}
for k, v in sch.items():
    if k in SKIP or not isinstance(v, dict):
        continue
    d = v.get("default")
    out[k] = fallback(v.get("type")) if d is None else d
out.update(overrides)
print(json.dumps(out, ensure_ascii=False))
PYEOF
}

# ── 0) 前置 ────────────────────────────────────────────────────────────────
step "0/8 前置检查（neuron / Modbus 模拟器 / MQTT broker）"
if [ "${SKIP_START}" != "1" ] && ! curl -s --max-time 3 "${API}/api/v2/ping" >/dev/null 2>&1; then
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
elif [ "${SKIP_START}" != "1" ] && [ -x "${BUILD}/simulator/modbus_simulator" ]; then
    info "拉起模拟器：modbus_simulator tcp ${SLAVE_PORT} ip_v4（Slave ID=1）"
    ( cd "${BUILD}/simulator" && nohup ./modbus_simulator tcp "${SLAVE_PORT}" ip_v4 \
        > "${BUILD}/modbus_sim.log" 2>&1 & )
    sleep 2
    port_up 127.0.0.1 "${SLAVE_PORT}" && ok "模拟器已监听" || bad "模拟器未能监听"
else
    bad "模拟器不可用（缺 ${BUILD}/simulator/modbus_simulator 或端口被占）"
fi

if [ "${SKIP_MQTT}" = "1" ]; then
    info "SKIP_MQTT=1 → 跳过 broker 与北向验证"
elif port_up "${BROKER_HOST}" "${BROKER_PORT}"; then
    ok "MQTT broker 已在 ${BROKER_HOST}:${BROKER_PORT}"
elif [ "${SKIP_START}" != "1" ] && command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    info "用 docker 起 NanoMQ"
    docker rm -f nanomq >/dev/null 2>&1
    docker run -d --name nanomq -p "${BROKER_PORT}:1883" emqx/nanomq:latest >/dev/null 2>&1
    for _ in $(seq 1 20); do port_up "${BROKER_HOST}" "${BROKER_PORT}" && break; sleep 1; done
    port_up "${BROKER_HOST}" "${BROKER_PORT}" && ok "NanoMQ 已监听" || bad "NanoMQ 未监听"
else
    bad "没有可用 broker（docker 不可用且 ${BROKER_PORT} 未监听）→ 只验南向"
    SKIP_MQTT=1
fi

# ── 1) 登录 ────────────────────────────────────────────────────────────────
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
    bad "登录失败：$(printf '%s' "$resp" | head -c 200)（错误码 $(printf '%s' "$resp" | je)）"; exit 1
fi

# ── 2) 南向节点：先停 → schema 参数 → 启动 ────────────────────────────────
step "2/8 南向节点 ${DRV_NODE}（plugin=\"Modbus TCP\"，参数由 schema 自动铺满）"
# ★ 节点存在性必须用 ?type=1（驱动）—— 不带 type 会 1003，之前的存在性检查全是静默失效
nodes="$(api GET "/api/v2/node?type=1")"
if contains "${nodes}" "\"${DRV_NODE}\""; then
    ok "节点已存在（type=1 列表确认）"
else
    chk "创建节点" "$(api POST /api/v2/node "{\"plugin\":\"Modbus TCP\",\"name\":\"${DRV_NODE}\"}")" "2002"
fi
# 已在运行的节点带着旧设置，先停掉（2008/2009 = 本来就没在跑，忽略）
api POST /api/v2/node/ctl "{\"node\":\"${DRV_NODE}\",\"cmd\":1}" >/dev/null

MODBUS_PARAMS="$(schema_params "Modbus TCP" \
    "{\"host\":\"127.0.0.1\",\"port\":${SLAVE_PORT},\"timeout\":3000,\"interval\":10}")"
info "铺满后的参数（共 $(printf '%s' "${MODBUS_PARAMS}" | python3 -c 'import json,sys;print(len(json.load(sys.stdin)))') 个字段）"
chk "写入驱动参数（host=127.0.0.1 port=${SLAVE_PORT}）" \
    "$(api POST /api/v2/node/setting "{\"node\":\"${DRV_NODE}\",\"params\":${MODBUS_PARAMS}}")"
chk "启动 ${DRV_NODE}" "$(api POST /api/v2/node/ctl "{\"node\":\"${DRV_NODE}\",\"cmd\":0}")"

# ── 3) 组与点位 ──────────────────────────────────────────────────────────────
step "3/8 组 ${GROUP} + 点位 ${TAG}（1!40001，INT16，只发 4 个必填字段）"
api POST /api/v2/group "{\"node\":\"${DRV_NODE}\",\"group\":\"${GROUP}\",\"interval\":1000}" >/dev/null
groups="$(api GET "/api/v2/group?node=${DRV_NODE}")"
contains "${groups}" "\"${GROUP}\"" \
    && ok "组存在（读回确认，interval=1000ms）" \
    || bad "组未建成，GET /api/v2/group 返回：$(printf '%s' "${groups}" | head -c 200)"

# ★ 只发 4 个必填：type/name/attribute/address。带 decimal/bias 等可选字段反而 1002
#   （decimal 声明为 DOUBLE，传整数 0 会被解码器拒掉）—— 实测二分定位过。
TAG_JSON="{\"node\":\"${DRV_NODE}\",\"group\":\"${GROUP}\",\"tags\":[{\"type\":3,\"name\":\"${TAG}\",\"attribute\":3,\"address\":\"1!40001\"}]}"
r="$(api POST /api/v2/tags "${TAG_JSON}")"
if printf '%s' "$r" | jok; then
    ok "点位已添加（只发 4 个必填字段）"
else
    e="$(printf '%s' "$r" | je)"
    if [ "$e" = "1102" ] || printf '%s' "$r" | grep -qiE "exist|already"; then
        ok "点位已存在，幂等跳过"
    else
        bad "添加点位失败：$(printf '%s' "$r" | head -c 200)（错误码 ${e}）"
    fi
fi
tags="$(api GET "/api/v2/tags?node=${DRV_NODE}&group=${GROUP}")"
contains "${tags}" "\"${TAG}\"" \
    && ok "点位读回确认" \
    || bad "点位未建成，GET /api/v2/tags 返回：$(printf '%s' "$tags" | head -c 200)"

# ── 4) 北向应用 ──────────────────────────────────────────────────────────────
if [ "${SKIP_MQTT}" != "1" ]; then
    step "4/8 北向应用 ${APP_NODE}（plugin=\"MQTT\"，参数由 schema 自动铺满）"
    # 应用存在性：?type=2
    apps="$(api GET "/api/v2/node?type=2")"
    if contains "${apps}" "\"${APP_NODE}\""; then
        ok "应用已存在（type=2 列表确认）"
    else
        chk "创建应用" "$(api POST /api/v2/node "{\"plugin\":\"MQTT\",\"name\":\"${APP_NODE}\"}")" "2002"
    fi
    api POST /api/v2/node/ctl "{\"node\":\"${APP_NODE}\",\"cmd\":1}" >/dev/null

    MQTT_PARAMS="$(schema_params "MQTT" \
        "{\"client-id\":\"neuron-quickstart\",\"host\":\"${BROKER_HOST}\",\"port\":${BROKER_PORT},\"qos\":0,\"format\":0,\"driver-topic-prefix\":\"${TOPIC_PREFIX}\"}")"
    info "铺满后的参数（共 $(printf '%s' "${MQTT_PARAMS}" | python3 -c 'import json,sys;print(len(json.load(sys.stdin)))') 个字段，含必填的 cache-mem-size 等）"
    chk "写入应用参数（${BROKER_HOST}:${BROKER_PORT}，prefix=${TOPIC_PREFIX}）" \
        "$(api POST /api/v2/node/setting "{\"node\":\"${APP_NODE}\",\"params\":${MQTT_PARAMS}}")"
    chk "启动 ${APP_NODE}" "$(api POST /api/v2/node/ctl "{\"node\":\"${APP_NODE}\",\"cmd\":0}")"
else
    step "4/8 北向应用（SKIP_MQTT=1 → 跳过）"
fi

# ── 5) 节点状态 ──────────────────────────────────────────────────────────────
step "5/8 节点状态"
sleep 2
api GET /api/v2/node/state | python3 -c '
import json,sys
try: d=json.load(sys.stdin)
except Exception: d={}
for s in ((d or {}).get("states") or []):
    print("  %-16s running=%-4s link=%-4s rtt=%s" % (s.get("node"), s.get("running"), s.get("link"), s.get("rtt")))' 2>/dev/null || true

# ── 6) 订阅 ──────────────────────────────────────────────────────────────────
if [ "${SKIP_MQTT}" != "1" ]; then
    step "6/8 订阅 ${DRV_NODE}/${GROUP} → ${APP_NODE}"
    # 2101 = 重复订阅（已订阅）。GET /api/v2/subscribes 确认存在即算成功 —— 幂等。
    r="$(api POST /api/v2/subscribe \
        "{\"app\":\"${APP_NODE}\",\"driver\":\"${DRV_NODE}\",\"group\":\"${GROUP}\"}")"
    e="$(printf '%s' "$r" | je)"
    subs="$(api GET "/api/v2/subscribes?app=${APP_NODE}")"
    if [ "$e" = "0" ]; then
        ok "建立订阅"
    elif [ "$e" = "2101" ] && printf '%s' "$subs" | grep -q '"subscribed": *true'; then
        ok "订阅已存在（2101，幂等跳过）"
    else
        bad "订阅失败：$(printf '%s' "$r" | head -c 200)（错误码 ${e}）"
    fi
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
[ -n "${got}" ] || bad "15 次重试仍未采到有效值 —— 看下面两个日志（哪个存在看哪个）"

# ── 8) 自证：北向发出 MQTT ───────────────────────────────────────────────────
if [ "${SKIP_MQTT}" != "1" ]; then
    step "8/8 验证北向 MQTT（订阅 '#' —— 实际 topic 带前导斜杠：/${TOPIC_PREFIX}/${APP_NODE}/${DRV_NODE}/${GROUP}）"
    if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
        # ★ stderr 必须丢掉：否则 docker pull 的进度会被当成"收到的报文"打印出来（实测踩过）
        out="$(docker run --rm --network host eclipse-mosquitto:2 \
               mosquitto_sub -h "${BROKER_HOST}" -p "${BROKER_PORT}" \
               -t '#' -C 1 -W 20 -v 2>/dev/null || true)"
        # 收到的内容必须真像一条 MQTT 报文，而不是 docker 的杂音
        if [ -n "${out}" ] && printf '%s' "${out}" | grep -qE '^[^ ]+ \{.*\}|values|metas'; then
            ok "收到 MQTT 报文："
            printf '%s\n' "${out}" | head -3 | cut -c1-200 | sed 's/^/         /'
        else
            bad "20 秒内没收到有效报文（got=\"${out:0:120}\"）"
        fi
    else
        info "docker 不可用，跳过订阅验证；可用 mqtt_e2e --rawsub 或 MQTTX 连 broker 订阅 '#'"
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
echo "  实际话题 : /${TOPIC_PREFIX}/${APP_NODE}/${DRV_NODE}/${GROUP}（订阅 '#' 最保险）"
echo "  日志     : ${BUILD}/smoke.log（bootstrap 起的）或 ${BUILD}/neuron.log（本脚本起的）"
echo "  停止进程 : pkill -f '${BUILD}/neuron'; pkill -f modbus_simulator; docker rm -f nanomq"
exit ${overall}
