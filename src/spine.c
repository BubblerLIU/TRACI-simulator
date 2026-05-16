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
static sim_mode_t sim_mode = SIM_MODE_BASELINE;
static rtb_table_t rtb;
static isc_table_t isc;

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
 * baseline_route_packet - Baseline 模式原始转发逻辑
 */
static void baseline_route_packet(packet_entry_t *entry, eth_header_t *eth,
    traci_header_t *traci) {

    if (strstr(entry->device->name, "-leaf") != NULL) {
        forward_to_leaf(entry, eth, traci);
    }
    else {
        fprintf(stderr, "Spine %s: unknown ingress device %s\n",
            node_id, entry->device->name);
    }
}

/*
 * make_isc_response - 将命中 ISC 的 request 改写成本地生成的 response
 */
static void make_isc_response(eth_header_t *eth, traci_header_t *traci,
    uint32_t data) {

    uint8_t tmp[6];
    memcpy(tmp, eth->src_mac, 6);
    memcpy(eth->src_mac, eth->dst_mac, 6);
    memcpy(eth->dst_mac, tmp, 6);

    traci->count = 1;
    traci->data = data;
    traci->traci_type = TRACI_TYPE_RESPONSE;
}

static void traci_handle_response(packet_entry_t *entry, eth_header_t *eth,
    traci_header_t *traci) {

    if (traci->count == 1 && traci->iaddr != 0) {
        isc_insert(&isc, traci->iaddr, traci->data);
        printf("Spine %s: ISC inserted iaddr=%" PRIu32
            ", data=%" PRIu32 "\n",
            node_id, traci->iaddr, traci->data);
    }

    rtb_response_result_t result = rtb_reduce_response(&rtb, traci);

    if (result == RTB_RESPONSE_DROP) {
        printf("Spine %s: RTB reduced and dropped response "
            "seq_num=%" PRIu32 ", oaddr=%" PRIu32 "\n",
            node_id, traci->seq_num, traci->oaddr);
        return;
    }
    if (result == RTB_RESPONSE_EVOKE) {
        printf("Spine %s: RTB emitted response seq_num=%" PRIu32
            ", oaddr=%" PRIu32 ", count=%" PRIu32
            ", data=%" PRIu32 "\n",
            node_id, traci->seq_num, traci->oaddr,
            traci->count, traci->data);
    }

    baseline_route_packet(entry, eth, traci);
}

/*
 * traci_route_packet - TRACI 模式：RTB 处理后复用 Baseline 转发路径
 */
static void traci_route_packet(packet_entry_t *entry, eth_header_t *eth,
    traci_header_t *traci) {

    if (traci->traci_type == TRACI_TYPE_REQUEST) {
        rtb_request_result_t result = rtb_track_request(&rtb, traci, 0);
        if (result == RTB_REQUEST_BYPASS) {
            printf("Spine %s: RTB full, bypassed request seq_num=%" PRIu32
                ", oaddr=%" PRIu32 "\n",
                node_id, traci->seq_num, traci->oaddr);
        }

        uint32_t cached_data = 0;
        if (result == RTB_REQUEST_TRACKED &&
            isc_lookup(&isc, traci->iaddr, &cached_data)) {
            printf("Spine %s: ISC hit iaddr=%" PRIu32
                ", generated response seq_num=%" PRIu32 "\n",
                node_id, traci->iaddr, traci->seq_num);
            make_isc_response(eth, traci, cached_data);
            traci_handle_response(entry, eth, traci);
            return;
        }

        baseline_route_packet(entry, eth, traci);
        return;
    }

    if (traci->traci_type == TRACI_TYPE_RESPONSE) {
        traci_handle_response(entry, eth, traci);
        return;
    }

    fprintf(stderr, "Spine %s: unknown TRACI packet type %" PRIu8
        " from %s\n", node_id, traci->traci_type, entry->device->name);
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
    if (eth->ether_type != ETH_TYPE) {
        return;
    }

    traci_header_t *traci =
        (traci_header_t *)(entry->data + sizeof(eth_header_t));

    if (sim_mode == SIM_MODE_TRACI) {
        traci_route_packet(entry, eth, traci);
        return;
    }

    baseline_route_packet(entry, eth, traci);
}

/*
 * spine - 执行 Spine Switch 的行为：接收并转发来自 Leaf 的包
 */
void spine() {
    while (!stop) {
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
    printf("Spine %s: mode=%s\n", node_id, sim_mode_name(sim_mode));

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
    rtb_init(&rtb);
    isc_init(&isc);

    // 扫描设备并启动监听
    if (common_init() == -1) {
        fprintf(stderr, "Devices initalization error\n");
        common_shutdown();
        return 1;
    }

    // 运行主进程
    spine();
    common_shutdown();

    return 0;
}
