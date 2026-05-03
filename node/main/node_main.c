#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "led_strip.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "ss_mesh.h"
#include "ss_protocol.h"

static const char *TAG = "ss_node";

#ifndef CONFIG_SS_BLE_GATT_DEVICE_NAME
#define CONFIG_SS_BLE_GATT_DEVICE_NAME "SS-FOREST-NODE"
#endif

static const ble_uuid128_t SS_SERVICE_UUID = BLE_UUID128_INIT(0x70, 0x6c, 0x9a, 0x02, 0xf4, 0x6d, 0x48, 0x83,
                                                             0x9b, 0x6e, 0x13, 0x8e, 0x7a, 0x8f, 0x00, 0x01);
static const ble_uuid128_t SS_UUID_CHAR_UUID = BLE_UUID128_INIT(0x70, 0x6c, 0x9a, 0x02, 0xf4, 0x6d, 0x48, 0x83,
                                                               0x9b, 0x6e, 0x13, 0x8e, 0x7a, 0x8f, 0x00, 0x02);

typedef enum {
    LED_OFF,
    LED_KEEP_COLOR,
    LED_MESH_INIT,
    LED_BLUE_FAST,
    LED_BLUE_SLOW,
    LED_YELLOW_SOLID,
    LED_GREEN_SOLID,
    LED_GREEN_FAST,
    LED_RED_SOLID,
} led_state_t;

typedef struct {
    ble_addr_t addr;
    int8_t rssi;
    uint8_t payload_len;
    uint8_t payload[SS_PROTOCOL_MAX_PAYLOAD];
    bool found;
} ble_target_t;

typedef struct {
    uint8_t len;
    uint8_t value[SS_PROTOCOL_MAX_PAYLOAD];
} uuid_request_t;

typedef struct {
    QueueHandle_t button_queue;
    QueueHandle_t uuid_queue;
    QueueHandle_t response_queue;
    SemaphoreHandle_t scan_lock;
    led_strip_handle_t led_strip;
    led_state_t led_state;
    bool mesh_connected_once;
    bool mesh_connected;
    uint8_t color_rgb[3];
    ble_target_t best_target;
    uint32_t current_session_id;
} node_ctx_t;

static node_ctx_t s_ctx;
static uint8_t s_own_addr_type;

static void start_ble_advertising(void);

static uint16_t estimate_distance_cm(int8_t rssi)
{
    float ratio = ((float)CONFIG_SS_BLE_RSSI_AT_ONE_METER - (float)rssi) /
                  ((float)CONFIG_SS_BLE_PATH_LOSS_EXPONENT_X10 / 10.0f * 10.0f);
    float meters = powf(10.0f, ratio);

    if (meters < 0.01f) {
        meters = 0.01f;
    }
    if (meters > 10.0f) {
        meters = 10.0f;
    }
    return (uint16_t)(meters * 100.0f);
}

static void led_set_pixels(uint8_t color_red, uint8_t color_green, uint8_t color_blue,
                           uint8_t status_red, uint8_t status_green, uint8_t status_blue)
{
    if (CONFIG_SS_LED_COUNT > 0) {
        led_strip_set_pixel(s_ctx.led_strip, 0, color_red, color_green, color_blue);
    }
    if (CONFIG_SS_LED_COUNT > 1) {
        led_strip_set_pixel(s_ctx.led_strip, 1, status_red, status_green, status_blue);
    }
    for (int i = 2; i < CONFIG_SS_LED_COUNT; i++) {
        led_strip_set_pixel(s_ctx.led_strip, i, 0, 0, 0);
    }
    led_strip_refresh(s_ctx.led_strip);
}

static void led_set_color_from_payload(const uint8_t *payload, uint8_t payload_len)
{
    if (payload_len == 0) {
        s_ctx.color_rgb[0] = 0;
        s_ctx.color_rgb[1] = 0;
        s_ctx.color_rgb[2] = 0;
        return;
    }
    if (payload_len != 3) {
        ESP_LOGW(TAG, "invalid color payload length: %u", payload_len);
        return;
    }
    s_ctx.color_rgb[0] = payload[0];
    s_ctx.color_rgb[1] = payload[1];
    s_ctx.color_rgb[2] = payload[2];
}

