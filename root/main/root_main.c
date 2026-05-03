#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ss_mesh.h"
#include "ss_protocol.h"

static const char *TAG = "ss_root";

typedef struct {
    uint8_t node_mac[6];
    uint32_t session_id;
} pending_response_t;

static QueueHandle_t s_pending_queue;

static void print_hex(const uint8_t *data, uint8_t len)
{
    for (int i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
}

static void send_response(const uint8_t node_mac[6], uint32_t session_id, const char *text)
{
    ss_mesh_packet_t packet = {
        .magic = SS_PROTOCOL_MAGIC,
        .version = SS_PROTOCOL_VERSION,
        .type = SS_PACKET_ROOT_RESPONSE,
        .session_id = session_id,
    };
    size_t len = strlen(text);
    if (len > SS_PROTOCOL_MAX_PAYLOAD) {
        len = SS_PROTOCOL_MAX_PAYLOAD;
    }
    memcpy(packet.node_mac, node_mac, sizeof(packet.node_mac));
    packet.payload_len = len;
    memcpy(packet.payload, text, len);

    esp_err_t err = ss_mesh_send_to_node(node_mac, &packet, sizeof(packet));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send response to " MACSTR " failed: %s", MAC2STR(node_mac), esp_err_to_name(err));
    }
}

static void send_color_response(const uint8_t node_mac[6], uint32_t session_id, uint8_t red, uint8_t green,
                                uint8_t blue)
{
    ss_mesh_packet_t packet = {
        .magic = SS_PROTOCOL_MAGIC,
        .version = SS_PROTOCOL_VERSION,
        .type = SS_PACKET_COLOR_RESPONSE,
        .session_id = session_id,
        .payload_len = 3,
        .payload = { red, green, blue },
    };
    memcpy(packet.node_mac, node_mac, sizeof(packet.node_mac));

    esp_err_t err = ss_mesh_send_to_node(node_mac, &packet, sizeof(packet));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send color to " MACSTR " failed: %s", MAC2STR(node_mac), esp_err_to_name(err));
    }
}

static void mesh_recv_cb(mesh_addr_t *from, mesh_data_t *data)
{
    if (data->size < sizeof(ss_mesh_packet_t)) {
        ESP_LOGW(TAG, "short packet from " MACSTR, MAC2STR(from->addr));
        return;
    }

    ss_mesh_packet_t packet;
    memcpy(&packet, data->data, sizeof(packet));
    if (packet.magic != SS_PROTOCOL_MAGIC || packet.version != SS_PROTOCOL_VERSION) {
        ESP_LOGW(TAG, "unknown packet from " MACSTR, MAC2STR(from->addr));
        return;
    }

    if (packet.type == SS_PACKET_UUID_REQUEST) {
        printf("UUID_REQUEST session=%" PRIu32 " node=" MACSTR " uuid=", packet.session_id,
               MAC2STR(packet.node_mac));
        printf("%.*s", packet.payload_len, (char *)packet.payload);
        printf(" uuid_hex=");
        print_hex(packet.payload, packet.payload_len);
        printf("\n");
        fflush(stdout);

        pending_response_t pending = {
            .session_id = packet.session_id,
        };
        memcpy(pending.node_mac, packet.node_mac, sizeof(pending.node_mac));
        xQueueOverwrite(s_pending_queue, &pending);
        return;
    }

    if (packet.type != SS_PACKET_NODE_REPORT) {
        ESP_LOGW(TAG, "unsupported packet type %u from " MACSTR, packet.type, MAC2STR(from->addr));
        return;
    }

    printf("NODE_REPORT session=%" PRIu32 " node=" MACSTR " ble=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d distance_cm=%u payload_hex=",
           packet.session_id, MAC2STR(packet.node_mac), packet.bt_addr[5], packet.bt_addr[4], packet.bt_addr[3],
           packet.bt_addr[2], packet.bt_addr[1], packet.bt_addr[0], packet.rssi, packet.distance_cm);
    print_hex(packet.payload, packet.payload_len);
    printf("\n");
    fflush(stdout);

    pending_response_t pending = {
        .session_id = packet.session_id,
    };
    memcpy(pending.node_mac, packet.node_mac, sizeof(pending.node_mac));
    xQueueOverwrite(s_pending_queue, &pending);
    send_response(packet.node_mac, packet.session_id, CONFIG_SS_ROOT_DEFAULT_RESPONSE);
}

