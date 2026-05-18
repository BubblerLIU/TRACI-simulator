#!/bin/bash

# 编译并启动 fat-tree 拓扑中的所有节点程序

set -euo pipefail

# ========== 基本配置 ==========

# 目录结构
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="$ROOT_DIR/.fat_tree_config"
SRC_DIR="$ROOT_DIR/src"
APP_DIR="/traci"

# 通过目录名选择必需的 workload 集
usage() {
    echo "Usage: $0 <WORKLOAD_DIR> [-b|-r|-i|-t] [GPU_START_DELAY_SECONDS]"
}

if [ $# -lt 1 ] || [ $# -gt 3 ]; then
    usage
    exit 1
fi

GPU_START_DELAY=3
WORKLOAD_SET="$1"
RUN_MODE="-b"
delay_specified=0
mode_specified=0
shift

for arg in "$@"; do
    if [[ "$arg" =~ ^[0-9]+$ ]]; then
        if [ "$delay_specified" -eq 1 ]; then
            usage
            exit 1
        fi
        GPU_START_DELAY="$arg"
        delay_specified=1
    elif [ "$arg" = "-b" ] || [ "$arg" = "-r" ] ||
        [ "$arg" = "-i" ] || [ "$arg" = "-t" ]; then
        if [ "$mode_specified" -eq 1 ]; then
            usage
            exit 1
        fi
        RUN_MODE="$arg"
        mode_specified=1
    else
        usage
        exit 1
    fi
done

WORKLOAD_DIR="$ROOT_DIR/workloads/$WORKLOAD_SET"

# 检查配置文件是否存在
if [ ! -f "$CONFIG_FILE" ]; then
    echo "Error: Configuration file not found at $CONFIG_FILE"
    echo "Please run script/setup.sh first."
    exit 1
fi

# 读取配置文件
source "$CONFIG_FILE" 2>/dev/null
if [ -z "${GPU_NUM:-}" ] || [ -z "${LEAF_NUM:-}" ] ||
    [ -z "${SPINE_NUM:-}" ]; then
    echo "Error: Invalid configuration file (missing topology parameters)"
    exit 1
fi

if [ $((GPU_NUM % LEAF_NUM)) -ne 0 ]; then
    echo "Error: GPU_NUM must be a multiple of LEAF_NUM"
    exit 1
fi

GPU_PER_LEAF=$((GPU_NUM / LEAF_NUM))

if [ ! -d "$WORKLOAD_DIR" ]; then
    echo "Error: workload directory not found at $WORKLOAD_DIR"
    exit 1
fi

# 检查配置信息
echo "======== FAT-TREE-TEST ========"
echo "$GPU_NUM GPUs, $LEAF_NUM leaf switches, $SPINE_NUM spine switches"
echo "$GPU_PER_LEAF GPUs under a leaf switch"
echo "GPU request start delay: ${GPU_START_DELAY}s"
echo "Workload directory: $WORKLOAD_SET"
echo "Run mode: $RUN_MODE"
echo "==============================="

# ========== 设置容器 ==========

# 检查容器是否存在且在运行
require_container() {
    local container="$1"
    local running

    running="$(docker container inspect -f '{{.State.Running}}' \
        "$container" 2>/dev/null || true)"
    if [ -z "$running" ]; then
        echo "Error: container $container not found"
        exit 1
    fi
    if [ "$running" != "true" ]; then
        echo "Error: container $container is not running"
        exit 1
    fi
}

# 向旧程序发送终止信号并等待其完成清理
stop_program() {
    local container="$1"
    local binary="$2"

    docker exec "$container" sh -c "
        find_pids() {
            for proc in /proc/[0-9]*; do
                [ -r \"\$proc/comm\" ] || continue
                if [ \"\$(cat \"\$proc/comm\" 2>/dev/null)\" = '$binary' ]; then
                    printf '%s\n' \"\${proc##*/}\"
                fi
            done
        }

        pids=\"\$(find_pids)\"
        if [ -n \"\$pids\" ]; then
            kill -TERM \$pids 2>/dev/null || true
            i=0
            while [ -n \"\$(find_pids)\" ]; do
                if [ \"\$i\" -ge 10 ]; then
                    pids=\"\$(find_pids)\"
                    [ -z \"\$pids\" ] || kill -KILL \$pids 2>/dev/null || true
                    break
                fi
                sleep 1
                i=\$((i + 1))
            done
        fi
    "
}

# 将源文件复制到容器并编译
prepare_container() {
    local container="$1"
    local main_src="$2"
    local binary="$3"

    require_container "$container"

    docker exec "$container" mkdir -p "$APP_DIR/src" "$APP_DIR/bin" \
        "$APP_DIR/logs"
    stop_program "$container" "$binary"

    docker cp "$SRC_DIR/common.h" "$container:$APP_DIR/src/common.h"
    docker cp "$SRC_DIR/common.c" "$container:$APP_DIR/src/common.c"
    docker cp "$SRC_DIR/$main_src" "$container:$APP_DIR/src/$main_src"

    docker exec "$container" gcc -Wall -Wextra -I"$APP_DIR/src" \
        "$APP_DIR/src/$main_src" "$APP_DIR/src/common.c" \
        -lpcap -lpthread -o "$APP_DIR/bin/$binary"
}

# 将测试数据复制到 GPU 容器
copy_workload_if_present() {
    local gpu_id="$1"
    local container="gpu$gpu_id"
    local workload_name="workload_gpu$gpu_id"
    local workload_src=""

    if [ -f "$ROOT_DIR/$workload_name" ]; then
        workload_src="$ROOT_DIR/$workload_name"
    elif [ -f "$WORKLOAD_DIR/$workload_name.txt" ]; then
        workload_src="$WORKLOAD_DIR/$workload_name.txt"
    fi

    if [ -n "$workload_src" ]; then
        docker cp "$workload_src" "$container:$APP_DIR/$workload_name"
    else
        docker exec "$container" rm -f "$APP_DIR/$workload_name"
    fi
}

# 在容器中启动程序
start_node() {
    local container="$1"
    local binary="$2"
    local mode_arg="$3"
    shift 3

    docker exec -d -w "$APP_DIR" "$@" "$container" sh -c \
        "exec stdbuf -oL -eL '$APP_DIR/bin/$binary' '$mode_arg' > '$APP_DIR/logs/$container.log' 2>&1"
}

gpu_workload_complete() {
    local gpu_id="$1"
    local container="gpu$gpu_id"
    local log_file="$APP_DIR/logs/$container.log"

    docker exec "$container" sh -c \
        "test -f '$log_file' && grep -q 'workload complete' '$log_file'"
}

wait_for_gpu_completion() {
    local all_done
    local completed
    local start
    local elapsed
    local timeout

    echo "Waiting for all GPUs to receive responses..."
    start="$(date +%s)"
    timeout="${TEST_WAIT_TIMEOUT_SECONDS:-600}"
    while true; do
        all_done=1
        completed=0

        for ((i=0; i<GPU_NUM; i++)); do
            if gpu_workload_complete "$i"; then
                completed=$((completed + 1))
            else
                all_done=0
            fi
        done

        if [ "$all_done" -eq 1 ]; then
            echo "All GPUs completed workload responses."
            return
        fi

        elapsed=$(($(date +%s) - start))
        if [ "$elapsed" -ge "$timeout" ]; then
            echo "Error: timed out after ${timeout}s waiting for responses."
            echo "Incomplete GPUs:"
            for ((i=0; i<GPU_NUM; i++)); do
                if ! gpu_workload_complete "$i"; then
                    echo "  gpu$i"
                fi
            done
            return 1
        fi

        echo "Completed GPUs: $completed/$GPU_NUM"
        sleep 2
    done
}

echo "Copying sources and compiling inside containers..."
for ((i=0; i<LEAF_NUM; i++)); do
    prepare_container "leaf$i" "leaf.c" "leaf"
done

for ((i=0; i<SPINE_NUM; i++)); do
    prepare_container "spine$i" "spine.c" "spine"
done

for ((i=0; i<GPU_NUM; i++)); do
    prepare_container "gpu$i" "gpu.c" "gpu"
    copy_workload_if_present "$i"
done
echo "Compilation done"

echo "Starting leaf switches..."
for ((i=0; i<LEAF_NUM; i++)); do
    start_node "leaf$i" "leaf" "$RUN_MODE"
done

echo "Starting spine switches..."
for ((i=0; i<SPINE_NUM; i++)); do
    start_node "spine$i" "spine" "$RUN_MODE"
done

sleep 1

echo "Starting GPUs..."
for ((i=0; i<GPU_NUM; i++)); do
    start_node "gpu$i" "gpu" "$RUN_MODE" \
        -e "GPU_START_DELAY=$GPU_START_DELAY"
done

echo "Test programs started"
echo "Logs are stored in each container under $APP_DIR/logs/<container>.log"
wait_for_gpu_completion
echo "Simulation complete. You can now run script/logs.sh to save logs."
