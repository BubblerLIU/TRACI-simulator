/*
 * common.c - common.h 中函数的实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

/* 全局变量 */
packet_buffer_t pkt_buffer;
net_device_t devices[MAX_DEVICES];
int device_count = 0;
volatile int stop = 0; // 程序终止标志

/* 本地环境变量 */
const char *node_role = NULL;
const char *node_id = NULL;
const char *gpu_per_leaf = NULL;
const char *spine_num = NULL;

/*
 * common_init - 扫描网络设备并创建对应线程
 */
int common_init(void) {
    char errbuf[PCAP_ERRBUF_SIZE];

    // 初始化包缓冲区
    memset(&pkt_buffer, 0, sizeof(pkt_buffer));
    pkt_buffer.head = pkt_buffer.tail = 0;
    pthread_mutex_init(&pkt_buffer.lock, NULL);

    // 扫描网络设备
    pcap_if_t *alldevs;
    if (pcap_findalldevs(&alldevs, errbuf) == -1) {
        fprintf(stderr, "%s %s: Failed to find devices: %s\n",
            node_role, node_id, errbuf);
        return -1;
    }

    // 对目标设备创建监听线程
    pcap_if_t *d;
    for (d = alldevs; d != NULL && device_count < MAX_DEVICES; d = d->next) {
        if (strncmp(d->name, "gpu", 3) == 0 || 
            strncmp(d->name, "leaf", 4) == 0 ||
            strncmp(d->name, "spine", 5) == 0) {

            strncpy(devices[device_count].name, d->name, 31);
            devices[device_count].index = device_count;

            // 创建监听句柄
            devices[device_count].handle = pcap_open_live(d->name,
                PACKET_BUF_SIZE, 1, 1000, errbuf);
            if (!devices[device_count].handle) {
                fprintf(stderr, "%s %s:Failed to open device %s: %s\n",
                    node_role, node_id, d->name, errbuf);
                continue;
            }

            // 创建监听线程
            if (pthread_create(&devices[device_count].thread_id,
                NULL, capture_thread, &devices[device_count]) != 0) {
                    fprintf(stderr, "%s %s: Failed to create thread for %s\n",
                        node_role, node_id, d->name);
                pcap_close(devices[device_count].handle);
                continue;
            }

            device_count++;
        }
    }

    pcap_freealldevs(alldevs);
    printf("%s %s: Initialized %d network devices\n",
        node_role, node_id, device_count);
    return device_count > 0 ? 0 : -1;
}

/*
 * capture_thread - 监听线程
 */
void *capture_thread(void *arg) {
    net_device_t *dev = (net_device_t *)arg;
    struct pcap_pkthdr header;
    const u_char *packet;
    
    printf("%s %s: Starting capture on %s\n", node_role, node_id, dev->name);
    
    while (1) {
        packet = pcap_next(dev->handle, &header);
        if (!packet) continue;        
        
        pthread_mutex_lock(&pkt_buffer.lock);
        
        // 缓冲区已满
        if ((pkt_buffer.head + 1) % MAX_PACKETS == pkt_buffer.tail) {
            fprintf(stderr, "%s %s: Packet buffer full, dropping packet\n",
                node_role, node_id);
            pthread_mutex_unlock(&pkt_buffer.lock);
            continue;
        }
        
        // 将包存入缓冲区
        packet_entry_t *entry = &pkt_buffer.packets[pkt_buffer.head];
        entry->device = dev;
        entry->len = header.len;
        entry->timestamp = header.ts.tv_sec * 1000000 + header.ts.tv_usec;
        memcpy(entry->data, packet, header.len > PACKET_BUF_SIZE ? PACKET_BUF_SIZE : header.len);
        
        pkt_buffer.head = (pkt_buffer.head + 1) % MAX_PACKETS;
        pthread_mutex_unlock(&pkt_buffer.lock);
    }
    
    return NULL;
}

/*
 * get_mac - 根据端口类型和连接的主机编号确定 MAC 地址
 *           符合 setup.sh 中的 MAC 地址分配规则
 *           默认 id1 靠近 GPU 而 id2 靠近 Spine
 */
void get_mac(char role, int id1, int id2, uint8_t mac[6]) {
    mac[0] = 0xaa;
    mac[1] = 0xbb;
    mac[2] = 0xcc;

    switch (role) {
        case 'G': // GPU-Leaf 端口
            mac[3] = 0x00;
            mac[4] = 0x00;
            mac[5] = (uint8_t)id1;
            break;
        case 'D': // Leaf-GPU 下行端口
            mac[3] = 0x01;
            mac[4] = 0x00;
            mac[5] = (uint8_t)id1;
            break;
        case 'U': // Leaf-Spine 上行端口
            mac[3] = 0x02;
            mac[4] = (uint8_t)id1;
            mac[5] = (uint8_t)id2;
            break;
        case 'S': // Spine-Leaf 端口
            mac[3] = 0x03;
            mac[4] = (uint8_t)id2;
            mac[5] = (uint8_t)id1;
            break;
        default:
            mac[3] = 0x00;
            mac[4] = 0x00;
            mac[5] = 0x00;
            break;
    }
}

/*
 * send_packet - 发送包
 */
int send_packet(net_device_t *dev, const uint8_t *data, uint32_t len) {
    if (pcap_inject(dev->handle, data, len) == -1) {
        fprintf(stderr, "%s %s: Error sending packet on %s: %s\n",
               node_role, node_id, dev->name, pcap_geterr(dev->handle));
        return -1;
    }
    return 0;
}