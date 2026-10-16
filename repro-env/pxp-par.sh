#!/usr/bin/env bash
# pxp-par.sh —— 多存档并行复现 desync
# 每个存档一个独立服务器（不同端口），客户端按轮次加入/退出，
# 任一实例出现 desync 报告即收集证据并停止该实例。
#
# 用法: ./pxp-par.sh [轮数] [sav1 sav2 ...]
#   缺省轮数=6；缺省存档= <项目>/desync/*.sav 全部
#   每轮: 客户端加入 90 秒 → 退出 → 下一轮
set -u

PROJ="/home/pulsex/Projects/OpenTTD-patches"
BIN="$PROJ/build/openttd"
SAVDIR="$PROJ/desync"
BASE="/tmp/pxp-par"
RESULTS="$PROJ/repro-env/results"
ROUNDS="${1:-6}"; shift || true
SAVS=("$@")
[ ${#SAVS[@]} -eq 0 ] && SAVS=("$SAVDIR"/desync-server-*.sav)
mkdir -p "$BASE" "$RESULTS"
CLIENT_NAME="ReproClient"
PORT0=3979

log()  { echo "[pxp-par $(date +%H:%M:%S)] $*"; }

pids=()   # instance pids: srv_<i>, cli_<i>

kill_cli() { # 杀掉实例 $1 的客户端
    local i=$1
    [ -n "${cli_pid[$i]:-}" ] && kill -9 "${cli_pid[$i]}" 2>/dev/null
    unset "cli_pid[$i]"
}
collect() { # 实例 $1 desync 后收集
    local i=$1 d="$RESULTS/par-$(date +%Y%m%dT%H%M%S)-i$i"
    mkdir -p "$d"
    cp -f "$BASE/p$i"/srv-stdout.log "$BASE/p$i"/cli-stdout.log "$d/" 2>/dev/null
    find "$BASE/p$i" -maxdepth 4 \( -name "desync-*.log" -o -name "inconsistency-*" -o -name "random-out-*" -o -name "*.sav" -name "inconsistency*" \) -exec cp -f {} "$d/" \; 2>/dev/null
    log "实例 $i 证据收集: $d"
    ls "$d" | head -10
}

declare -A cli_pid alive
N=0
for sav in "${SAVS[@]}"; do
    i=$N; N=$((N+1))
    port=$((PORT0+i*2))
    echo "$i $port $sav" > "$BASE/instances.txt.new$i"; cat "$BASE/instances.txt" "$BASE/instances.txt.new$i" 2>/dev/null > "$BASE/instances.txt.t"; mv "$BASE/instances.txt.t" "$BASE/instances.txt"
    # 沙盒
    rm -rf "$BASE/p$i"; mkdir -p "$BASE/p$i/srv/openttd/save/autosave" "$BASE/p$i/srv/cfg/openttd" \
                            "$BASE/p$i/cli/openttd/save/autosave" "$BASE/p$i/cli/cfg/openttd"
    ln -sfn ~/.local/share/openttd/baseset "$BASE/p$i/srv/openttd/baseset"
    ln -sfn ~/.local/share/openttd/baseset "$BASE/p$i/cli/openttd/baseset"
    ln -sfn ~/.local/share/openttd/content_download "$BASE/p$i/srv/openttd/content_download"
    ln -sfn ~/.local/share/openttd/content_download "$BASE/p$i/cli/openttd/content_download"
    for f in "$BASE/p$i/srv/cfg/openttd/openttd.cfg" "$BASE/p$i/cli/cfg/openttd/openttd.cfg"; do
        [ -s "$f" ] || { [ -s ~/.config/openttd/openttd.cfg ] && cp ~/.config/openttd/openttd.cfg "$f" || : > "$f"; }
    done
    printf '[network]\nclient_name = %s\n' "$CLIENT_NAME" > "$BASE/p$i/cli/cfg/openttd/private.cfg"
    printf '[network]\nclient_name = %s\n' "$CLIENT_NAME" > "$BASE/p$i/cli/openttd/private.cfg"
    cp "$sav" "$BASE/p$i/srv/openttd/save/input.sav"
    alive[$i]=1

    log "[$i] 启动服务器 port=$port sav=$(basename "$sav")"
    ( cd "$BASE/p$i/srv/openttd" && exec stdbuf -o0 -e0 env \
        XDG_DATA_HOME="$BASE/p$i/srv" XDG_CONFIG_HOME="$BASE/p$i/srv/cfg" \
        "$BIN" -D "127.0.0.1:$port" -g save/input.sav -d "desync=2:statecsum=1" \
        > "$BASE/p$i/srv-stdout.log" 2>&1 ) & srv_pid[$i]=$!

    # 等端口监听
    ok=0
    for _ in $(seq 1 90); do ss -tln 2>/dev/null | grep -q ":$port " && { ok=1; break; }; sleep 1; done
    [ $ok -eq 1 ] && log "[$i] 服务器监听 $port ✓" || { log "[$i] 服务器未监听，跳过"; kill -9 "${srv_pid[$i]}" 2>/dev/null; alive[$i]=0; }
done

# 轮次循环
for round in $(seq 1 "$ROUNDS"); do
    nactive=$(for i in $(seq 0 $((N-1))); do [ "${alive[$i]}" = 1 ] && echo x; done | wc -l)
    [ "$nactive" -eq 0 ] && { log "无活动实例，结束"; break; }
    log "===== 第 $round 轮 (活动实例 $nactive/$N) ====="
    # 启动客户端（每实例 2 个）
    for i in $(seq 0 $((N-1))); do
        [ "${alive[$i]}" = 1 ] || continue
        port=$((PORT0+i*2))
        ( cd "$BASE/p$i/cli/openttd" && exec stdbuf -o0 -e0 env \
            XDG_DATA_HOME="$BASE/p$i/cli" XDG_CONFIG_HOME="$BASE/p$i/cli/cfg" \
            "$BIN" -v null:until_exit=true -s null -m null -n "127.0.0.1:$port#255" \
            -d "desync=2:statecsum=1" > "$BASE/p$i/cli-stdout.log" 2>&1 ) &
        cli_pid[$i]=$!
    done
    # 等 90 秒，期间每 10 秒查 desync
    for t in $(seq 1 15); do
        sleep 10
        for i in $(seq 0 $((N-1))); do
            [ "${alive[$i]}" = 1 ] || continue
            if find "$BASE/p$i" -maxdepth 4 -name "desync-*.log" 2>/dev/null | grep -q .; then
                log "!!! [$i] desync 触发 (第 $round 轮)"
                sleep 3; collect "$i"
                kill -9 "${srv_pid[$i]}" "${cli_pid[$i]}" 2>/dev/null
                alive[$i]=0
            fi
        done
    done
    # 杀客户端，清客户端 random-out（省磁盘），保留服务器
    for i in $(seq 0 $((N-1))); do
        [ "${alive[$i]}" = 1 ] || continue
        kill_cli "$i"
        find "$BASE/p$i/cli/openttd/save/autosave" -name "random-out-*" -delete 2>/dev/null
        find "$BASE/p$i/cli/openttd/save/autosave" -name "inconsistency-*" -delete 2>/dev/null
        # 磁盘保护：截断服务器端过大的校验流（保留 fd，进程不受影响）
        find "$BASE/p$i/srv/openttd/save/autosave" -name "random-out-*" -size +800M -exec truncate -s 0 {} \; 2>/dev/null
    done
done

for i in $(seq 0 $((N-1))); do [ "${alive[$i]}" = 1 ] && kill -9 "${srv_pid[$i]}" 2>/dev/null; done
log "结束。"
