#!/usr/bin/env bash
# pxp-test.sh —— 一键启动 desync 复现环境（服务器 + 客户端）
#
# 用法:
#   ./pxp-test.sh setup                 首次/重建沙盒环境（幂等，不覆盖已有效配置）
#   ./pxp-test.sh run <sav文件名> [客户端数]
#                                       启动服务器 + N 个客户端（默认 1），
#                                       阻塞监视直到出现 desync 报告或超时
#   ./pxp-test.sh loop <sav文件名> [轮数]
#                                       服务器一直跑，客户端按轮加入/退出，
#                                       任一轮出现 desync 即停（默认 6 轮）
#   ./pxp-test.sh stop                  杀掉所有 openttd 测试进程
#   ./pxp-test.sh status                查看当前进程/连接状态
#
# 环境变量:
#   BIN=...          指定二进制（默认 <项目>/build/openttd）
#   TIMEOUT=300      run 监视超时秒数（默认 300）
#   SAVDIR=...       存档所在目录（默认 <项目>/desync）

set -u

PROJ="/home/pulsex/Projects/OpenTTD-patches"
BIN="${BIN:-$PROJ/build/openttd}"
SAVDIR="${SAVDIR:-$PROJ/desync}"
TIMEOUT="${TIMEOUT:-300}"
BASE="/tmp/pxp-repro"                      # 沙盒根目录
CLIENT_NAME="ReproClient"
RESULTS="$PROJ/repro-env/results"

srv_data="$BASE/server"                    # XDG_DATA_HOME 下会生成 openttd/
cli_data="$BASE/client"
srv_cfg="$BASE/server-cfg"
cli_cfg="$BASE/client-cfg"

log()  { echo "[pxp-test] $*"; }
die()  { echo "[pxp-test] 错误: $*" >&2; exit 1; }

# ---------- setup ----------
setup() {
    [ -x "$BIN" ] || die "二进制不存在: $BIN（先 cmake --build build）"
    mkdir -p "$srv_data/openttd/save/autosave" "$srv_cfg/openttd" \
             "$cli_data/openttd/save/autosave" "$cli_cfg/openttd" "$RESULTS"
    [ -d ~/.local/share/openttd/baseset ] && ln -sfn ~/.local/share/openttd/baseset "$srv_data/openttd/baseset" && ln -sfn ~/.local/share/openttd/baseset "$cli_data/openttd/baseset"
    [ -d ~/.local/share/openttd/content_download ] && ln -sfn ~/.local/share/openttd/content_download "$srv_data/openttd/content_download" && ln -sfn ~/.local/share/openttd/content_download "$cli_data/openttd/openttd-content-tmp" 2>/dev/null; ln -sfn ~/.local/share/openttd/content_download "$cli_data/openttd/content_download"

    # openttd.cfg：若沙盒里还没有就从用户配置复制一份
    for f in "$srv_cfg/openttd/openttd.cfg" "$cli_cfg/openttd/openttd.cfg"; do
        if [ ! -s "$f" ] && [ -s ~/.config/openttd/openttd.cfg ]; then
            cp ~/.config/openttd/openttd.cfg "$f"
        fi
        [ -s "$f" ] || : > "$f"     # 实在没有就给个空文件，用默认值
    done

    # client_name 在 private.cfg（config 目录）里 —— 之前连接失败就是因为这里为空
    printf '[network]\nclient_name = %s\n' "$CLIENT_NAME" > "$cli_cfg/openttd/private.cfg"
    # 兼容部分版本把 private.cfg 放数据目录
    printf '[network]\nclient_name = %s\n' "$CLIENT_NAME" > "$cli_data/openttd/private.cfg"

    log "沙盒就绪: $BASE"
}

# ---------- stop ----------
stop() {
    pkill -9 -x openttd 2>/dev/null
    sleep 1
    if pgrep -x openttd >/dev/null; then
        pgrep -x openttd | xargs -r kill -9 2>/dev/null
    fi
    pgrep -x openttd >/dev/null && die "仍有残留进程" || log "全部已停止"
}

# ---------- status ----------
status() {
    echo "进程:"; pgrep -ax openttd || echo "  无"
    echo "连接:"; ss -tn 2>/dev/null | grep -E "397[89]" || echo "  无"
}

wait_port() {
    for _ in $(seq 1 90); do
        ss -tln 2>/dev/null | grep -q ":3979 " && return 0
        sleep 1
    done
    return 1
}