static void led_task(void *arg)
{
    bool mesh_on = false;
    bool ble_on = false;
    led_state_t last_state = -1;
    uint8_t last_color[3] = { 0xff, 0xff, 0xff };
    uint8_t last_status[3] = { 0xff, 0xff, 0xff };

    while (true) {
        led_state_t mesh_state = s_ctx.mesh_connected ? LED_GREEN_SOLID :
                                 (s_ctx.mesh_connected_once ? LED_RED_SOLID : LED_MESH_INIT);
        bool use_ble_color = !(s_ctx.led_state == LED_OFF ||
                               s_ctx.led_state == LED_KEEP_COLOR ||
                               s_ctx.led_state == LED_MESH_INIT);

        uint8_t color_red = s_ctx.color_rgb[0];
        uint8_t color_green = s_ctx.color_rgb[1];
        uint8_t color_blue = s_ctx.color_rgb[2];
        uint8_t status_red = 0;
        uint8_t status_green = 0;
        uint8_t status_blue = 0;
        uint32_t mesh_delay_ms = 250;
        uint32_t ble_delay_ms = 250;

        switch (mesh_state) {
        case LED_OFF:
            mesh_delay_ms = 250;
            break;
        case LED_KEEP_COLOR:
            mesh_delay_ms = 250;
            break;
        case LED_MESH_INIT:
            mesh_on = !mesh_on;
            status_red = mesh_on ? 24 : 0;
            status_blue = mesh_on ? 24 : 0;
            mesh_delay_ms = 250;
            break;
        case LED_BLUE_FAST:
            status_blue = 32;
            mesh_delay_ms = 120;
            break;
        case LED_BLUE_SLOW:
            status_blue = 32;
            mesh_delay_ms = 500;
            break;
        case LED_YELLOW_SOLID:
            status_red = 32;
            status_green = 24;
            mesh_delay_ms = 250;
            break;
        case LED_GREEN_SOLID:
            status_green = 32;
            mesh_delay_ms = 250;
            break;
        case LED_GREEN_FAST:
            status_green = 32;
            mesh_delay_ms = 120;
            break;
        case LED_RED_SOLID:
            status_red = 32;
            mesh_delay_ms = 250;
            break;
        }

        if (use_ble_color) {
            switch (s_ctx.led_state) {
            case LED_BLUE_FAST:
                ble_on = !ble_on;
                color_blue = ble_on ? 32 : 0;
                ble_delay_ms = 120;
                break;
            case LED_BLUE_SLOW:
                ble_on = !ble_on;
                color_blue = ble_on ? 32 : 0;
                ble_delay_ms = 500;
                break;
            case LED_YELLOW_SOLID:
                color_red = 32;
                color_green = 24;
                ble_delay_ms = 250;
                break;
            case LED_GREEN_SOLID:
                color_green = 32;
                ble_delay_ms = 250;
                break;
            case LED_GREEN_FAST:
                ble_on = !ble_on;
                color_green = ble_on ? 32 : 0;
                ble_delay_ms = 120;
                break;
            case LED_RED_SOLID:
                color_red = 32;
                ble_delay_ms = 250;
                break;
            default:
                break;
            }
        }

        uint32_t delay_ms = use_ble_color ? ble_delay_ms : mesh_delay_ms;

        if (last_state != mesh_state ||
            last_color[0] != color_red || last_color[1] != color_green || last_color[2] != color_blue ||
            last_status[0] != status_red || last_status[1] != status_green || last_status[2] != status_blue) {
            led_set_pixels(color_red, color_green, color_blue, status_red, status_green, status_blue);
            last_color[0] = color_red;
            last_color[1] = color_green;
            last_color[2] = color_blue;
            last_status[0] = status_red;
            last_status[1] = status_green;
            last_status[2] = status_blue;
        }

        last_state = mesh_state;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static esp_err_t led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_SS_LED_GPIO,
        .max_leds = CONFIG_SS_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_ctx.led_strip), TAG,
                        "init led strip");
    led_strip_clear(s_ctx.led_strip);
    return ESP_OK;
}

