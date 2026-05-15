/*
 * leaf.c - Leaf Switch 主线程
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include "common.h"

#define TRACI_PKT_LEN (sizeof(eth_header_t) + sizeof(traci_header_t))

static int leaf_id = 0;
static int spine_count = 0;
static int gpu_count_per_leaf = 0;
static sim_mode_t sim_mode = SIM_MODE_BASELINE;
static rtb_table_t rtb;

typedef enum {
    PACKET_DONE = 0,
    PACKET_STALLED = 1,
} packet_result_t;

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
 * forward_to_spine - 上行转发至 Spine Switch
 */
static void forward_to_spine(packet_entry_t *entry, traci_header_t *traci) {
    int spine_id = hash_oaddr(traci->oaddr) % spine_count;
    char name[32];
    snprintf(name, sizeof(name), "leaf%d-spine%d", leaf_id, spine_id);

    net_device_t *out_dev = find_device(name);
    if (out_dev == NULL) {
        fprintf(stderr, "Leaf %s: failed to find spine device %d\n",
            node_id, spine_id);
        return;
    }

    if (send_packet(out_dev, entry->data, entry->len) == 0) {
        printf("Leaf %s: forwarded seq_num=%" PRIu32
            " from %s to spine %d\n",
            node_id, traci->seq_num, entry->device->name, spine_id);
    }
}

/*
 * forward_to_gpu - 下行转发至 GPU
 */
static void forward_to_gpu(packet_entry_t *entry, eth_header_t *eth,
    traci_header_t *traci) {

    if (!(eth->dst_mac[0] == 0xaa && eth->dst_mac[1] == 0xbb &&
        eth->dst_mac[2] == 0xcc && eth->dst_mac[3] == 0x00 &&
        eth->dst_mac[4] == 0x00)) {
        fprintf(stderr, "Leaf %s: packet from %s has non-GPU dst MAC\n",
            node_id, entry->device->name);
        return;
    }

    uint8_t dst_gpu = eth->dst_mac[5];
    char name[32];
    snprintf(name, sizeof(name), "leaf%d-gpu%" PRIu8, leaf_id, dst_gpu);

    net_device_t *out_dev = find_device(name);
    if (out_dev == NULL) {
        fprintf(stderr, "Leaf %s: failed to find GPU device %" PRIu8 "\n",
            node_id, dst_gpu);
        return;
    }

    if (send_packet(out_dev, entry->data, entry->len) == 0) {
        printf("Leaf %s: forwarded seq_num=%" PRIu32
            " from %s to GPU %" PRIu8 "\n",
            node_id, traci->seq_num, entry->device->name, dst_gpu);
    }
}

/*
 * baseline_route_packet - Baseline 模式原始转发逻辑
 */
static void baseline_route_packet(packet_entry_t *entry, eth_header_t *eth,
    traci_header_t *traci) {

    if (strstr(entry->device->name, "-gpu") != NULL) {
        if (!(eth->dst_mac[0] == 0xaa && eth->dst_mac[1] == 0xbb &&
            eth->dst_mac[2] == 0xcc && eth->dst_mac[3] == 0x00 &&
            eth->dst_mac[4] == 0x00)) {
            fprintf(stderr, "Leaf %s: packet from %s has non-GPU dst MAC\n",
                node_id, entry->device->name);
            return;
        }

        int dst_gpu = eth->dst_mac[5];
        int local_gpu_start = leaf_id * gpu_count_per_leaf;
        int local_gpu_end = local_gpu_start + gpu_count_per_leaf;
        if (dst_gpu >= local_gpu_start && dst_gpu < local_gpu_end) {
            forward_to_gpu(entry, eth, traci);
        }
        else {
            forward_to_spine(entry, traci);
        }
    }
    else if (strstr(entry->device->name, "-spine") != NULL) {
        forward_to_gpu(entry, eth, traci);
    }
    else {
        fprintf(stderr, "Leaf %s: unknown ingress device %s\n",
            node_id, entry->device->name);
    }
}

/*
 * traci_route_packet - TRACI 模式：RTB 处理后复用 Baseline 转发路径
 */
