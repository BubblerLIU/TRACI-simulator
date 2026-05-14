#!/bin/bash

# 打印 fat-tree 拓扑中所有容器内的运行日志

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="$ROOT_DIR/.fat_tree_config"
APP_DIR="/traci"

container_exists() {
    local container="$1"
    docker container inspect "$container" >/dev/null 2>&1
}

container_running() {
    local container="$1"
    local running

    running="$(docker container inspect -f '{{.State.Running}}' \
        "$container" 2>/dev/null || true)"
    [ "$running" = "true" ]
}

print_log() {
    local container="$1"
    local log_file="$APP_DIR/logs/$container.log"
    local tmp_dir

    echo "===== $container ====="

    if ! container_exists "$container"; then
        echo "Container not found"
        echo
        return
    fi

    if container_running "$container"; then
        docker exec "$container" sh -c "
            if [ -f '$log_file' ]; then
                cat '$log_file'
            else
                echo 'Log file not found: $log_file'
            fi
        "
        echo
        return
    fi

    tmp_dir="$(mktemp -d)"
    if docker cp "$container:$log_file" "$tmp_dir/$container.log" \
        >/dev/null 2>&1; then
        cat "$tmp_dir/$container.log"
    else
        echo "Container is not running and log file not found: $log_file"
    fi
    rm -rf "$tmp_dir"
    echo
}

# ========== main ==========

if ! command -v docker >/dev/null 2>&1; then
    echo "Error: docker command not found"
    exit 1
fi

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

for ((i=0; i<GPU_NUM; i++)); do
    print_log "gpu$i"
done

for ((i=0; i<LEAF_NUM; i++)); do
    print_log "leaf$i"
done

for ((i=0; i<SPINE_NUM; i++)); do
    print_log "spine$i"
done