static bool parse_mac(const char *text, uint8_t mac[6])
{
    unsigned int bytes[6];
    int parsed = sscanf(text, "%02x:%02x:%02x:%02x:%02x:%02x", &bytes[0], &bytes[1], &bytes[2],
                        &bytes[3], &bytes[4], &bytes[5]);
    if (parsed != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)bytes[i];
    }
    return true;
}

static bool parse_hex_color(const char *text, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (text[0] == '#') {
        text++;
    }

    if (strlen(text) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)text[i])) {
            return false;
        }
    }

    unsigned int value;
    if (sscanf(text, "%06x", &value) != 1) {
        return false;
    }
    *red = (value >> 16) & 0xff;
    *green = (value >> 8) & 0xff;
    *blue = value & 0xff;
    return true;
}

static void print_routes(void)
{
    mesh_addr_t route_table[CONFIG_MESH_ROUTE_TABLE_SIZE];
    int route_table_size = 0;
    esp_err_t err = esp_mesh_get_routing_table(route_table, sizeof(route_table), &route_table_size);
    if (err != ESP_OK) {
        printf("ERR routes %s\n", esp_err_to_name(err));
        return;
    }
    printf("ROUTES count=%d\n", route_table_size);
    for (int i = 0; i < route_table_size; i++) {
        printf("%d " MACSTR "\n", i, MAC2STR(route_table[i].addr));
    }
}

static void print_help(void)
{
    printf("Commands:\n");
    printf("help - show commands\n");
    printf("ping - healthcheck root serial\n");
    printf("routes - print mesh routing table\n");
    printf("send <node_mac> <text> - send response to node\n");
    printf("reply <text> - send response to last reporting node\n");
    printf("color <#RRGGBB> - send color to last UUID/reporting node\n");
    printf("sendcolor <node_mac> <#RRGGBB> - send color to node\n");
}

static void serial_task(void *arg)
{
    char line[160];
    pending_response_t pending = { 0 };

    print_help();
    while (true) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "help") == 0) {
            print_help();
        } else if (strcasecmp(line, "ping") == 0) {
            printf("PONG\n");
            fflush(stdout);
        } else if (strcmp(line, "routes") == 0) {
            print_routes();
        } else if (strncmp(line, "reply ", 6) == 0) {
            if (xQueuePeek(s_pending_queue, &pending, 0) != pdTRUE) {
                printf("ERR no pending node\n");
                continue;
            }
            send_response(pending.node_mac, pending.session_id, line + 6);
        } else if (strncmp(line, "color ", 6) == 0) {
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            if (xQueuePeek(s_pending_queue, &pending, 0) != pdTRUE) {
                printf("ERR no pending node\n");
                continue;
            }
            if (!parse_hex_color(line + 6, &red, &green, &blue)) {
                printf("ERR invalid color\n");
                continue;
            }
            send_color_response(pending.node_mac, pending.session_id, red, green, blue);
            printf("COLOR_SENT node=" MACSTR " session=%" PRIu32 " color=%02x%02x%02x\n",
                   MAC2STR(pending.node_mac), pending.session_id, red, green, blue);
        } else if (strncmp(line, "send ", 5) == 0) {
            char *mac_text = line + 5;
            char *payload = strchr(mac_text, ' ');
            uint8_t mac[6];
            if (payload == NULL) {
                printf("ERR usage: send <node_mac> <text>\n");
                continue;
            }
            *payload++ = '\0';
            if (!parse_mac(mac_text, mac)) {
                printf("ERR invalid mac\n");
                continue;
            }
            send_response(mac, 0, payload);
        } else if (strncmp(line, "sendcolor ", 10) == 0) {
            char *mac_text = line + 10;
            char *color = strchr(mac_text, ' ');
            uint8_t mac[6];
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            if (color == NULL) {
                printf("ERR usage: sendcolor <node_mac> <#RRGGBB>\n");
                continue;
            }
            *color++ = '\0';
            if (!parse_mac(mac_text, mac)) {
                printf("ERR invalid mac\n");
                continue;
            }
            if (!parse_hex_color(color, &red, &green, &blue)) {
                printf("ERR invalid color\n");
                continue;
            }
            send_color_response(mac, 0, red, green, blue);
        } else if (line[0] != '\0') {
            printf("ERR unknown command\n");
        }
    }
}

void app_main(void)
{
    s_pending_queue = xQueueCreate(1, sizeof(pending_response_t));
    ESP_ERROR_CHECK(s_pending_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(ss_mesh_init(mesh_recv_cb, true));
    xTaskCreate(serial_task, "serial", 4096, NULL, 5, NULL);
}