static void button_task(void *arg)
{
    bool old_level;
    uint32_t last_diag = 0;

    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(CONFIG_SS_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    old_level = gpio_get_level(CONFIG_SS_BUTTON_GPIO);
    ESP_LOGI(TAG, "button configured on GPIO%d with internal pull-up, initial level=%d",
             CONFIG_SS_BUTTON_GPIO, old_level);

    while (true) {
        bool new_level = gpio_get_level(CONFIG_SS_BUTTON_GPIO);
        if (!new_level && old_level) {
            uint8_t event = 1;
            ESP_LOGI(TAG, "button press detected on GPIO%d", CONFIG_SS_BUTTON_GPIO);
            xQueueSend(s_ctx.button_queue, &event, 0);
        }
        uint32_t now = xTaskGetTickCount();
        if (now - last_diag > pdMS_TO_TICKS(5000)) {
            ESP_LOGI(TAG, "button GPIO%d level=%d", CONFIG_SS_BUTTON_GPIO, new_level);
            last_diag = now;
        }
        old_level = new_level;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC) {
        const struct ble_gap_disc_desc *disc = &event->disc;
        xSemaphoreTake(s_ctx.scan_lock, portMAX_DELAY);
        if (!s_ctx.best_target.found || disc->rssi > s_ctx.best_target.rssi) {
            s_ctx.best_target.addr = disc->addr;
            s_ctx.best_target.rssi = disc->rssi;
            s_ctx.best_target.payload_len = disc->length_data > SS_PROTOCOL_MAX_PAYLOAD ?
                                            SS_PROTOCOL_MAX_PAYLOAD : disc->length_data;
            memcpy(s_ctx.best_target.payload, disc->data, s_ctx.best_target.payload_len);
            s_ctx.best_target.found = true;
        }
        xSemaphoreGive(s_ctx.scan_lock);
        return 0;
    } else if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (event->connect.status != 0) {
            start_ble_advertising();
        }
        return 0;
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        start_ble_advertising();
        return 0;
    } else if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) {
        start_ble_advertising();
        return 0;
    }

    return 0;
}

static int gatt_uuid_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                               void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > SS_PROTOCOL_MAX_PAYLOAD) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uuid_request_t request = {
        .len = len,
    };
    int rc = ble_hs_mbuf_to_flat(ctxt->om, request.value, sizeof(request.value), NULL);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (xQueueSend(s_ctx.uuid_queue, &request, 0) != pdTRUE) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    ESP_LOGI(TAG, "received UUID over BLE GATT, len=%u", request.len);
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SS_SERVICE_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &SS_UUID_CHAR_UUID.u,
                .access_cb = gatt_uuid_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 },
        },
    },
    { 0 },
};

static void start_ble_advertising(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    const char *name = ble_svc_gap_device_name();

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "set adv fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "start advertising failed: %d", rc);
    }
}

static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE address init failed: %d", rc);
        return;
    }
    start_ble_advertising();
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_init(void)
{
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "init nimble");
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(CONFIG_SS_BLE_GATT_DEVICE_NAME);
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "count gatt cfg failed: %d", rc);
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "add gatt svcs failed: %d", rc);
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

