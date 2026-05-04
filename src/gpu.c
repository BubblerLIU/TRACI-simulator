/* 
 * gpu.c - GPU 主线程
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

/* 
 * gpu - 执行 GPU 的行为：发送 response 和 request
 */
void gpu() {
    while (1) {
        pthread_mutex_lock(&pkt_buffer.lock);

        int operation = 0; // 标记本次循环是否有操作
        
        // 检查缓冲区是否为空
        if (pkt_buffer.head != pkt_buffer.tail) {
            // 从缓冲区取出一个包
            packet_entry_t *entry = &pkt_buffer.packets[pkt_buffer.tail];
            pkt_buffer.tail = (pkt_buffer.tail + 1) % MAX_PACKETS;
            pthread_mutex_unlock(&pkt_buffer.lock);

            // 发送 response 到源 GPU

        }

    }
}

int main()
{
    // 从环境变量中获取当前结点信息
    const char *node_role = getenv("NODE_ROLE");
    const char *node_id = getenv("NODE_ID");
    const char *gpu_per_leaf = getenv("GPU_PER_LEAF");
    const char *spine_num = getenv("SPINE_NUM");

    // 确保当前结点是 GPU
    if (strcmp(node_role, "gpu") != 0) {
        fprintf(stderr, "Wrong node role: %s, shoule be gpu\n", node_role);
        return 1;
    }

    // 扫描设备并启动监听
    if (common_init() == -1) {
        fprintf(stderr, "Devices initalization error\n");
        return 1;
    }

    // 主线程行为
    gpu();
}