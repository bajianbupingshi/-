#!/usr/bin/env bash
# ============================================================================
# D2–D3：在 WSL 里构建 EMQ Neuron（含十余项源码依赖）并做冒烟自检
#
# 用法（在 WSL 里执行）：
#   bash "/mnt/e/桌面项目/agent/嵌入式/4/gateway/tools/bootstrap_neuron_wsl.sh"
#
# 可选环境变量：
#   WORK=~/           工作目录（Neuron 会被克隆到 $WORK/neuron）
#   SKIP_DEPS=1       跳过依赖安装（依赖已装好时用）
#   SKIP_BUILD=1      只装依赖，不构建 Neuron
#   DASH_VER=2.6.3    开源面板版本
#   NO_RUN=1          构建完不启动 Neuron
#
# ─────────────────────────────────────────────────────────────────────────────
# 这个脚本存在的理由：官方 install_dependencies.sh 有三个会让人白干一场的坑，
# 全都是我读过它的源码后确认的（见下方每条注释）。
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail

WORK="${WORK:-$HOME}"
NEURON_DIR="${WORK}/neuron"
DASH_VER="${DASH_VER:-2.6.3}"
BUILD_DIR="${NEURON_DIR}/build"
DASH_URL="https://github.com/emqx/neuron-dashboard/releases/download/${DASH_VER}/neuron-dashboard.zip"

overall=0
step() { printf '\n\033[1m── %s ──\033[0m\n' "$1"; }
ok()   { printf '  [OK]   %s\n' "$1"; }
bad()  { printf '  [FAIL] %s\n' "$1"; overall=1; }

# ── 坑 1：官方脚本的 make 全部没有 -j，8 核机器按单核跑 ─────────────────────
# 缓解：make 会从环境读 MAKEFLAGS，所以在这里导出即可对所有 make 生效。
export MAKEFLAGS="${MAKEFLAGS:--j$(nproc)}"

step "0/6 环境与工具链"
echo "  工作目录 : ${WORK}"
echo "  Neuron   : ${NEURON_DIR}"
echo "  MAKEFLAGS: ${MAKEFLAGS}   ← 官方脚本没写 -j，靠这里补上"
echo "  并发核数 : $(nproc)"

# 坑 5（我自己的）：用 ls /var/lib/dpkg/info 判断包是否安装会漏掉多架构包
# （文件名是 libssl-dev:amd64.list）。改用 dpkg-query，不带那个后缀问题。
is_installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q "install ok installed"; }

REQUIRED_PKGS=(cmake ninja-build g++ gcc make git wget curl unzip pkg-config autoconf automake)
missing=()
for p in "${REQUIRED_PKGS[@]}"; do
    is_installed "$p" || missing+=("$p")
