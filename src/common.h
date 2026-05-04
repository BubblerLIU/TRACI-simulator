/*
 * common.h - 共享结构体和函数
 */

#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <pcap/pcap.h>
#include <pthread.h>

/* 处理 VSCode 插件报错 */
#if defined(__INTELLISENSE__) || !defined(__u_char_defined)
typedef unsigned char u_char;
#endif
/* 不会真正参与编译 */

/* 宏定义 */
#define MAC_PREFIX "aa:bb:cc"
#define ETH_TYPE 0xAAAA
#define MAX_DEVICES 128
#define PACKET_BUF_SIZE 2048
#define MAX_PACKETS 1024

/* 以太网头 */
typedef struct {
    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint16_t ether_type;
} __attribute__((packed)) eth_header_t;

/* 自定义头部 */
typedef struct {
    uint16_t iaddr;
    uint16_t oaddr;
} __attribute__((packed)) traci_header_t;

/* 网络设备 */
typedef struct {
    char name[32];
    pcap_t *handle;
    pthread_t thread_id;
    int index;
} net_device_t;

/* 包缓冲区条目 */
typedef struct {
    net_device_t *device;
    uint8_t data[PACKET_BUF_SIZE];
    uint32_t len;
    uint64_t timestamp;
} packet_entry_t;

/* 包缓冲区 */
typedef struct {
    packet_entry_t packets[MAX_PACKETS];
    int head;
    int tail;
    pthread_mutex_t lock;
} packet_buffer_t;

/* 函数签名 */
int common_init(void);
void *capture_thread(void *arg);
void get_mac(char role, int id1, int id2, char *mac_buf);

static inline uint16_t hash_oaddr(uint16_t oaddr) {
    uint32_t h = oaddr;
    h ^= h >> 16;
    h *= UINT32_C(0x85ebca6b);
    h ^= h >> 13;
    h *= UINT32_C(0xc2b2ae35);
    h ^= h >> 16;
    return h;
}

/* 全局变量 */
packet_buffer_t pkt_buffer;
net_device_t devices[MAX_DEVICES];
int device_count = 0;
volatile int stop = 0; // 程序终止标志

#endif // COMMON_H