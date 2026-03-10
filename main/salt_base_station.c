#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "esp_http_server.h"

// LoRa component
#include "ra01s.h"

// ---------------- Wi-Fi Credentials ----------------
#define STA_SSID "42Fortress"
#define STA_PASS "sweetdoll684"

#define AP_SSID  "SaltRobot_Base"
#define AP_PASS  "saltrobot123"

// ---------------- LoRa Settings ----------------
#define LORA_FREQ_HZ     915000000
#define LORA_TX_POWER_DBM 22
#define LORA_TCXO_VOLT   3.3f
#define LORA_USE_LDO     true

#define LORA_SF          7
#define LORA_BW          4   
#define LORA_CR          1  
#define LORA_PREAMBLE    8
#define LORA_PAYLOAD_LEN 0  
#define LORA_CRC_ON      true
#define LORA_INVERT_IRQ  false

// Limit command size over LoRa
#define LORA_CMD_MAX     200

static const char *TAG = "BASE";

// ---------------- Wi-Fi Event Group ----------------
static EventGroupHandle_t wifi_event_group;
static const int WIFI_GOT_IP_BIT = BIT0;

// ---------------- Shared State ----------------
static SemaphoreHandle_t status_lock;

// Keep these reasonably sized so they fit in RAM.
static char status_json[256] = "{\"battery\":85,\"state\":\"IDLE\",\"mode\":\"BOOT\"}";
static char last_cmd[192]    = "none";

// ---------------- LoRa Queue ----------------
typedef struct {
    uint16_t len;
    char payload[LORA_CMD_MAX];
} lora_cmd_t;

static QueueHandle_t lora_cmd_q;

// ---------------- HTTP Handlers ----------------

static esp_err_t status_get_handler(httpd_req_t *req) {
    xSemaphoreTake(status_lock, portMAX_DELAY);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, status_json, HTTPD_RESP_USE_STRLEN);
    xSemaphoreGive(status_lock);
    return ESP_OK;
}

static esp_err_t command_post_handler(httpd_req_t *req) {
    const int max_body = (int)sizeof(last_cmd) - 1;
    int len = req->content_len;

    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    if (len > max_body) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Body too large");
        return ESP_FAIL;
    }

    // Read the body into last_cmd
    int received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, last_cmd + received, len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Body recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    last_cmd[received] = '\0';

    ESP_LOGI(TAG, "Received /command: %s", last_cmd);

    // Update JSON status safely
    xSemaphoreTake(status_lock, portMAX_DELAY);
    snprintf(status_json, sizeof(status_json),
             "{\"battery\":85,\"state\":\"CMD_RCVD\",\"mode\":\"HTTP\",\"last_cmd\":\"%.120s\"}",
             last_cmd);
    xSemaphoreGive(status_lock);

    // Queue command for LoRa TX
    if (lora_cmd_q) {
        lora_cmd_t c = {0};
        // Cap payload for LoRa
        snprintf(c.payload, sizeof(c.payload), "%s", last_cmd);
        c.len = (uint16_t)strnlen(c.payload, sizeof(c.payload));

        // If queue is full, drop
        if (xQueueSend(lora_cmd_q, &c, 0) != pdTRUE) {
            ESP_LOGW(TAG, "LoRa cmd queue full; dropping command");
        }
    }

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static httpd_handle_t start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t status_uri = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = status_get_handler
        };
        httpd_uri_t cmd_uri = {
            .uri = "/command",
            .method = HTTP_POST,
            .handler = command_post_handler
        };

        httpd_register_uri_handler(server, &status_uri);
        httpd_register_uri_handler(server, &cmd_uri);

        ESP_LOGI(TAG, "HTTP server: GET /status, POST /command");
    }
    return server;
}

// ---------------- Wi-Fi Events ----------------

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA disconnected, retrying...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_GOT_IP_BIT);
    }
}

// ---------------- STA then AP fallback ----------------

