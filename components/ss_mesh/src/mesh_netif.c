#include "mesh_netif.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MESH_RX_BUFFER_SIZE 1560

static const char *TAG = "mesh_netif";
static TaskHandle_t s_receive_task;
static mesh_raw_recv_cb_t *s_receive_cb;
static uint8_t s_station_mac[6];

static void receive_task(void *arg)
{
    uint8_t rx_buffer[MESH_RX_BUFFER_SIZE];
    mesh_addr_t from;
    mesh_data_t data;
    int flag;

    while (s_receive_task != NULL) {
        data.data = rx_buffer;
        data.size = sizeof(rx_buffer);
        flag = 0;
        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK) {
            if (s_receive_task != NULL) {
                ESP_LOGW(TAG, "mesh receive failed: %s", esp_err_to_name(err));
            }
            continue;
        }
        if (data.proto == MESH_PROTO_BIN && s_receive_cb != NULL) {
            s_receive_cb(&from, &data);
        }
    }

    s_receive_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t mesh_netifs_init(mesh_raw_recv_cb_t *cb)
{
    s_receive_cb = cb;
    return ESP_OK;
}

esp_err_t mesh_netifs_destroy(void)
{
    return mesh_netifs_stop();
}

esp_err_t mesh_netifs_start(bool is_root)
{
    (void)is_root;
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, s_station_mac), TAG, "get station MAC");
    if (s_receive_task == NULL) {
        BaseType_t created = xTaskCreate(receive_task, "mesh_rx", 3072, NULL, 5, &s_receive_task);
        if (created != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t mesh_netifs_stop(void)
{
    TaskHandle_t task = s_receive_task;
    s_receive_task = NULL;
    if (task != NULL) {
        vTaskDelete(task);
    }
    return ESP_OK;
}

esp_err_t mesh_netif_start_root_ap(bool is_root, uint32_t dns_addr)
{
    (void)is_root;
    (void)dns_addr;
    return ESP_OK;
}

uint8_t *mesh_netif_get_station_mac(void)
{
    return s_station_mac;
}
