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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ss_mesh.h"
#include "ss_mesh_tree.h"
#include "ss_protocol.h"

static const char *TAG = "ss_root";

typedef struct {
    uint8_t node_mac[6];
    uint32_t session_id;
} pending_response_t;

typedef struct {
    mesh_addr_t from;
    ss_mesh_packet_t packet;
} mesh_rx_item_t;

#define MAX_PENDING_REQUESTS 16
#define PENDING_REQUEST_TIMEOUT_MS 30000

typedef struct {
    pending_response_t entries[MAX_PENDING_REQUESTS];
    TickType_t created_at[MAX_PENDING_REQUESTS];
    size_t count;
} pending_requests_t;

static pending_requests_t s_pending_requests;
static SemaphoreHandle_t s_pending_lock;
static QueueHandle_t s_mesh_rx_queue;

static void pending_prune_expired(void)
{
    TickType_t now = xTaskGetTickCount();

    for (size_t i = 0; i < s_pending_requests.count;) {
        if (now - s_pending_requests.created_at[i] >= pdMS_TO_TICKS(PENDING_REQUEST_TIMEOUT_MS)) {
            ESP_LOGI(TAG, "expired pending request for " MACSTR " session=%" PRIu32,
                     MAC2STR(s_pending_requests.entries[i].node_mac), s_pending_requests.entries[i].session_id);
            size_t last = --s_pending_requests.count;
            s_pending_requests.entries[i] = s_pending_requests.entries[last];
            s_pending_requests.created_at[i] = s_pending_requests.created_at[last];
            continue;
        }
        i++;
    }
}

