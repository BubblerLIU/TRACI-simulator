/*
 * spine.c - Spine Switch 主线程
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include "common.h"

#define TRACI_PKT_LEN (sizeof(eth_header_t) + sizeof(traci_header_t))

static int spine_id = 0;
static int gpu_count_per_leaf = 0;

/*
 * find_device - 根据名称查找设备
 */
static inline net_device_t *find_device(const char *name) {
    for (int i = 0; i < device_count; ++i) {
        if (strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

/*
 * forward_to_leaf - 根据目标 GPU 转发至对应 Leaf Switch
 */
static void forward_to_leaf(packet_entry_t *entry, eth_header_t *eth,
    traci_header_t *traci) {

    if (!(eth->dst_mac[0] == 0xaa && eth->dst_mac[1] == 0xbb &&
        eth->dst_mac[2] == 0xcc && eth->dst_mac[3] == 0x00 &&
        eth->dst_mac[4] == 0x00)) {
        fprintf(stderr, "Spine %s: packet from %s has non-GPU dst MAC\n",
            node_id, entry->device->name);
        return;
    }

    uint8_t dst_gpu = eth->dst_mac[5];
    int dst_leaf = dst_gpu / gpu_count_per_leaf;

    char name[32];
    snprintf(name, sizeof(name), "spine%d-leaf%d", spine_id, dst_leaf);

    net_device_t *out_dev = find_device(name);
    if (out_dev == NULL) {
        fprintf(stderr, "Spine %s: failed to find leaf device %d\n",
            node_id, dst_leaf);
        return;
    }

    if (send_packet(out_dev, entry->data, entry->len) == 0) {
        printf("Spine %s: forwarded seq_num=%" PRIu32
            " from %s to leaf %d for GPU %" PRIu8 "\n",
            node_id, traci->seq_num, entry->device->name, dst_leaf, dst_gpu);
    }
}

/*
 * parse_pkt - 处理包
 */
static void parse_pkt(packet_entry_t *entry) {
    if (entry->len < TRACI_PKT_LEN) {
        fprintf(stderr, "Spine %s: dropping short packet from %s, len=%" PRIu32 "\n",
            node_id, entry->device->name, entry->len);
        return;
    }

    eth_header_t *eth = (eth_header_t *)entry->data;
    traci_header_t *traci =
        (traci_header_t *)(entry->data + sizeof(eth_header_t));

    if (strstr(entry->device->name, "-leaf") != NULL) {
        forward_to_leaf(entry, eth, traci);
    }
    else {
        fprintf(stderr, "Spine %s: unknown ingress device %s\n",
            node_id, entry->device->name);
    }
}

/*
 * spine - 执行 Spine Switch 的行为：接收并转发来自 Leaf 的包
 */
void spine() {
    while (1) {
        int operation = 0;
        int has_packet = 0;
        packet_entry_t entry;

        // 检查缓冲区
        pthread_mutex_lock(&pkt_buffer.lock);
        if (pkt_buffer.head != pkt_buffer.tail) {
            operation = 1;
            has_packet = 1;

            // 从缓冲区取出一个包
            memcpy(&entry, &pkt_buffer.packets[pkt_buffer.tail], sizeof(entry));
            pkt_buffer.tail = (pkt_buffer.tail + 1) % MAX_PACKETS;
        }
        pthread_mutex_unlock(&pkt_buffer.lock);

        if (has_packet) {
            // 处理包
            parse_pkt(&entry);
        }

        // 没事干就休息一会儿
        if (!operation) {
            usleep(1000);
        }
    }
}

int main()
{
    node_role = getenv("NODE_ROLE");
    node_id = getenv("NODE_ID");
    gpu_per_leaf = getenv("GPU_PER_LEAF");
    spine_num = getenv("SPINE_NUM");

    if (node_role == NULL || node_id == NULL ||
        gpu_per_leaf == NULL || spine_num == NULL) {
            fprintf(stderr, "Failed to get environment variables!\n");
            return 1;
        }

    // 检查信息
    printf("Hello from %s %s, %s GPUs per Leaf, %s Spines\n",
        node_role, node_id, gpu_per_leaf, spine_num);

    // 检查节点类型
    if (strcmp(node_role, "Spine") != 0) {
        fprintf(stderr, "Wrong node role: %s, shoule be Spine\n", node_role);
        return 1;
    }

    spine_id = atoi(node_id);
    gpu_count_per_leaf = atoi(gpu_per_leaf);
    if (gpu_count_per_leaf <= 0) {
        fprintf(stderr, "Spine %s: invalid GPU_PER_LEAF=%s\n",
            node_id, gpu_per_leaf);
        return 1;
    }

    // 扫描设备并启动监听
    if (common_init() == -1) {
        fprintf(stderr, "Devices initalization error\n");
        return 1;
    }

    // 运行主进程
    spine();

    return 0;
}
