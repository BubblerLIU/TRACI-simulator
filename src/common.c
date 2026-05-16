/*
 * common.c - common.h 中函数的实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>
#include "common.h"

/* 全局变量 */
packet_buffer_t pkt_buffer;
net_device_t devices[MAX_DEVICES];
int device_count = 0;
volatile sig_atomic_t stop = 0; // 程序终止标志
static int pkt_buffer_initialized = 0;

/* 本地环境变量 */
const char *node_role = NULL;
const char *node_id = NULL;
const char *gpu_per_leaf = NULL;
const char *spine_num = NULL;

/*
 * handle_signal - 收到终止信号时只设置退出标志
 */
static void handle_signal(int signo) {
    (void)signo;
    stop = 1;
}

/*
 * common_setup_signal_handlers - 注册主程序终止信号
 */
void common_setup_signal_handlers(void) {
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);

    sigaction(SIGTERM, &action, NULL);
}

/*
 * common_init - 扫描网络设备并创建对应线程
 */
int common_init(void) {
    char errbuf[PCAP_ERRBUF_SIZE];

    // 初始化包缓冲区
    memset(&pkt_buffer, 0, sizeof(pkt_buffer));
    pkt_buffer.head = pkt_buffer.tail = 0;
    pthread_mutex_init(&pkt_buffer.lock, NULL);
    pkt_buffer_initialized = 1;

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
            if (pcap_setdirection(devices[device_count].handle, PCAP_D_IN) == -1) {
                fprintf(stderr, "%s %s: Failed to set direction for %s: %s\n",
                    node_role, node_id, d->name,
                    pcap_geterr(devices[device_count].handle));
            }

            // 创建监听线程
            if (pthread_create(&devices[device_count].thread_id,
                NULL, capture_thread, &devices[device_count]) != 0) {
                    fprintf(stderr, "%s %s: Failed to create thread for %s\n",
                        node_role, node_id, d->name);
                pcap_close(devices[device_count].handle);
                devices[device_count].handle = NULL;
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
 * common_shutdown - 清理抓包线程、pcap 句柄和缓冲区锁
 */
void common_shutdown(void) {
    stop = 1;

    for (int i = 0; i < device_count; ++i) {
        if (devices[i].handle != NULL) {
            pcap_breakloop(devices[i].handle);
        }
    }

    for (int i = 0; i < device_count; ++i) {
        pthread_join(devices[i].thread_id, NULL);
    }

    for (int i = 0; i < device_count; ++i) {
        if (devices[i].handle != NULL) {
            pcap_close(devices[i].handle);
            devices[i].handle = NULL;
        }
    }

    if (pkt_buffer_initialized) {
        pthread_mutex_destroy(&pkt_buffer.lock);
        pkt_buffer_initialized = 0;
    }

    memset(&pkt_buffer, 0, sizeof(pkt_buffer));
    memset(devices, 0, sizeof(devices));
    device_count = 0;
}

/*
 * capture_thread - 监听线程
 */
void *capture_thread(void *arg) {
    net_device_t *dev = (net_device_t *)arg;
    struct pcap_pkthdr header;
    const u_char *packet;
    
    printf("%s %s: Starting capture on %s\n", node_role, node_id, dev->name);
    
    while (!stop) {
        packet = pcap_next(dev->handle, &header);
        if (!packet || stop) continue;
        
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
        entry->len = header.caplen > PACKET_BUF_SIZE ?
            PACKET_BUF_SIZE : header.caplen;
        entry->timestamp = header.ts.tv_sec * 1000000 + header.ts.tv_usec;
        memcpy(entry->data, packet, entry->len);
        
        pkt_buffer.head = (pkt_buffer.head + 1) % MAX_PACKETS;
        pthread_mutex_unlock(&pkt_buffer.lock);
    }

    printf("%s %s: Stopping capture on %s\n", node_role, node_id, dev->name);
    
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
    if (stop) {
        return -1;
    }

    if (pcap_inject(dev->handle, data, len) == -1) {
        fprintf(stderr, "%s %s: Error sending packet on %s: %s\n",
               node_role, node_id, dev->name, pcap_geterr(dev->handle));
        return -1;
    }
    return 0;
}

/*
 * parse_sim_mode_args - 解析 -b/-t 模式参数
 */
int parse_sim_mode_args(int argc, char **argv, sim_mode_t *mode,
    const char *program) {

    if (mode == NULL) {
        return -1;
    }

    *mode = SIM_MODE_BASELINE;
    int mode_specified = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-b") == 0) {
            if (mode_specified) {
                fprintf(stderr, "Usage: %s [-b|-t]\n", program);
                return -1;
            }
            *mode = SIM_MODE_BASELINE;
            mode_specified = 1;
        }
        else if (strcmp(argv[i], "-t") == 0) {
            if (mode_specified) {
                fprintf(stderr, "Usage: %s [-b|-t]\n", program);
                return -1;
            }
            *mode = SIM_MODE_TRACI;
            mode_specified = 1;
        }
        else {
            fprintf(stderr, "Usage: %s [-b|-t]\n", program);
            return -1;
        }
    }

    return 0;
}

