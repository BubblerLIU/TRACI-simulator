# !/bin/bash
# 自动配置 fat-tree 拓扑结构

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
echo "======== TOPO ========"
echo "$GPU_NUM GPUs, $LEAF_NUM leaf switches, $SPINE_NUM spine switches"
echo "$GPU_PER_LEAF GPUs under a leaf switch"
echo "======================"

# 创建镜像
docker image build -t node .
echo "image build done"

# 创建并启动容器
for ((i=0; i<GPU_NUM; i++)); do
    docker container create --cap-add NET_ADMIN --name gpu$i node
    docker container start gpu$i
done

for ((i=0; i<LEAF_NUM; i++)); do
    docker container create --cap-add NET_ADMIN --name leaf$i node
    docker container start leaf$i
done

for ((i=0; i<SPINE_NUM; i++)); do
    docker container create --cap-add NET_ADMIN --name spine$i node
    docker container start spine$i
done

echo "container create and start done"

# 连接网络
# GPU - Leaf
gpu_id=0
for ((i=0; i<LEAF_NUM; i++)); do
    for ((j=0; j<GPU_PER_LEAF; j++)); do
        veth_gpu="gpu${gpu_id}-eth0"
        veth_leaf="leaf${i}-gpu${gpu_id}"
        ip link add $veth_gpu type veth peer name $veth_leaf

        # 只涉及交换机的二层网络，无需配置 IP 地址
        gpu_name="gpu${gpu_id}"        
        ns_gpu=$(docker inspect -f '{{.State.Pid}}' ${gpu_name})
        ln -s /proc/$ns_gpu/ns/net /var/run/netns/$ns_gpu
        ip link set $veth_gpu netns $ns_gpu
        ip netns exec $ns_gpu ip link set $veth_gpu up

        leaf_name="leaf${i}"
        ns_leaf=$(docker inspect -f '{{.State.Pid}}' ${leaf_name})
        ln -s /proc/$ns_leaf/ns/net /var/run/netns/$ns_leaf
        ip link set $veth_leaf netns $ns_leaf
        ip netns exec $ns_leaf ip link set $veth_leaf up

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

        leaf_name="leaf${i}"
        ns_leaf=$(docker inspect -f '{{.State.Pid}}' ${leaf_name})
        ln -s /proc/$ns_leaf/ns/net /var/run/netns/$ns_leaf
        ip link set $veth_leaf netns $ns_leaf
        ip netns exec $ns_leaf ip link set $veth_leaf up

        spine_name="spine${j}"        
        ns_spine=$(docker inspect -f '{{.State.Pid}}' ${spine_name})
        ln -s /proc/$ns_spine/ns/net /var/run/netns/$ns_spine
        ip link set $veth_spine netns $ns_spine
        ip netns exec $ns_spine ip link set $veth_spine up
    done
done

echo "Leaf-Spine connect done"