/* 
 * gpu.c - GPU 主线程
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "common.h"

/* 全局变量 */
FILE *workload = NULL;
uint32_t next_seq = 0;
uint32_t next_line = 0;

/*
 * parse_pkt - 解析和处理收到的包
 */
void parse_pkt(packet_entry_t *entry) {
    // 解析以太网头
    eth_header_t *eth = (eth_header_t *)entry->data;

    // 检查包是不是发给自己的
    int dst_id = (int)eth->dst_mac[5];
    int src_id = (int)eth->src_mac[5];
    if (dst_id != atoi(node_id)) {
        fprintf(stderr, "GPU %s: got a wrong packet from GPU %d, "
            "should be sent to GPU %d\n", node_id, src_id, dst_id);
    }
    else {
        // 解析 traci 头
        traci_header_t *traci = (traci_header_t *)entry->data[14];

        // 收到 response
        if (traci->traci_type == 2) {
            printf("GPU %s: got a response from GPU %d, "
                "seq_num=%" PRIu32 "\n", node_id, traci->seq_num, src_id);
            return;
        }

        // 收到 request, 发回 response
        printf("GPU %s: got a request from GPU %d, "
            "seq_num=%" PRIu32 "\n", node_id, traci->seq_num, src_id);
        uint8_t tmp[6];
        memcpy(tmp, eth->src_mac, 6);
        memcpy(eth->src_mac, eth->dst_mac, 6);
        memcpy(eth->dst_mac, tmp, 6);
        traci->traci_type = 2;
        send_packet(entry->device, entry->data, entry->len);
    }
}

/*
 * construct_pkt - 为 request 创建包
 */
uint8_t *construct_pkt(uint8_t input_gpu, 
    uint32_t input_local_addr, uint32_t output_local_addr) {

    uint8_t *data = malloc(27);

    eth_header_t *eth = (eth_header_t *)data;
    

}

/* 
 * gpu - 执行 GPU 的行为：发送 response 和 request
 */
void gpu() {
    while (1) {
        pthread_mutex_lock(&pkt_buffer.lock);

        int operation = 0; // 标记本次循环是否有操作
        
        // 检查缓冲区是否为空
        if (pkt_buffer.head != pkt_buffer.tail) {
            operation = 1;

            // 从缓冲区取出一个包
            packet_entry_t *entry = &pkt_buffer.packets[pkt_buffer.tail];
            pkt_buffer.tail = (pkt_buffer.tail + 1) % MAX_PACKETS;
            pthread_mutex_unlock(&pkt_buffer.lock);

            // 处理包
            parse_pkt(entry);
        }

        // 检查是否有待发送的 request
        char line[MAX_LINE];
        if (fgets(line, sizeof(line), workload) != NULL) {
            operation = 1;

            // 读取下一行的信息
            uint8_t input_gpu = 0;
            uint32_t input_local_addr = 0, output_local_addr = 0;
            int n = sscanf(line, "%" PRId8 " %" PRId32 " %" PRId32 "", 
                        &input_gpu, &input_local_addr, &output_local_addr);
            ++next_line;
            if (n != 3) {
                fprintf(stderr, "%s %s: Error when reading workload line "
                    "%" PRId32 "\n", node_role, node_id, next_line);
                continue;
            }

        }
    }
}

int main()
{
    // 从环境变量中获取当前结点信息
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

    // 确保当前结点是 GPU
    if (strcmp(node_role, "gpu") != 0) {
        fprintf(stderr, "Wrong node role: %s, shoule be gpu\n", node_role);
        return 1;
    }

    // 从外部文件读取测试数据
    char filename[MAX_FILENAME];
    snprintf(filename, 20, "workload_gpu%s", node_id);
    workload = fopen(filename, "r");
    if (workload == NULL) {
        fprintf(stderr, "GPU %s: Failed to open workload file\n", node_id);
        // 先不退出程序，只是失去发送 request 的能力
    }

    // 扫描设备并启动监听
    if (common_init() == -1) {
        fprintf(stderr, "Devices initalization error\n");
        return 1;
    }

    // 启动主线程行为
    gpu();
}