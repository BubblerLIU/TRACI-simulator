/*
 * common.h - 共享结构体和函数
 */

#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <pcap/pcap.h>
#include <pthread.h>
#include <signal.h>

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
#define MAX_LINE 21
#define MAX_FILENAME 21

/* 以太网头 */
typedef struct {
    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint16_t ether_type;
} __attribute__((packed)) eth_header_t;

/* 自定义头部 */
typedef struct {
    uint32_t seq_num;
    uint32_t iaddr;
    uint32_t oaddr;
    uint8_t traci_type; // 0x01 request, 0x02 response
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
void common_setup_signal_handlers(void);
int common_init(void);
void common_shutdown(void);
void *capture_thread(void *arg);
void get_mac(char role, int id1, int id2, uint8_t mac[6]);
int send_packet(net_device_t *dev, const uint8_t *data, uint32_t len);

static inline uint16_t hash_oaddr(uint32_t oaddr) {
    uint32_t h = oaddr;
    h ^= h >> 16;
    h *= UINT32_C(0x85ebca6b);
    h ^= h >> 13;
    h *= UINT32_C(0xc2b2ae35);
    h ^= h >> 16;

    /*
     * Leaf uses hash_oaddr(oaddr) % spine_count.  For small power-of-two
     * spine counts that mostly samples the low bits, so fold middle bits
     * down instead of returning the raw low 16 bits.
     */
    return (uint16_t)(h ^ (h >> 8));
}

/* 全局变量 */
extern packet_buffer_t pkt_buffer;
extern net_device_t devices[MAX_DEVICES];
extern int device_count;
extern volatile sig_atomic_t stop; // 程序终止标志

/* 本地环境变量 */
extern const char *node_role;
extern const char *node_id;
extern const char *gpu_per_leaf;
extern const char *spine_num;

#endif // COMMON_H
