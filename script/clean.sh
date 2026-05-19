#!/bin/bash

# 清理配置

# 目录变量
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="$ROOT_DIR/.fat_tree_config"
LAST_WORKLOAD_FILE="$ROOT_DIR/.last_workload"

container_names() {
    docker ps -a --format '{{.Names}}' |
        grep -E '^(gpu|leaf|spine)[0-9]+$' || true
}

# 容器内程序终止逻辑
is_container_running() {
    local container="$1"
    local running

    running="$(docker container inspect -f '{{.State.Running}}' \
        "$container" 2>/dev/null || true)"
    [ "$running" = "true" ]
}

stop_program() {
    local container="$1"
    local binary="$2"

    if ! is_container_running "$container"; then
        return
    fi

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
    " >/dev/null 2>&1 || true
}

# ========== main ==========

# 确保配置文件存在
if [ ! -f "$CONFIG_FILE" ]; then
    containers="$(container_names)"
    if [ -z "$containers" ]; then
        rm -f "$LAST_WORKLOAD_FILE"
        echo "No topology configuration or containers found."
        echo "Removed last workload state if present."
        exit 0
    fi

    echo "Configuration file not found at $CONFIG_FILE"
    echo "Removing detected gpu/leaf/spine containers without topology config."
    for container in $containers; do
        docker container rm -f "$container"
    done
    rm -f "$LAST_WORKLOAD_FILE"
    echo "Cleanup complete, stale containers removed."
    exit 0
fi

# 读取配置
source "$CONFIG_FILE" 2>/dev/null
if [ -z "$GPU_NUM" ] || [ -z "$LEAF_NUM" ] || [ -z "$SPINE_NUM" ]; then
    echo "Error: Invalid configuration file (missing topology parameters)"
    exit 1
fi

# 停止并删除容器

echo "Removing: $GPU_NUM GPUs, $LEAF_NUM Leaf switches, $SPINE_NUM Spine switches"

for ((i=0; i<GPU_NUM; i++)); do
    stop_program "gpu$i" "gpu"
done

for ((i=0; i<LEAF_NUM; i++)); do
    stop_program "leaf$i" "leaf"
done

for ((i=0; i<SPINE_NUM; i++)); do
    stop_program "spine$i" "spine"
done

for ((i=0; i<GPU_NUM; i++)); do
    docker container stop gpu$i
    docker container rm gpu$i
done

for ((i=0; i<LEAF_NUM; i++)); do
    docker container stop leaf$i
    docker container rm leaf$i
done

for ((i=0; i<SPINE_NUM; i++)); do
    docker container stop spine$i
    docker container rm spine$i
done

echo "Containers removed"

# 删除配置文件
rm -f "$CONFIG_FILE"
rm -f "$LAST_WORKLOAD_FILE"
echo "Cleanup complete, configuration files removed."