wait_join() {
    # 服务器端出现 ESTAB 连接即视为客户端已加入
    for _ in $(seq 1 60); do
        ss -tn 2>/dev/null | grep -q "ESTAB.*:3979" && return 0
        pgrep -x openttd >/dev/null || die "客户端进程退出（查看 $BASE/client-*.log）"
        sleep 1
    done
    return 1
}

# ---------- run ----------
run() {
    local sav="$1" ncli="${2:-1}"
    local savpath
    savpath="$SAVDIR/$sav"; [[ "$sav" == */* ]] && savpath="$sav"
    [ -s "$savpath" ] || die "存档不存在: $savpath"

    setup
    stop

    local ts; ts=$(date +%Y%m%dT%H%M%S)
    local rundir="$RESULTS/run-$ts"
    mkdir -p "$rundir"
    log "结果目录: $rundir"

    # 清理上次的证据文件，避免误判
    find "$srv_data" "$cli_data" -maxdepth 3 \( -name "desync-*.log" -o -name "random-out-*" \) -delete 2>/dev/null

    log "启动服务器: $savpath"
    cp "$savpath" "$srv_data/openttd/save/input.sav"
    ( cd "$srv_data/openttd" && stdbuf -o0 -e0 env \
        XDG_DATA_HOME="$srv_data" XDG_CONFIG_HOME="$srv_cfg" \
        "$BIN" -D -g "save/input.sav" -d "desync=1:statecsum=1" \
        > "$BASE/server-stdout.log" 2>&1 ) &
    wait_port || die "服务器未能监听 3979（查看 $BASE/server-stdout.log）"
    log "服务器已监听 3979"

    for i in $(seq 1 "$ncli"); do
        ( cd "$cli_data/openttd" && stdbuf -o0 -e0 env \
            XDG_DATA_HOME="$cli_data" XDG_CONFIG_HOME="$cli_cfg" \
            "$BIN" -v null:until_exit=true -s null -m null -n "127.0.0.1#255" \
            -d "desync=1:statecsum=1" > "$BASE/client-$i-stdout.log" 2>&1 ) &
        log "客户端 $i 已启动，等待加入..."
        wait_join || die "客户端 $i 未能在 60s 内加入（查看 $BASE/client-$i-stdout.log）"
        log "客户端 $i 已加入 ✓"
        sleep 2
    done

    # 监视 desync 报告文件
    local waited=0
    while [ $waited -lt $TIMEOUT ]; do
        local hit
        hit=$(find "$srv_data" "$cli_data" -maxdepth 2 -name "desync-*.log" 2>/dev/null | head -1)
        if [ -n "$hit" ]; then
            log "!!! desync 已触发: $hit"
            sleep 3   # 等双端报告都写完
            collect "$rundir"
            log "证据已收集到 $rundir，进程保持运行（可 stop 停止）"
            return 0
        fi
        if ! pgrep -x openttd >/dev/null; then
            log "!!! 进程全部退出（可能是崩溃），收集现有日志"
            collect "$rundir"
            return 1
        fi
        sleep 2; waited=$((waited+2))
    done
    log "超时 ${TIMEOUT}s 未复现 desync。进程保持运行。"
    collect "$rundir" 2>/dev/null || true
    return 2
}

collect() {
    local rundir="$1"
    cp -f "$BASE"/server-stdout.log "$rundir/" 2>/dev/null
    cp -f "$BASE"/client-*-stdout.log "$rundir/" 2>/dev/null
    find "$srv_data" "$cli_data" -maxdepth 3 -name "desync-*.log"    -exec cp -f {} "$rundir/" \; 2>/dev/null
    find "$srv_data" "$cli_data" -maxdepth 3 -name "inconsistency-*" -exec cp -f {} "$rundir/" \; 2>/dev/null
    find "$srv_data" "$cli_data" -maxdepth 3 -name "random-out-*"    -exec cp -f {} "$rundir/" \; 2>/dev/null
    ls -la "$rundir" | head -20
}

case "${1:-}" in
    setup)  setup ;;
    run)    shift; run "${1:?用法: run <sav文件名> [客户端数]}" "${2:-1}" ;;
    stop)   stop ;;
    status) status ;;
    *)      grep '^#' "$0" | head -20; exit 1 ;;
esac
