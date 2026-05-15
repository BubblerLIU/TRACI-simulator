/* 
 * gpu.c - GPU 主线程
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include "common.h"

#define LOCAL_ADDR_MASK UINT32_C(0x00ffffff)
#define TRACI_PKT_LEN (sizeof(eth_header_t) + sizeof(traci_header_t))

/* 全局变量 */
static FILE *workload = NULL;
static uint32_t next_seq = 0;
static uint32_t next_line = 0;
static sim_mode_t sim_mode = SIM_MODE_BASELINE;

/*
 * get_gpu_start_delay - 获取发送 request 前的等待时间
 */
static unsigned int get_gpu_start_delay(void) {
    const char *delay = getenv("GPU_START_DELAY");
    if (delay == NULL || delay[0] == '\0') {
        return 0;
    }

    int seconds = atoi(delay);
    return seconds > 0 ? (unsigned int)seconds : 0;
}

/* 地址打包工具 */
static uint32_t make_traci_addr(uint8_t gpu_id, uint32_t local_addr) {
    return ((uint32_t)gpu_id << 24) | (local_addr & LOCAL_ADDR_MASK);
}

/*
 * make_response_data - 本模拟器不建模真实向量，使用本地地址作为标量数据
 */
static uint32_t make_response_data(uint32_t iaddr) {
    return iaddr & LOCAL_ADDR_MASK;
}

/*
 * parse_pkt - 解析和处理收到的包
 */
static void parse_pkt(packet_entry_t *entry) {
    if (entry->len < TRACI_PKT_LEN) {
        return;
    }

    // 解析以太网头
    eth_header_t *eth = (eth_header_t *)entry->data;
    if (eth->ether_type != ETH_TYPE) {
        return;
    }

    // 检查包是不是发给自己的
    int self_id = atoi(node_id);
    int dst_id = (int)eth->dst_mac[5];
    int src_id = (int)eth->src_mac[5];
    if (dst_id != self_id) {
        if (src_id == self_id) {
            return;
        }

        fprintf(stderr, "GPU %s: got a wrong packet from GPU %d, "
            "should be sent to GPU %d\n", node_id, src_id, dst_id);
    }
    else {
        // 解析 traci 头
        traci_header_t *traci =
            (traci_header_t *)(entry->data + sizeof(eth_header_t));

        // 收到 response
        if (traci->traci_type == TRACI_TYPE_RESPONSE) {
            if (sim_mode == SIM_MODE_TRACI) {
                printf("GPU %s: got a response from GPU %d, "
                    "seq_num=%" PRIu32 ", count=%" PRIu32
                    ", data=%" PRIu32 "\n",
                    node_id, src_id, traci->seq_num,
                    traci->count, traci->data);
            }
            else {
                printf("GPU %s: got a response from GPU %d, "
                    "seq_num=%" PRIu32 "\n",
                    node_id, src_id, traci->seq_num);
            }
            return;
        }

        // 收到 request, 发回 response
        printf("GPU %s: got a request from GPU %d, "
            "seq_num=%" PRIu32 "\n", node_id, src_id, traci->seq_num);
        uint8_t tmp[6];
        memcpy(tmp, eth->src_mac, 6);
        memcpy(eth->src_mac, eth->dst_mac, 6);
        memcpy(eth->dst_mac, tmp, 6);
        traci->count = 1;
        traci->data = make_response_data(traci->iaddr);
        traci->traci_type = TRACI_TYPE_RESPONSE;
        send_packet(entry->device, entry->data, entry->len);
    }
}

/*
 * construct_pkt - 为 request 创建包
 */
static uint8_t *construct_pkt(uint8_t input_gpu, 
    uint32_t input_local_addr, uint32_t output_local_addr) {

    uint32_t pkt_len = TRACI_PKT_LEN;
    uint8_t *data = malloc(pkt_len);
    if (data == NULL) {
        fprintf(stderr, "GPU %s: Failed to allocate packet\n", node_id);
        return NULL;
    }
    memset(data, 0, pkt_len);

    eth_header_t *eth = (eth_header_t *)data;
    get_mac('G', input_gpu, 0, eth->dst_mac);
    get_mac('G', atoi(node_id), 0, eth->src_mac);
    eth->ether_type = ETH_TYPE;

    traci_header_t *traci = (traci_header_t *)(data + sizeof(eth_header_t));
    traci->seq_num = next_seq++;
    traci->iaddr = make_traci_addr(input_gpu, input_local_addr);
    traci->oaddr = make_traci_addr((uint8_t)atoi(node_id), output_local_addr);
    traci->count = 0;
    traci->data = 0;
    traci->traci_type = TRACI_TYPE_REQUEST;

    return data;
}

/* 
 * gpu - 执行 GPU 的行为：发送 response 和 request
 */
void gpu() {
    // 找到 GPU 上行网络接口
    net_device_t *gpu_dev = NULL;
    for (int i = 0; i < device_count; ++i) {
        if (strncmp(devices[i].name, "gpu", 3) == 0) {
            gpu_dev = &devices[i];
            break;
        }
    }
    if (gpu_dev == NULL) {
        fprintf(stderr, "GPU %s: Failed to find GPU network device\n", node_id);
        return;
    }

    // 循环检查缓冲区和待发 request
    while (!stop) {
        int operation = 0; // 标记本次循环是否有操作
        int has_packet = 0;
        packet_entry_t entry;
        
        // 检查缓冲区是否为空
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

        // 检查是否有待发送的 request
        char line[MAX_LINE];
        if (workload != NULL && !feof(workload) &&
            fgets(line, sizeof(line), workload) != NULL) {
            operation = 1;

            // 读取下一行的信息
            uint8_t input_gpu = 0;
            uint32_t input_local_addr = 0, output_local_addr = 0;
            int n = sscanf(line, "%" SCNu8 " %" SCNu32 " %" SCNu32 "",
                        &input_gpu, &input_local_addr, &output_local_addr);
            ++next_line;
            if (n != 3) {
                fprintf(stderr, "%s %s: Error when reading workload line "
                    "%" PRId32 "\n", node_role, node_id, next_line);
                continue;
            }

            uint8_t *pkt = construct_pkt(input_gpu, input_local_addr,
                output_local_addr);
            if (pkt == NULL) {
                fprintf(stderr, "GPU %s: failed to construct packet for line"
                    "%" PRId32 "\n", node_id, next_line);
                continue;
            }

            traci_header_t *traci =
                (traci_header_t *)(pkt + sizeof(eth_header_t));
            if (send_packet(gpu_dev, pkt, TRACI_PKT_LEN) == 0) {
                printf("GPU %s: sent a request to GPU %" PRIu8 ", "
                    "seq_num=%" PRIu32 "\n",
                    node_id, input_gpu, traci->seq_num);
            }
            free(pkt);
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
    printf("GPU %s: mode=%s\n", node_id, sim_mode_name(sim_mode));

    // 确保当前结点是 GPU
    if (strcmp(node_role, "GPU") != 0) {
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
        if (workload != NULL) {
            fclose(workload);
            workload = NULL;
        }
        common_shutdown();
        return 1;
    }

    unsigned int start_delay = get_gpu_start_delay();
    if (start_delay > 0) {
        printf("GPU %s: wait %u seconds before sending requests\n",
            node_id, start_delay);
        sleep(start_delay);
    }

    // 启动主线程行为
    gpu();
    if (workload != NULL) {
        fclose(workload);
        workload = NULL;
    }
    common_shutdown();

    return 0;
}
