#!/bin/bash

# 自动配置 fat-tree 拓扑结构

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ $# -ne 3 ]; then
    echo "Usage: $0 <GPU> <Leaf> <Spine>"
    exit 1
fi

GPU_NUM=$1
LEAF_NUM=$2
SPINE_NUM=$3

# GPU 必须是 Leaf 的倍数
if [ $((GPU_NUM % LEAF_NUM)) -ne 0 ]; then
    echo "Error: <GPU> must be a multiple of <Leaf>"
    exit 1
fi

GPU_PER_LEAF=$((GPU_NUM / LEAF_NUM))

# 展示拓扑结构信息
echo "======== FAT-TREE-TOPO ========"
echo "$GPU_NUM GPUs, $LEAF_NUM leaf switches, $SPINE_NUM spine switches"
echo "$GPU_PER_LEAF GPUs under a leaf switch"
echo "==============================="

# 创建镜像
docker image build -t node "$ROOT_DIR"
echo "Image build done"

# 创建并启动容器，通过环境变量传入拓扑信息
for ((i=0; i<GPU_NUM; i++)); do
    docker container create --network none --cap-add NET_ADMIN \
        -e NODE_ROLE=GPU -e NODE_ID=$i \
        -e GPU_PER_LEAF=$GPU_PER_LEAF -e SPINE_NUM=$SPINE_NUM \
        --name gpu$i node
    docker container start gpu$i
done

for ((i=0; i<LEAF_NUM; i++)); do
    docker container create --network none --cap-add NET_ADMIN \
        -e NODE_ROLE=Leaf -e NODE_ID=$i \
        -e GPU_PER_LEAF=$GPU_PER_LEAF -e SPINE_NUM=$SPINE_NUM \
        --name leaf$i node
    docker container start leaf$i
done

for ((i=0; i<SPINE_NUM; i++)); do
    docker container create --network none --cap-add NET_ADMIN \
        -e NODE_ROLE=Spine -e NODE_ID=$i \
        -e GPU_PER_LEAF=$GPU_PER_LEAF -e SPINE_NUM=$SPINE_NUM \
        --name spine$i node
    docker container start spine$i
done

echo "Container create and start done"

# 连接网络

# 只涉及交换机的二层网络，无需配置 IP 地址，手动设置 MAC 地址
MAC_PREFIX="aa:bb:cc"

# GPU - Leaf
gpu_id=0
for ((i=0; i<LEAF_NUM; i++)); do
    for ((j=0; j<GPU_PER_LEAF; j++)); do
        veth_gpu="gpu${gpu_id}-eth0"
        veth_leaf="leaf${i}-gpu${gpu_id}"
        ip link add $veth_gpu type veth peer name $veth_leaf

        gpu_name="gpu${gpu_id}"
        mac_hex=$(printf '%02x' $gpu_id)
        gpu_mac="${MAC_PREFIX}:00:00:${mac_hex}"

        ip link set $veth_gpu netns $(docker inspect -f '{{.State.Pid}}' ${gpu_name})
        docker exec ${gpu_name} ip link set $veth_gpu up
        docker exec ${gpu_name} ip link set dev $veth_gpu address $gpu_mac

        leaf_name="leaf${i}"
        leaf_mac="${MAC_PREFIX}:01:00:${mac_hex}"

        ip link set $veth_leaf netns $(docker inspect -f '{{.State.Pid}}' ${leaf_name})
        docker exec ${leaf_name} ip link set $veth_leaf up
        docker exec ${leaf_name} ip link set dev $veth_leaf address $leaf_mac

        let gpu_id++
    done
done

echo "GPU-Leaf connect done"

# Leaf - Spine
for ((i=0; i<LEAF_NUM; i++)); do
    for ((j=0; j<SPINE_NUM; j++)); do
        veth_leaf="leaf${i}-spine${j}"
        veth_spine="spine${j}-leaf${i}"
        ip link add $veth_leaf type veth peer name $veth_spine

        i_hex=$(printf '%02x' $i)
        j_hex=$(printf '%02x' $j)

        leaf_mac="${MAC_PREFIX}:02:${i_hex}:${j_hex}"
        leaf_name="leaf${i}"

        ip link set $veth_leaf netns $(docker inspect -f '{{.State.Pid}}' ${leaf_name})
        docker exec ${leaf_name} ip link set $veth_leaf up
        docker exec ${leaf_name} ip link set dev $veth_leaf address $leaf_mac

        spine_mac="${MAC_PREFIX}:03:${j_hex}:${i_hex}"
        spine_name="spine${j}"

        ip link set $veth_spine netns $(docker inspect -f '{{.State.Pid}}' ${spine_name})
        docker exec ${spine_name} ip link set $veth_spine up
        docker exec ${spine_name} ip link set dev $veth_spine address $spine_mac
    done
done

echo "Leaf-Spine connect done"

# 保存配置信息

CONFIG_FILE="$ROOT_DIR/.fat_tree_config"

cat > "$CONFIG_FILE" <<EOF
GPU_NUM=$GPU_NUM
LEAF_NUM=$LEAF_NUM
SPINE_NUM=$SPINE_NUM
CREATED_AT=$(date -Iseconds)
EOF

echo "Configuration saved to $CONFIG_FILE"