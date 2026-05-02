#!/bin/bash

# 清理配置

CONFIG_FILE="./.fat_tree_config"

# 确保配置文件存在
if [ ! -f "$CONFIG_FILE" ]; then
    echo "Error: Configuration file not found at $CONFIG_FILE"
    echo "Please run setup.sh first."
    exit 1
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
echo "Cleanup complete, configuration file removed."