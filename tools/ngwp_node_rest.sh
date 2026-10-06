#!/usr/bin/env bash
# 对真 Neuron 完成自研 NGWP 驱动插件的建节点全流程（幂等，可重复跑）。
#
# 前置（均已实测，见 plugins/README.md）：
#   1. ~/neuron/build/neuron 已加载 libgw_driver.so（default_plugins.json 登记），
#      且 schema 在 build/plugins/schema/ngwp-sim.json
#   2. 设备在跑：gw_sim --port 1502（本脚本自动拉起 build-wsl-plugin 里的）
#   3. 本 daily 版的 /api/v2/ping 只认 POST（旧版是 GET —— 实测差异）
#
# 已知幂等码：2002 NODE_EXIST / 2104 GROUP_EXIST / 2202 TAG_EXIST，重复跑全绿。
set -uo pipefail

GW="${GW:-/mnt/e/桌面项目/agent/嵌入式/4/gateway}"
API="http://127.0.0.1:7000/api/v2"
NODE="ngwp1"

pgrep -f "gw_sim --port 1502$" > /dev/null || \
  ( cd "$GW/build-wsl-plugin/apps/sim" && nohup ./gw_sim --port 1502 > /dev/null 2>&1 & )
sleep 1

TOKEN=$(curl -s --max-time 5 -X POST "$API/login" -H "Content-Type: application/json" \
  -d "{\"name\":\"admin\",\"pass\":\"0000\"}" \
  | python3 -c "import json,sys; print(json.load(sys.stdin)['token'])")
[ -z "$TOKEN" ] && { echo "[FAIL] 登录失败"; exit 1; }
echo "登录 OK"
AUTH=(-H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json")

api() { curl -s --max-time 8 -X "$1" "$API$2" "${AUTH[@]}" ${3:+-d "$3"}; }

echo "建节点: $(api POST /node "{\"plugin\":\"NGWP Sim\",\"name\":\"$NODE\"}")"
api POST /node/ctl "{\"node\":\"$NODE\",\"cmd\":1}" > /dev/null   # 容错停（2006/2007 忽略）
echo "写参数: $(api POST /node/setting "{\"node\":\"$NODE\",\"params\":{\"host\":\"127.0.0.1\",\"port\":1502,\"timeout\":1000}}")"
echo "建组:   $(api POST /group "{\"node\":\"$NODE\",\"group\":\"g1\",\"interval\":1000}")"
echo "建点位: $(api POST /tags "{\"node\":\"$NODE\",\"group\":\"g1\",\"tags\":[{\"type\":3,\"name\":\"t0\",\"attribute\":1,\"address\":\"1!40001\"},{\"type\":3,\"name\":\"t1\",\"attribute\":1,\"address\":\"1!40002\"},{\"type\":3,\"name\":\"t2\",\"attribute\":1,\"address\":\"1!40003\"},{\"type\":3,\"name\":\"t3\",\"attribute\":1,\"address\":\"1!40004\"}]}")"
echo "启动:   $(api POST /node/ctl "{\"node\":\"$NODE\",\"cmd\":0}")"
sleep 3
for i in 1 2 3; do
    echo "第 $i 次读: $(api POST /read "{\"node\":\"$NODE\",\"group\":\"g1\"}")"
    sleep 1
done
echo "节点状态: $(api GET /node/state)"
