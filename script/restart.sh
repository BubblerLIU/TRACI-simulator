#!/bin/bash

# Restart fat-tree programs with a new mode after a previous simulation run.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="$ROOT_DIR/.fat_tree_config"
APP_DIR="/traci"

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

if [ ! -f "$CONFIG_FILE" ]; then
    echo "Error: Configuration file not found at $CONFIG_FILE"
    echo "Please run script/setup.sh first."
    exit 1
fi

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

require_binary() {
    local container="$1"
    local binary="$2"

    require_container "$container"
    if ! docker exec "$container" test -x "$APP_DIR/bin/$binary"; then
        echo "Error: $APP_DIR/bin/$binary not found in $container"
        echo "Please run script/test.sh once to compile and install binaries."
        exit 1
    fi
}

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

clear_log() {
    local container="$1"
    docker exec "$container" mkdir -p "$APP_DIR/logs"
    docker exec "$container" rm -f "$APP_DIR/logs/$container.log"
}

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

echo "======== FAT-TREE-RESTART ========"
echo "$GPU_NUM GPUs, $LEAF_NUM leaf switches, $SPINE_NUM spine switches"
echo "$GPU_PER_LEAF GPUs under a leaf switch"
echo "GPU request start delay: ${GPU_START_DELAY}s"
echo "Workload directory: $WORKLOAD_SET"
echo "Run mode: $RUN_MODE"
echo "=================================="

echo "Checking containers and binaries..."
for ((i=0; i<LEAF_NUM; i++)); do
    require_binary "leaf$i" "leaf"
done
for ((i=0; i<SPINE_NUM; i++)); do
    require_binary "spine$i" "spine"
done
for ((i=0; i<GPU_NUM; i++)); do
    require_binary "gpu$i" "gpu"
done

echo "Stopping previous programs..."
for ((i=0; i<GPU_NUM; i++)); do
    stop_program "gpu$i" "gpu"
done
for ((i=0; i<LEAF_NUM; i++)); do
    stop_program "leaf$i" "leaf"
done
for ((i=0; i<SPINE_NUM; i++)); do
    stop_program "spine$i" "spine"
done

echo "Copying workloads and clearing logs..."
for ((i=0; i<GPU_NUM; i++)); do
    copy_workload_if_present "$i"
done
for ((i=0; i<GPU_NUM; i++)); do
    clear_log "gpu$i"
done
for ((i=0; i<LEAF_NUM; i++)); do
    clear_log "leaf$i"
done
for ((i=0; i<SPINE_NUM; i++)); do
    clear_log "spine$i"
done

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

echo "Restarted test programs"
echo "Logs are stored in each container under $APP_DIR/logs/<container>.log"
wait_for_gpu_completion
echo "Simulation complete. You can now run script/logs.sh to save logs."