static esp_err_t scan_nearest_device(ble_target_t *target)
{
    struct ble_gap_disc_params params = {
        .filter_duplicates = 1,
        .passive = 0,
        .itvl = 0x0010,
        .window = 0x0010,
    };

    xSemaphoreTake(s_ctx.scan_lock, portMAX_DELAY);
    memset(&s_ctx.best_target, 0, sizeof(s_ctx.best_target));
    xSemaphoreGive(s_ctx.scan_lock);

    int rc = ble_gap_disc(s_own_addr_type, CONFIG_SS_BLE_SCAN_SECONDS * 1000, &params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE scan start failed: %d", rc);
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(CONFIG_SS_BLE_SCAN_SECONDS * 1000 + 100));
    ble_gap_disc_cancel();

    xSemaphoreTake(s_ctx.scan_lock, portMAX_DELAY);
    *target = s_ctx.best_target;
    xSemaphoreGive(s_ctx.scan_lock);

    return target->found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void mesh_recv_cb(mesh_addr_t *from, mesh_data_t *data)
{
    if (data->size < sizeof(ss_mesh_packet_t)) {
        ESP_LOGW(TAG, "short mesh packet from " MACSTR, MAC2STR(from->addr));
        return;
    }

    ss_mesh_packet_t packet;
    memcpy(&packet, data->data, sizeof(packet));
    if (packet.magic != SS_PROTOCOL_MAGIC || packet.version != SS_PROTOCOL_VERSION ||
        (packet.type != SS_PACKET_ROOT_RESPONSE && packet.type != SS_PACKET_COLOR_RESPONSE) ||
        (packet.session_id != s_ctx.current_session_id && packet.session_id != 0)) {
        return;
    }

    if (packet.type == SS_PACKET_COLOR_RESPONSE && packet.session_id == 0) {
        led_set_color_from_payload(packet.payload, packet.payload_len);
        return;
    }

    xQueueSend(s_ctx.response_queue, &packet, 0);
}

static void mesh_status_task(void *arg)
{
    while (true) {
        s_ctx.mesh_connected = ss_mesh_is_connected();
        if (s_ctx.mesh_connected) {
            s_ctx.mesh_connected_once = true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void uuid_session_task(void *arg)
{
    uuid_request_t request;

    while (true) {
        xQueueReceive(s_ctx.uuid_queue, &request, portMAX_DELAY);
        if (!ss_mesh_is_connected()) {
            ESP_LOGW(TAG, "UUID received but mesh is not connected yet");
            s_ctx.led_state = LED_RED_SOLID;
            vTaskDelay(pdMS_TO_TICKS(1200));
            s_ctx.led_state = LED_OFF;
            continue;
        }
        s_ctx.current_session_id = esp_random();
        s_ctx.led_state = LED_GREEN_FAST;

        ss_mesh_packet_t packet = {
            .magic = SS_PROTOCOL_MAGIC,
            .version = SS_PROTOCOL_VERSION,
            .type = SS_PACKET_UUID_REQUEST,
            .session_id = s_ctx.current_session_id,
            .payload_len = request.len,
        };
        memcpy(packet.node_mac, ss_mesh_get_station_mac(), sizeof(packet.node_mac));
        memcpy(packet.payload, request.value, request.len);

        esp_err_t err = ss_mesh_send_to_root(&packet, sizeof(packet));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "send UUID to root failed: %s", esp_err_to_name(err));
            s_ctx.led_state = LED_RED_SOLID;
            vTaskDelay(pdMS_TO_TICKS(1200));
            s_ctx.led_state = LED_OFF;
            continue;
        }

        ss_mesh_packet_t response;
        if (xQueueReceive(s_ctx.response_queue, &response, pdMS_TO_TICKS(CONFIG_SS_ROOT_RESPONSE_TIMEOUT_MS))) {
            if (response.type == SS_PACKET_COLOR_RESPONSE) {
                led_set_color_from_payload(response.payload, response.payload_len);
            }
        } else {
            ESP_LOGW(TAG, "color response timeout");
            s_ctx.led_state = LED_RED_SOLID;
            vTaskDelay(pdMS_TO_TICKS(1200));
            s_ctx.led_state = LED_OFF;
        }
    }
}

static void session_task(void *arg)
{
    uint8_t event;

    while (true) {
        xQueueReceive(s_ctx.button_queue, &event, portMAX_DELAY);
        if (!ss_mesh_is_connected()) {
            ESP_LOGW(TAG, "button pressed but mesh is not connected yet");
            s_ctx.led_state = LED_RED_SOLID;
            vTaskDelay(pdMS_TO_TICKS(500));
            s_ctx.led_state = LED_OFF;
            continue;
        }
        s_ctx.current_session_id = esp_random();
        s_ctx.led_state = LED_BLUE_FAST;

        ble_target_t target;
        esp_err_t err = scan_nearest_device(&target);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "BLE device not found");
            s_ctx.led_state = LED_BLUE_SLOW;
            vTaskDelay(pdMS_TO_TICKS(1200));
            s_ctx.led_state = LED_OFF;
            continue;
        }

        uint16_t distance_cm = estimate_distance_cm(target.rssi);
        if (distance_cm > CONFIG_SS_BLE_MAX_DISTANCE_CM) {
            ESP_LOGW(TAG, "nearest BLE device too far: addr=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d distance=%u cm",
                     target.addr.val[5], target.addr.val[4], target.addr.val[3], target.addr.val[2],
                     target.addr.val[1], target.addr.val[0], target.rssi, distance_cm);
            s_ctx.led_state = LED_BLUE_SLOW;
            vTaskDelay(pdMS_TO_TICKS(1200));
            s_ctx.led_state = LED_OFF;
            continue;
        }

        s_ctx.led_state = LED_YELLOW_SOLID;
        ss_mesh_packet_t packet = {
            .magic = SS_PROTOCOL_MAGIC,
            .version = SS_PROTOCOL_VERSION,
            .type = SS_PACKET_NODE_REPORT,
            .session_id = s_ctx.current_session_id,
            .rssi = target.rssi,
            .distance_cm = distance_cm,
            .payload_len = target.payload_len,
        };
        memcpy(packet.node_mac, ss_mesh_get_station_mac(), sizeof(packet.node_mac));
        memcpy(packet.bt_addr, target.addr.val, sizeof(packet.bt_addr));
        memcpy(packet.payload, target.payload, target.payload_len);

        s_ctx.led_state = LED_GREEN_FAST;
        err = ss_mesh_send_to_root(&packet, sizeof(packet));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "send report to root failed: %s", esp_err_to_name(err));
            s_ctx.led_state = LED_RED_SOLID;
            vTaskDelay(pdMS_TO_TICKS(1200));
            s_ctx.led_state = LED_OFF;
            continue;
        }

        ss_mesh_packet_t response;
        if (xQueueReceive(s_ctx.response_queue, &response, pdMS_TO_TICKS(CONFIG_SS_ROOT_RESPONSE_TIMEOUT_MS))) {
            ESP_LOGI(TAG, "root response: %.*s", response.payload_len, response.payload);
            s_ctx.led_state = LED_GREEN_SOLID;
        } else {
            ESP_LOGW(TAG, "root response timeout");
            s_ctx.led_state = LED_RED_SOLID;
        }

        vTaskDelay(pdMS_TO_TICKS(1500));
        s_ctx.led_state = LED_OFF;
    }
}