const char *sim_mode_name(sim_mode_t mode) {
    return mode == SIM_MODE_TRACI ? "TRACI" : "Baseline";
}

/*
 * rtb_init - 初始化 RTB
 */
void rtb_init(rtb_table_t *rtb) {
    memset(rtb, 0, sizeof(*rtb));
}

static rtb_entry_t *rtb_find(rtb_table_t *rtb, uint32_t tag) {
    for (int i = 0; i < RTB_ENTRY_NUM; ++i) {
        if (rtb->entries[i].valid && rtb->entries[i].tag == tag) {
            return &rtb->entries[i];
        }
    }
    return NULL;
}

static rtb_entry_t *rtb_find_free(rtb_table_t *rtb) {
    for (int i = 0; i < RTB_ENTRY_NUM; ++i) {
        if (!rtb->entries[i].valid) {
            return &rtb->entries[i];
        }
    }
    return NULL;
}

/*
 * rtb_track_request - request 经过交换机时按 OAddr 创建/命中 RTB
 */
rtb_request_result_t rtb_track_request(rtb_table_t *rtb,
    const traci_header_t *traci, int can_stall) {

    rtb_entry_t *entry = rtb_find(rtb, traci->oaddr);
    if (entry == NULL) {
        entry = rtb_find_free(rtb);
        if (entry == NULL) {
            return can_stall ? RTB_REQUEST_STALL : RTB_REQUEST_BYPASS;
        }

        memset(entry, 0, sizeof(*entry));
        entry->valid = 1;
        entry->tag = traci->oaddr;
        entry->seq_num = traci->seq_num;
    }

    entry->waiting_count += 1;
    return RTB_REQUEST_TRACKED;
}

/*
 * rtb_reduce_response - response 命中 RTB 时累加并按需生成聚合 response
 */
rtb_response_result_t rtb_reduce_response(rtb_table_t *rtb,
    traci_header_t *traci) {

    rtb_entry_t *entry = rtb_find(rtb, traci->oaddr);
    if (entry == NULL) {
        return RTB_RESPONSE_BYPASS;
    }

    if (entry->waiting_count == 0) {
        fprintf(stderr, "%s %s: RTB entry for OAddr=%" PRIu32
            " has zero waiting count\n", node_role, node_id, traci->oaddr);
        entry->valid = 0;
        return RTB_RESPONSE_BYPASS;
    }

    uint32_t response_count = traci->count == 0 ? 1 : traci->count;
    if (response_count > entry->waiting_count) {
        fprintf(stderr, "%s %s: RTB response count %" PRIu32
            " exceeds waiting count %" PRIu32 " for OAddr=%" PRIu32 "\n",
            node_role, node_id, response_count, entry->waiting_count,
            traci->oaddr);
        response_count = entry->waiting_count;
    }

    entry->data += traci->data;
    entry->waiting_count -= response_count;
    entry->arrived_count += response_count;

    if (entry->waiting_count > 0) {
        return RTB_RESPONSE_DROP;
    }

    traci->seq_num = entry->seq_num;
    traci->iaddr = 0;
    traci->oaddr = entry->tag;
    traci->count = entry->arrived_count;
    traci->data = entry->data;
    traci->traci_type = TRACI_TYPE_RESPONSE;
    entry->valid = 0;

    return RTB_RESPONSE_EVOKE;
}

/*
 * isc_init - 初始化 ISC
 */
void isc_init(isc_table_t *isc) {
    memset(isc, 0, sizeof(*isc));
}

/*
 * isc_lookup - 按 IAddr 查找完整缓存块
 */
int isc_lookup(isc_table_t *isc, uint32_t iaddr, uint32_t *data) {
    for (int i = 0; i < ISC_ENTRY_NUM; ++i) {
        if (isc->entries[i].valid && isc->entries[i].tag == iaddr) {
            if (data != NULL) {
                *data = isc->entries[i].data;
            }
            return 1;
        }
    }

    return 0;
}

/*
 * isc_insert - response 经过交换机时插入/更新缓存
 */
void isc_insert(isc_table_t *isc, uint32_t iaddr, uint32_t data) {
    isc_entry_t *entry = NULL;

    for (int i = 0; i < ISC_ENTRY_NUM; ++i) {
        if (isc->entries[i].valid && isc->entries[i].tag == iaddr) {
            entry = &isc->entries[i];
            break;
        }
    }

    if (entry == NULL) {
        for (int i = 0; i < ISC_ENTRY_NUM; ++i) {
            if (!isc->entries[i].valid) {
                entry = &isc->entries[i];
                break;
            }
        }
    }

    if (entry == NULL) {
        entry = &isc->entries[isc->next_evict];
        isc->next_evict = (isc->next_evict + 1) % ISC_ENTRY_NUM;
    }

    entry->valid = 1;
    entry->tag = iaddr;
    entry->data = data;
}
