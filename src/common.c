/*
 * common.c - common.h 中函数的实现
 */

#include <stdio.h>
#include "common.h"

/*
 * get_mac - 根据端口类型和连接的主机编号确定 MAC 地址
 *           符合 setup.sh 中的 MAC 地址分配规则
 *           默认 id1 靠近 GPU 而 id2 靠近 Spine
 */
void get_mac(char role, int id1, int id2, char *mac_buf) {
    switch (role) {
        case 'G': // GPU-Leaf 端口
            sprintf(mac_buf, "%s:00:00:%02x", MAC_PREFIX, id1);
            break;
        case 'D': // Leaf-GPU 下行端口
            sprintf(mac_buf, "%s:01:00:%02x", MAC_PREFIX, id1);
            break;
        case 'U': // Leaf-Spine 上行端口
            sprintf(mac_buf, "%s:02:%02x:%02x", MAC_PREFIX, id1, id2);
            break;
        case 'S': // Spine-Leaf 端口
            sprintf(mac_buf, "%s:03:%02x:%02x", MAC_PREFIX, id2, id1);
            break;
    }
}