static packet_result_t traci_route_packet(packet_entry_t *entry,
    eth_header_t *eth, traci_header_t *traci) {

    if (traci->traci_type == TRACI_TYPE_REQUEST) {
        int from_gpu = strstr(entry->device->name, "-gpu") != NULL;
        rtb_request_result_t result =
            rtb_track_request(&rtb, traci, from_gpu);

        if (result == RTB_REQUEST_STALL) {
            printf("Leaf %s: RTB full, stalled request seq_num=%" PRIu32
                ", oaddr=%" PRIu32 "\n",
                node_id, traci->seq_num, traci->oaddr);
            return PACKET_STALLED;
        }
        if (result == RTB_REQUEST_BYPASS) {
            printf("Leaf %s: RTB full, bypassed request seq_num=%" PRIu32
                ", oaddr=%" PRIu32 "\n",
                node_id, traci->seq_num, traci->oaddr);
        }

        baseline_route_packet(entry, eth, traci);
        return PACKET_DONE;
    }

    if (traci->traci_type == TRACI_TYPE_RESPONSE) {
        rtb_response_result_t result = rtb_reduce_response(&rtb, traci);

        if (result == RTB_RESPONSE_DROP) {
            printf("Leaf %s: RTB reduced and dropped response "
                "seq_num=%" PRIu32 ", oaddr=%" PRIu32 "\n",
                node_id, traci->seq_num, traci->oaddr);
            return PACKET_DONE;
        }
        if (result == RTB_RESPONSE_EVOKE) {
            printf("Leaf %s: RTB emitted response seq_num=%" PRIu32
                ", oaddr=%" PRIu32 ", count=%" PRIu32
                ", data=%" PRIu32 "\n",
                node_id, traci->seq_num, traci->oaddr,
                traci->count, traci->data);
        }

        baseline_route_packet(entry, eth, traci);
        return PACKET_DONE;
    }

    fprintf(stderr, "Leaf %s: unknown TRACI packet type %" PRIu8
        " from %s\n", node_id, traci->traci_type, entry->device->name);
    return PACKET_DONE;
}

/*
 * parse_pkt - 处理包
 */
static packet_result_t parse_pkt(packet_entry_t *entry) {
    if (entry->len < TRACI_PKT_LEN) {
        fprintf(stderr, "Leaf %s: dropping short packet from %s, len=%" PRIu32 "\n",
            node_id, entry->device->name, entry->len);
        return PACKET_DONE;
    }

    eth_header_t *eth = (eth_header_t *)entry->data;
    if (eth->ether_type != ETH_TYPE) {
        return PACKET_DONE;
    }

    traci_header_t *traci =
        (traci_header_t *)(entry->data + sizeof(eth_header_t));

    if (sim_mode == SIM_MODE_TRACI) {
        return traci_route_packet(entry, eth, traci);
    }

    baseline_route_packet(entry, eth, traci);
    return PACKET_DONE;
}

/*
 * leaf - 执行 Leaf Switch 的行为：接收和转发包 
 */
void leaf() {
    packet_entry_t stalled_packets[RTB_ENTRY_NUM];
    int stalled_count = 0;

    while (!stop) {
        int operation = 0;
        int has_packet = 0;
        int stalled_progress = 0;
        packet_entry_t entry;

        for (int i = 0; i < stalled_count;) {
            operation = 1;
            if (parse_pkt(&stalled_packets[i]) == PACKET_STALLED) {
                ++i;
                continue;
            }

            stalled_packets[i] = stalled_packets[stalled_count - 1];
            --stalled_count;
            stalled_progress = 1;
        }

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
            if (parse_pkt(&entry) == PACKET_STALLED) {
                if (stalled_count < RTB_ENTRY_NUM) {
                    stalled_packets[stalled_count++] = entry;
                }
                else {
                    fprintf(stderr, "Leaf %s: stalled packet buffer full, "
                        "dropping seq_num=%" PRIu32 "\n",
                        node_id,
                        ((traci_header_t *)(entry.data +
                            sizeof(eth_header_t)))->seq_num);
                }
            }
        }

        // 没事干就休息一会儿
        if (!operation ||
            (stalled_count > 0 && !has_packet && !stalled_progress)) {
            usleep(1000);
        }
    }
}

int main(int argc, char **argv)
{
    common_setup_signal_handlers();

    if (parse_sim_mode_args(argc, argv, &sim_mode, argv[0]) == -1) {
        return 1;
    }

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
    printf("Leaf %s: mode=%s\n", node_id, sim_mode_name(sim_mode));

    // 检查节点类型
    if (strcmp(node_role, "Leaf") != 0) {
        fprintf(stderr, "Wrong node role: %s, shoule be Leaf\n", node_role);
        return 1;
    }

    leaf_id = atoi(node_id);
    spine_count = atoi(spine_num);
    gpu_count_per_leaf = atoi(gpu_per_leaf);
    if (spine_count <= 0) {
        fprintf(stderr, "Leaf %s: invalid SPINE_NUM=%s\n", node_id, spine_num);
        return 1;
    }
    if (gpu_count_per_leaf <= 0) {
        fprintf(stderr, "Leaf %s: invalid GPU_PER_LEAF=%s\n",
            node_id, gpu_per_leaf);
        return 1;
    }
    rtb_init(&rtb);

    // 扫描设备并启动监听
    if (common_init() == -1) {
        fprintf(stderr, "Devices initalization error\n");
        common_shutdown();
        return 1;
    }

    // 运行主进程
    leaf();
    common_shutdown();

    return 0;
}