done
if [ ${#missing[@]} -gt 0 ]; then
    echo "  需要安装: ${missing[*]}"
    sudo apt-get install -y "${missing[@]}" \
        || { sudo apt-get update && sudo apt-get install -y "${missing[@]}"; }
else
    ok "构建工具齐全"
fi

# libtool-bin 提供 /usr/bin/libtool（Debian 把它从 libtool 包里拆出来了）。
# libxml2 / cyrus-sasl 走 autogen.sh + configure，通常只需 libtoolize，但缺 libtool
# 时个别 configure 会报 "libtool: command not found"。1 分钟就能排除的风险。
if ! is_installed libtool-bin; then
    echo "  [提示] 未装 libtool-bin（只影响 autotools 项目的边缘路径），顺手装上"
    sudo apt-get install -y libtool-bin || true
fi

# ── 1) 取源码 ────────────────────────────────────────────────────────────────
step "1/6 取 Neuron 源码"
if [ -d "${NEURON_DIR}/.git" ]; then
    ok "已存在，跳过 clone：${NEURON_DIR}"
else
    git clone https://github.com/emqx/neuron.git "${NEURON_DIR}" || { bad "clone 失败"; exit 1; }
    ok "clone 完成"
fi
cd "${NEURON_DIR}" || exit 1

# ── 2) 依赖 ──────────────────────────────────────────────────────────────────
step "2/6 源码依赖（十余项，protobuf / mbedtls / librdkafka / cyrus-sasl …）"
if [ "${SKIP_DEPS:-0}" = "1" ]; then
    echo "  SKIP_DEPS=1 → 跳过"
else
    # 坑 2：官方脚本**没有 set -e**，中途失败也会继续跑到最后，
    #       并且打印 "All dependencies installed successfully." —— 这句不可信。
    #       所以下一步必须逐项验证。
    # 坑 3：官方脚本**不幂等**（git clone 进已存在的目录会失败），所以只在首次跑。
    if [ -f install_dependencies.sh ]; then
        echo "  运行官方 install_dependencies.sh（输出很长，耐心等；protobuf 那个 wget 可能只有几十 KB/s）…"
        bash install_dependencies.sh 2>&1 | tail -40
        echo "  ↑ 官方脚本到此会打印「All dependencies installed successfully.」—— 别信它，下面逐项验。"
    else
        bad "仓库里找不到 install_dependencies.sh"
    fi
fi

# 逐项验证：不信脚本的自我报告，只信文件系在不在
step "2b/6 依赖验证（不看脚本的成功提示，只查产物）"
verify() {  # $1=名字  $2=库文件  $3=头文件
    if [ -e "$2" ] && [ -e "$3" ]; then ok "$1"; else bad "$1（缺 $2 或 $3）"; fi
}
# 注意：openssl 是官方脚本里唯一用 apt 装的（其余全是源码构建装到 /usr/local/lib），
# 所以它的路径必须是发行版路径，不能照抄 /usr/local。
if [ -e /usr/lib/x86_64-linux-gnu/libssl.so ] || [ -e /usr/local/lib/libssl.so ]; then
    ok "openssl（apt 安装）"
else
    bad "openssl（apt 安装）—— 缺 libssl-dev?"
fi
verify "zlog"        /usr/local/lib/libzlog.so          /usr/local/include/zlog.h
verify "jansson"     /usr/local/lib/libjansson.a        /usr/local/include/jansson.h
verify "mbedtls"     /usr/local/lib/libmbedtls.a        /usr/local/include/mbedtls/ssl.h
verify "NanoSDK/nng" /usr/local/lib/libnng.a            /usr/local/include/nng/nng.h
verify "libjwt"      /usr/local/lib/libjwt.a            /usr/local/include/jwt.h
verify "googletest"  /usr/local/lib/libgtest.a          /usr/local/include/gtest/gtest.h
verify "sqlite3"     /usr/local/lib/libsqlite3.so       /usr/local/include/sqlite3.h
verify "protobuf"    /usr/local/lib/libprotobuf.a       /usr/local/include/google/protobuf/message.h
verify "protobuf-c"  /usr/local/lib/libprotobuf-c.a     /usr/local/include/protobuf-c/protobuf-c.h
verify "cyrus-sasl"  /usr/local/lib/libsasl2.so         /usr/local/include/sasl/sasl.h
verify "zstd"        /usr/local/lib/libzstd.so          /usr/local/include/zstd.h
verify "librdkafka"  /usr/local/lib/librdkafka.so       /usr/local/include/librdkafka/rdkafka.h
verify "libxml2"     /usr/local/lib/libxml2.a           /usr/local/include/libxml2/libxml/parser.h

if [ $overall -ne 0 ]; then
    echo
    echo "  有依赖缺失。常见原因与处理："
    echo "   · cyrus-sasl / libxml2 失败 → 多半是 autotools 问题：sudo apt install -y libtool-bin gettext"
    echo "   · protobuf 下载失败（GitHub release 只有几十 KB/s）→ 重跑时会跳过已完成的项，但脚本不幂等；"
    echo "     建议 rm -rf ~/neuron 后重跑本脚本（或单独手动补那一项）"
    echo "   · 修好后用 SKIP_DEPS=1 重跑本脚本，直接进构建阶段"
    echo
fi

[ "${SKIP_BUILD:-0}" = "1" ] && { echo "SKIP_BUILD=1 → 停在依赖阶段"; exit $overall; }

# ── 3) 配置 ──────────────────────────────────────────────────────────────────
step "3/6 配置 Neuron（关键：避开 -O0 陷阱）"
# 读上游 CMakeLists 得到的两个硬事实：
#   a) set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS} -O0")  → Release 构建是 -O0
#   b) $ENV{CFLAGS} **只在 if(NOT DISABLE_WERROR) 里被读** → 同时给
#      CFLAGS="-O2" 和 -DDISABLE_WERROR=ON 时，-O2 被静默丢弃
#   所以必须用 -DCMAKE_C_FLAGS（cache 变量），它会被后面的
#   set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g") 正常拼接。
# KEEP_BUILD=1：保留已有 build 目录（增量重编用；仍然会重新配置以应用最新 flags）
# OPT_FLAGS：优化级别开关，默认 -O2。做「是不是 -O2 引入的」A/B 时传 OPT_FLAGS=-O0
OPT_FLAGS="${OPT_FLAGS:--O2}"

# ── 已知上游缺陷修复：libxml2 静态库丢失自己的依赖 ──────────────────────────
# Neuron 的 CMakeLists 里写的是 target_link_libraries(neuron-base ... xml2)，
# 用的是**裸名 xml2**，CMake 只会生成 -lxml2，不带传递依赖。
# 而官方 install_dependencies.sh 把 libxml2 编成**静态库**（--enable-shared=no），
# 它的 Libs.private 是 "-lz -llzma -lm"（已从 /usr/local/lib/pkgconfig/libxml-2.0.pc 读到）。
# 于是最终链接必然报：
#   libneuron-base.so: undefined reference to `inflate' / `deflate' / `crc32' / `lzma_code' …
# 上游 CI 用的是发行版共享 libxml2，所以从没暴露。
# 修法：把 zlib 与 lzma 显式补进链接行。
# 用 -Wl,--no-as-needed 是为了免疫 `--as-needed` 下的链接顺序问题
# （linker flags 会被 CMake 放在 objects 之前，--as-needed 时可能被丢掉）。
EXTRA_LINK_FLAGS="-Wl,--no-as-needed -lz -llzma"

if [ "${KEEP_BUILD:-0}" = "1" ] && [ -f "${BUILD_DIR}/CMakeCache.txt" ]; then
    echo "  KEEP_BUILD=1 → 保留已有 build 目录（增量编译，但仍重新配置以应用 flags）"
else
    rm -rf "${BUILD_DIR}"
fi
echo "  优化级别 OPT_FLAGS=${OPT_FLAGS}"
cmake -B "${BUILD_DIR}" -S "${NEURON_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DDISABLE_ASAN=ON \
    -DDISABLE_WERROR=ON \
    -DENABLE_DATALAYERS=OFF \
    -DCMAKE_C_FLAGS="${OPT_FLAGS}" \
    -DCMAKE_CXX_FLAGS="${OPT_FLAGS}" \
    -DCMAKE_SHARED_LINKER_FLAGS="${EXTRA_LINK_FLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS="${EXTRA_LINK_FLAGS}" || { bad "cmake 配置失败"; exit 1; }

# 自证：优化级别必须真的进了命令行，否则后面所有性能数字都不可信
if grep -q -- "${OPT_FLAGS}" "${BUILD_DIR}/compile_commands.json" 2>/dev/null; then
    n_opt=$(grep -c -- "${OPT_FLAGS}" "${BUILD_DIR}/compile_commands.json")
    ok "确认 ${OPT_FLAGS} 已进入命令行（compile_commands.json 命中 ${n_opt} 处）"
else
    bad "${OPT_FLAGS} 没进 compile_commands.json，优化没生效"
fi
# 只要还是 Debug 构建，就不该出现 -O0（-O0 是被硬编码进 CMAKE_C_FLAGS_RELEASE 的）
if [ "${OPT_FLAGS}" != "-O0" ] && grep -q -- '\-O0' "${BUILD_DIR}/compile_commands.json" 2>/dev/null; then
    bad "命令行里同时出现 -O0 —— 优化级别被覆盖，性能数据不可用"
fi
# 顺手记录事实：Neuron 的插件 kafka 是无条件编译的（CMakeLists 里没有 if 包裹）
grep -q "add_subdirectory(plugins/kafka)" CMakeLists.txt \
    && echo "  备注：plugins/kafka 在 CMakeLists 里无条件编译 ⇒ librdkafka 依赖不能省"

# ── 4) 构建 ──────────────────────────────────────────────────────────────────
step "4/6 构建 Neuron"
# 关键：完整日志落盘，失败时**从日志里精确抽出错误**，而不是 tail 一段警告了事
# （上一版就是 tail -25 把真错误截掉、只留下 strncpy 警告，白跑一轮）
BUILD_LOG="${NEURON_DIR}/build.log"
cmake --build "${BUILD_DIR}" -j"$(nproc)" > "${BUILD_LOG}" 2>&1
rc_build=$?

if [ $rc_build -ne 0 ]; then
    bad "构建失败（完整日志：${BUILD_LOG}）"
    echo
    echo "  ── 失败的 target ──"
    grep -E "^FAILED: " "${BUILD_LOG}" | head -20 || echo "  （日志里没有 ninja 的 FAILED: 行）"
    echo
    echo "  ── 错误行（error: / undefined reference / Error N）──"
    grep -nE "error:|undefined reference to|Error [0-9]+" "${BUILD_LOG}" | head -25 || echo "  （没匹配到）"
    echo
    echo "  ── 首个失败目标的完整编译输出 ──"
    first_failed="$(grep -n '^FAILED:' "${BUILD_LOG}" | head -1 | cut -d: -f1)"
    if [ -n "${first_failed}" ]; then
        sed -n "${first_failed},$((first_failed + 30))p" "${BUILD_LOG}"
    else
        tail -40 "${BUILD_LOG}"
    fi
    echo
    echo "  调试建议：加 KEEP_BUILD=1 复跑本脚本，可直接对单个失败目标增量重编，不用全量重来："
    echo "    KEEP_BUILD=1 SKIP_DEPS=1 bash \"\$0\""
    exit 1
fi

if [ -x "${BUILD_DIR}/neuron" ]; then
    ok "构建产物：${BUILD_DIR}/neuron"
    # 自证上面那条链接修复真的生效：libneuron-base.so 应当已带上 libz / liblzma 的 DT_NEEDED
    if command -v readelf >/dev/null 2>&1 && [ -f "${BUILD_DIR}/libneuron-base.so" ]; then
        needed="$(readelf -d "${BUILD_DIR}/libneuron-base.so" 2>/dev/null | grep -oE 'libz\.so[^]]*|liblzma\.so[^]]*' | sort -u | tr '\n' ' ')"
        if [ -n "${needed}" ]; then
            ok "libneuron-base.so 的 DT_NEEDED 含：${needed}"
        else
            bad "libneuron-base.so 的 DT_NEEDED 里没有 libz/liblzma（链接修复可能没生效）"
        fi
    fi
    ls -la "${BUILD_DIR}/plugins"/*.so 2>/dev/null | head -10
    [ -x "${BUILD_DIR}/simulator/modbus_simulator" ] \
        && ok "自带 Modbus 模拟器：${BUILD_DIR}/simulator/modbus_simulator"
else
    bad "构建命令返回 0 但没生成 neuron 可执行文件"
    exit 1
fi

# ── 5) 开源面板 ──────────────────────────────────────────────────────────────
step "5/6 开源面板 neuron-dashboard ${DASH_VER}"
if [ -f "${BUILD_DIR}/dist/index.html" ]; then
    ok "面板已就位"
else
    # GitHub release 在这条链路上可能只有几十 KB/s，所以给足重试
    for attempt in 1 2 3; do
        echo "  下载尝试 ${attempt}/3 …"
        if wget -q --timeout=60 -O /tmp/neuron-dashboard.zip "${DASH_URL}"; then break; fi
    done
    if [ -s /tmp/neuron-dashboard.zip ] && unzip -q -o /tmp/neuron-dashboard.zip -d "${BUILD_DIR}"; then
        [ -f "${BUILD_DIR}/dist/index.html" ] && ok "面板解压完成" || bad "面板解压后找不到 dist/index.html"
    else
        bad "面板下载失败（不影响 REST API 与采集功能，只影响 Web 界面）"
    fi
fi

# ── 6) 冒烟自检 ──────────────────────────────────────────────────────────────
step "6/6 冒烟自检"
if [ "${NO_RUN:-0}" = "1" ]; then
    echo "  NO_RUN=1 → 跳过启动"
else
    pkill -f "${BUILD_DIR}/neuron" 2>/dev/null
    ( cd "${BUILD_DIR}" && ./neuron --log > "${BUILD_DIR}/smoke.log" 2>&1 & )
    echo "  等待端口 7000 监听 …"
    up=0
    for _ in $(seq 1 20); do
        if curl -s -o /dev/null --max-time 2 http://127.0.0.1:7000; then up=1; break; fi
        sleep 1
    done
    if [ $up -eq 1 ]; then
        ok "HTTP 7000 已响应"

        # 免鉴权探针
        ping="$(curl -s --max-time 5 http://127.0.0.1:7000/api/v2/ping)"
        [ -n "${ping}" ] && ok "GET /api/v2/ping → ${ping}"

        # 真实 API 调用：admin / 0000 登录取 JWT
        # ★ 字段名是 "pass"，不是 "password"（读 src/parser/neu_json_login.c 确认）：
        #   写成 password 会让 neu_json_decode_login_req 解码失败，
        #   被 NEU_PROCESS_HTTP_REQUEST 宏回成 {"error": 1002} = BODY_IS_WRONG，
        #   很容易误判成"密码不对"。
        resp="$(curl -s --max-time 5 -X POST http://127.0.0.1:7000/api/v2/login \
                -H 'Content-Type: application/json' \
                -d '{"name":"admin","pass":"0000"}')"
        if printf '%s' "$resp" | grep -q '"token"'; then
            ok "REST 登录成功（拿到 token）"
        else
            echo "  [注意] 登录返回：${resp:0:200}"
            case "$resp" in
                *1002*) echo "         1002 = NEU_ERR_BODY_IS_WRONG → 请求体字段名不对（要 {\"name\",\"pass\"}）" ;;
                *1009*) echo "         1009 = NEU_ERR_INVALID_USER_OR_PASSWORD → 用户名或密码错" ;;
                *1012*) echo "         1012 = NEU_ERR_INVALID_PASSWORD_LEN → 密码长度不合规" ;;
                *)      echo "         错误码表见 include/neuron/errcodes.h" ;;
            esac
        fi
        echo
        echo "  Neuron 已在后台运行："
        echo "    Web 面板 : http://localhost:7000   （admin / 0000）"
        echo "    日志     : ${BUILD_DIR}/smoke.log"
        echo "    停止     : pkill -f '${BUILD_DIR}/neuron'"
    else
        bad "端口 7000 20 秒内未监听，看日志："
        tail -30 "${BUILD_DIR}/smoke.log"
    fi
fi

echo
echo "════════════════════════════════════════════════════════════"
if [ $overall -eq 0 ]; then
    echo " ✔ D2–D3 完成"
else
    echo " ✘ 存在失败项（见上方 [FAIL]）"
fi
echo "════════════════════════════════════════════════════════════"
exit $overall