void app_main(void)
{
    s_ctx.button_queue = xQueueCreate(4, sizeof(uint8_t));
    s_ctx.uuid_queue = xQueueCreate(4, sizeof(uuid_request_t));
    s_ctx.response_queue = xQueueCreate(2, sizeof(ss_mesh_packet_t));
    s_ctx.scan_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_ctx.button_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(s_ctx.uuid_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(s_ctx.response_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(s_ctx.scan_lock == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    ESP_ERROR_CHECK(led_init());
    s_ctx.mesh_connected = false;
    s_ctx.mesh_connected_once = false;
    s_ctx.color_rgb[0] = 0;
    s_ctx.color_rgb[1] = 0;
    s_ctx.color_rgb[2] = 0;
    s_ctx.led_state = LED_MESH_INIT;
    xTaskCreate(led_task, "led", 3072, NULL, 4, NULL);
    xTaskCreate(button_task, "button", 2048, NULL, 5, NULL);

    ESP_ERROR_CHECK(ss_mesh_init(mesh_recv_cb, false));
    ESP_ERROR_CHECK(ble_init());
    s_ctx.led_state = LED_GREEN_SOLID;
    vTaskDelay(pdMS_TO_TICKS(700));
    s_ctx.led_state = LED_OFF;

    xTaskCreate(mesh_status_task, "mesh_status", 2048, NULL, 4, NULL);

    xTaskCreate(session_task, "session", 4096, NULL, 5, NULL);
    xTaskCreate(uuid_session_task, "uuid_session", 4096, NULL, 5, NULL);
}
