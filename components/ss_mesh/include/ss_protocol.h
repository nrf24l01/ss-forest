#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SS_PROTOCOL_MAGIC 0x5346u
#define SS_PROTOCOL_VERSION 1u
#define SS_PROTOCOL_MAX_PAYLOAD 64u

typedef enum {
    SS_PACKET_NODE_REPORT = 1,
    SS_PACKET_ROOT_RESPONSE = 2,
    SS_PACKET_UUID_REQUEST = 3,
    SS_PACKET_COLOR_RESPONSE = 4,
} ss_packet_type_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    uint32_t session_id;
    uint8_t node_mac[6];
    uint8_t bt_addr[6];
    int8_t rssi;
    uint16_t distance_cm;
    uint8_t payload_len;
    uint8_t payload[SS_PROTOCOL_MAX_PAYLOAD];
} ss_mesh_packet_t;

#ifdef __cplusplus
}
#endif