static void print_hex(const uint8_t *data, uint8_t len)
{
    for (int i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
}

static esp_err_t send_color_response(const uint8_t node_mac[6], uint32_t session_id, uint8_t red, uint8_t green,
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
    return err;
}

static esp_err_t send_color_query(const uint8_t node_mac[6])
{
    ss_mesh_packet_t packet = {
        .magic = SS_PROTOCOL_MAGIC,
        .version = SS_PROTOCOL_VERSION,
        .type = SS_PACKET_COLOR_QUERY,
    };
    memcpy(packet.node_mac, node_mac, sizeof(packet.node_mac));
    return ss_mesh_send_to_node(node_mac, &packet, sizeof(packet));
}

static bool pending_add(const uint8_t node_mac[6], uint32_t session_id)
{
    bool added = false;
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    pending_prune_expired();
    for (size_t i = 0; i < s_pending_requests.count; i++) {
        if (memcmp(s_pending_requests.entries[i].node_mac, node_mac, 6) == 0) {
            // A node can only have one active LED request. Replace an expired
            // request so a later color command cannot target a stale session.
            s_pending_requests.entries[i].session_id = session_id;
            s_pending_requests.created_at[i] = xTaskGetTickCount();
            added = true;
            break;
        }
    }
    if (!added && s_pending_requests.count < MAX_PENDING_REQUESTS) {
        pending_response_t *entry = &s_pending_requests.entries[s_pending_requests.count++];
        memcpy(entry->node_mac, node_mac, sizeof(entry->node_mac));
        entry->session_id = session_id;
        s_pending_requests.created_at[s_pending_requests.count - 1] = xTaskGetTickCount();
        added = true;
    }
    xSemaphoreGive(s_pending_lock);
    return added;
}

static bool pending_take(const uint8_t node_mac[6], pending_response_t *pending)
{
    bool found = false;
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    pending_prune_expired();
    for (size_t i = 0; i < s_pending_requests.count; i++) {
        if (memcmp(s_pending_requests.entries[i].node_mac, node_mac, 6) == 0) {
            *pending = s_pending_requests.entries[i];
            size_t last = --s_pending_requests.count;
            s_pending_requests.entries[i] = s_pending_requests.entries[last];
            s_pending_requests.created_at[i] = s_pending_requests.created_at[last];
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_pending_lock);
    return found;
}

static void process_mesh_packet(const ss_mesh_packet_t *packet, const mesh_addr_t *from)
{
    if (packet->magic != SS_PROTOCOL_MAGIC || packet->version != SS_PROTOCOL_VERSION) {
        ESP_LOGW(TAG, "unknown packet from " MACSTR, MAC2STR(from->addr));
        return;
    }

    if (packet->type == SS_PACKET_UUID_REQUEST) {
        if (packet->payload_len != SS_TEAM_UUID_SIZE) {
            ESP_LOGW(TAG, "invalid UUID length from " MACSTR, MAC2STR(from->addr));
            return;
        }
        printf("UUID_REQUEST session=%" PRIu32 " node=" MACSTR " uuid=", packet->session_id,
               MAC2STR(packet->node_mac));
        print_hex(packet->payload, packet->payload_len);
        printf(" attack_points=%u rssi=%d distance_cm=%u uuid_hex=", packet->attack_points, packet->rssi,
               packet->distance_cm);
        print_hex(packet->payload, packet->payload_len);
        printf("\n");
        fflush(stdout);

        if (!pending_add(packet->node_mac, packet->session_id)) {
            ESP_LOGW(TAG, "pending request capacity reached for " MACSTR, MAC2STR(packet->node_mac));
        }
        return;
    }

    if (packet->type == SS_PACKET_COLOR_STATUS) {
        if (packet->payload_len != 3) {
            ESP_LOGW(TAG, "invalid color status length from " MACSTR, MAC2STR(from->addr));
            return;
        }
        printf("COLOR_STATUS node=" MACSTR " color=%02x%02x%02x\n", MAC2STR(packet->node_mac),
               packet->payload[0], packet->payload[1], packet->payload[2]);
        fflush(stdout);
        return;
    }

    if (packet->type != SS_PACKET_NODE_REPORT) {
        ESP_LOGW(TAG, "unsupported packet type %u from " MACSTR, packet->type, MAC2STR(from->addr));
        return;
    }

    printf("NODE_REPORT session=%" PRIu32 " node=" MACSTR " ble=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d distance_cm=%u payload_hex=",
           packet->session_id, MAC2STR(packet->node_mac), packet->bt_addr[5], packet->bt_addr[4], packet->bt_addr[3],
           packet->bt_addr[2], packet->bt_addr[1], packet->bt_addr[0], packet->rssi, packet->distance_cm);
    print_hex(packet->payload, packet->payload_len);
    printf("\n");
    fflush(stdout);

}

static void mesh_rx_task(void *arg)
{
    mesh_rx_item_t received;

    while (xQueueReceive(s_mesh_rx_queue, &received, portMAX_DELAY) == pdTRUE) {
        process_mesh_packet(&received.packet, &received.from);
    }
}

static void mesh_recv_cb(mesh_addr_t *from, mesh_data_t *data)
{
    if (data->size != sizeof(ss_mesh_packet_t) || data->data == NULL) {
        ESP_LOGW(TAG, "invalid packet size from " MACSTR, MAC2STR(from->addr));
        return;
    }

    mesh_rx_item_t received = { 0 };
    received.from = *from;
    memcpy(&received.packet, data->data, sizeof(received.packet));
    if (received.packet.payload_len > SS_PROTOCOL_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "invalid payload length from " MACSTR, MAC2STR(from->addr));
        return;
    }
    if (xQueueSend(s_mesh_rx_queue, &received, 0) != pdTRUE) {
        ESP_LOGW(TAG, "mesh RX queue full, dropping packet from " MACSTR, MAC2STR(from->addr));
    }
}

static bool parse_mac(const char *text, uint8_t mac[6])
{
    if (strlen(text) != 17) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        int offset = i * 3;
        if (!isxdigit((unsigned char)text[offset]) || !isxdigit((unsigned char)text[offset + 1]) ||
            (i < 5 && text[offset + 2] != ':')) {
            return false;
        }
        uint8_t high = isdigit((unsigned char)text[offset]) ? text[offset] - '0' :
                       (uint8_t)(tolower((unsigned char)text[offset]) - 'a' + 10);
        uint8_t low = isdigit((unsigned char)text[offset + 1]) ? text[offset + 1] - '0' :
                      (uint8_t)(tolower((unsigned char)text[offset + 1]) - 'a' + 10);
        mac[i] = (high << 4) | low;
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

static void print_nodes(void)
{
    mesh_addr_t route_table[CONFIG_MESH_ROUTE_TABLE_SIZE];
    int route_table_size = 0;
    esp_err_t err = esp_mesh_get_routing_table(route_table, sizeof(route_table), &route_table_size);
    if (err != ESP_OK) {
        printf("ERR nodes %s\n", esp_err_to_name(err));
        return;
    }
    printf("NODES count=%d\n", route_table_size);
    for (int i = 0; i < route_table_size; i++) {
        printf(MACSTR "\n", MAC2STR(route_table[i].addr));
    }
}

static void print_tree(void)
{
    ss_mesh_tree_t tree;
    esp_err_t err = ss_mesh_tree_build(&tree);
    if (err != ESP_OK) {
        printf("ERR tree %s\n", esp_err_to_name(err));
        return;
    }
    printf("TREE count=%u complete=%d\n", tree.count, tree.complete);
    for (uint16_t i = 0; i < tree.count; i++) {
        ss_mesh_tree_node_t *node = &tree.nodes[i];
        printf("%u " MACSTR " parent=" MACSTR " parent_known=%d direct_child=%d\n", i,
               MAC2STR(node->mac), MAC2STR(node->parent_mac), node->parent_known, node->direct_child);
    }
}

static void print_help(void)
{
    printf("Commands:\n");
    printf("help - show commands\n");
    printf("ping - healthcheck root serial\n");
    printf("routes - print mesh routing table\n");
    printf("tree - print the current mesh tree\n");
    printf("nodes - print connected node MAC addresses\n");
    printf("getcolor <node_mac> - request a node's displayed color\n");
    printf("color <node_mac> <#RRGGBB> - respond to that node's pending request\n");
}

static void serial_task(void *arg)
{
    char line[160];
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
        } else if (strcmp(line, "tree") == 0) {
            print_tree();
        } else if (strcmp(line, "nodes") == 0) {
            print_nodes();
        } else if (strncmp(line, "getcolor ", 9) == 0) {
            uint8_t mac[6];
            if (!parse_mac(line + 9, mac)) {
                printf("ERR usage: getcolor <node_mac>\n");
                continue;
            }
            esp_err_t err = send_color_query(mac);
            if (err == ESP_OK) {
                printf("COLOR_QUERY_SENT node=" MACSTR "\n", MAC2STR(mac));
            } else {
                printf("ERR getcolor %s\n", esp_err_to_name(err));
            }
        } else if (strncmp(line, "color ", 6) == 0) {
            char *mac_text = line + 6;
            char *color = strchr(mac_text, ' ');
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            uint8_t mac[6];
            pending_response_t pending;
            if (color == NULL) {
                printf("ERR usage: color <node_mac> <#RRGGBB>\n");
                continue;
            }
            *color++ = '\0';
            if (!parse_mac(mac_text, mac) || !parse_hex_color(color, &red, &green, &blue)) {
                printf("ERR invalid mac or color\n");
                continue;
            }
            if (!pending_take(mac, &pending)) {
                printf("ERR no pending request for " MACSTR "\n", MAC2STR(mac));
                continue;
            }
            esp_err_t err = send_color_response(pending.node_mac, pending.session_id, red, green, blue);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "send color to " MACSTR " failed: %s", MAC2STR(mac), esp_err_to_name(err));
                pending_add(pending.node_mac, pending.session_id);
                printf("ERR color %s\n", esp_err_to_name(err));
                continue;
            }
            printf("COLOR_SENT node=" MACSTR " session=%" PRIu32 " color=%02x%02x%02x\n",
                   MAC2STR(pending.node_mac), pending.session_id, red, green, blue);
        } else if (line[0] != '\0') {
            printf("ERR unknown command\n");
        }
    }
}

void app_main(void)
{
    s_mesh_rx_queue = xQueueCreate(8, sizeof(mesh_rx_item_t));
    s_pending_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_pending_lock == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(s_mesh_rx_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(ss_mesh_init(mesh_recv_cb, true));
    xTaskCreate(serial_task, "serial", 4096, NULL, 5, NULL);
    xTaskCreate(mesh_rx_task, "mesh_rx_proc", 4096, NULL, 5, NULL);
}