static void start_sta_then_fallback_ap(void) {
    wifi_event_group = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Try STA first
    esp_netif_create_default_wifi_sta();

    wifi_config_t sta_cfg = {0};
    strncpy((char*)sta_cfg.sta.ssid, STA_SSID, sizeof(sta_cfg.sta.ssid));
    strncpy((char*)sta_cfg.sta.password, STA_PASS, sizeof(sta_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Trying STA connect...");
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group, WIFI_GOT_IP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(12000));

    if (bits & WIFI_GOT_IP_BIT) {
        ESP_LOGI(TAG, "STA connected (got IP).");
        xSemaphoreTake(status_lock, portMAX_DELAY);
        snprintf(status_json, sizeof(status_json),
                 "{\"battery\":85,\"state\":\"IDLE\",\"mode\":\"STA\"}");
        xSemaphoreGive(status_lock);
        return;
    }

    // Fallback AP mode
    ESP_LOGW(TAG, "STA failed. Starting AP mode...");
    ESP_ERROR_CHECK(esp_wifi_stop());

    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {0};
    strncpy((char*)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid));
    strncpy((char*)ap_cfg.ap.password, AP_PASS, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(AP_SSID);
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    if (strlen(AP_PASS) == 0) ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    xSemaphoreTake(status_lock, portMAX_DELAY);
    snprintf(status_json, sizeof(status_json),
             "{\"battery\":85,\"state\":\"IDLE\",\"mode\":\"AP\"}");
    xSemaphoreGive(status_lock);

    ESP_LOGI(TAG, "AP started. SSID=%s (AP IP is usually 192.168.4.1)", AP_SSID);
}

// ---------------- LoRa ----------------

static void lora_init(void) {
    ESP_LOGI(TAG, "Initializing LoRa...");
    LoRaInit();

    int rc = LoRaBegin(LORA_FREQ_HZ, LORA_TX_POWER_DBM, LORA_TCXO_VOLT, LORA_USE_LDO);
    if (rc != 0) {
        ESP_LOGE(TAG, "LoRaBegin failed (%d). Check pins/TCXO/freq.", rc);
        return;
    }

    LoRaConfig(LORA_SF, LORA_BW, LORA_CR,
               LORA_PREAMBLE, LORA_PAYLOAD_LEN, LORA_CRC_ON, LORA_INVERT_IRQ);

    ESP_LOGI(TAG, "LoRa ready @ %d Hz", (int)LORA_FREQ_HZ);
}

// TX task: send queued commands over LoRa
static void lora_tx_task(void *arg) {
    (void)arg;
    lora_cmd_t c;

    while (1) {
        if (xQueueReceive(lora_cmd_q, &c, portMAX_DELAY) == pdTRUE) {
            if (c.len > 0) {
                ESP_LOGI(TAG, "LoRa TX: %.*s", c.len, c.payload);
                int send_result = LoRaSend((uint8_t*)c.payload, c.len, SX126x_TXMODE_SYNC);
                if (send_result != 0) {
                    ESP_LOGE(TAG, "LoRaSend failed with code: %d", send_result);
                    // Optionally retry or handle error (e.g., reset LoRa)
                }
            }
        }
    }
}

// RX task: receive messages over LoRa and store into status_json
static void lora_rx_task(void *arg) {
    (void)arg;
    uint8_t rx[255];

    while (1) {
        uint8_t n = LoRaReceive(rx, sizeof(rx));
        if (n > 0) {
            // Save last received LoRa text into /status
            xSemaphoreTake(status_lock, portMAX_DELAY);
            snprintf(status_json, sizeof(status_json),
                     "{\"battery\":85,\"state\":\"IDLE\",\"mode\":\"LORA\",\"last_lora\":\"%.*s\"}",
                     (n > 120 ? 120 : n), (char*)rx);
            xSemaphoreGive(status_lock);

            ESP_LOGI(TAG, "LoRa RX (%d): %.*s", n, n, (char*)rx);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ---------------- app_main ----------------

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    status_lock = xSemaphoreCreateMutex();

    start_sta_then_fallback_ap();
    start_http_server();

    // LoRa
    lora_cmd_q = xQueueCreate(8, sizeof(lora_cmd_t));
    lora_init();
    xTaskCreate(lora_tx_task, "lora_tx", 4096, NULL, 5, NULL);
    xTaskCreate(lora_rx_task, "lora_rx", 4096, NULL, 5, NULL);
}
// test: curl -X POST http://192.168.4.1/command -H "Content-Type: text/plain" -d "forward"