/*
 * common.h - 共享结构体和函数
 */

#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <pcap/pcap.h>

/* 宏定义 */
#define MAC_PREFIX "aa:bb:cc"
#define ETH_TYPE 0xAAAA
#define MAX_DEVICES 128

/* 以太网头 */
typedef struct {
    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint16_t ether_type;
} __attribute__((packed)) eth_header_t;

/* 函数签名 */
void get_mac(char role, int id1, int id2, char *mac_buf);
int hash_oaddr(uint32_t oaddr);

#endif // COMMON_H