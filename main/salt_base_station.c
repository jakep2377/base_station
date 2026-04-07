#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_system.h"

#include "esp_http_server.h"
#include "mdns.h"
#include "display.h"

#include "ra01s.h"

#define AP_SSID  "SaltRobot_Base"
#define AP_PASS  "saltrobot123"
#define MDNS_HOSTNAME "base-station"
#define MDNS_INSTANCE "Salt Robot Base Station"
#define DEFAULT_BACKEND_URL "https://robot-lora-server.onrender.com"

#define NVS_NAMESPACE "config"
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_BACKEND_URL "backend_url"

#define MAX_WIFI_SSID_LEN 32
#define MAX_WIFI_PASS_LEN 64
#define MAX_BACKEND_URL_LEN 160

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

#define LORA_CMD_MAX     200
#define LORA_QUEUE_DEPTH 64

static const char *TAG = "BASE";

static EventGroupHandle_t wifi_event_group;
static const int WIFI_GOT_IP_BIT = BIT0;
static SemaphoreHandle_t status_lock;

static char status_json[1024] = "{\"battery\":85,\"state\":\"BOOT\",\"mode\":\"BOOT\"}";
static char last_cmd[192] = "none";
static char last_cmd_id[64] = "";
static char last_cmd_status[32] = "idle";
static char last_lora_rx[256] = "";
static char last_ack_rx[160] = "";
static char current_state[32] = "BOOT";
static char current_mode[16] = "BOOT";
static char wifi_link_state[16] = "connecting";
static char lora_link_state[16] = "idle";
static uint32_t ack_count = 0;

static char provisioned_ssid[MAX_WIFI_SSID_LEN + 1] = "";
static char provisioned_pass[MAX_WIFI_PASS_LEN + 1] = "";
static char provisioned_backend_url[MAX_BACKEND_URL_LEN] = DEFAULT_BACKEND_URL;
static bool wifi_configured = false;
static bool ap_setup_mode = false;

typedef struct {
    uint16_t len;
    char payload[LORA_CMD_MAX];
} lora_cmd_t;

static QueueHandle_t lora_cmd_q;
static bool display_available = false;

static void refresh_status_json(void);

static void capture_ack_if_present(const char *text) {
    if (!text) return;
    const char *ack = strstr(text, "ACK:");
    if (!ack || ack[0] == '\0') return;
    snprintf(last_ack_rx, sizeof(last_ack_rx), "%.150s", ack);
    snprintf(last_cmd_status, sizeof(last_cmd_status), "acknowledged");
    snprintf(lora_link_state, sizeof(lora_link_state), "online");
    ack_count++;
}

static void refresh_status_json(void) {
    snprintf(status_json, sizeof(status_json),
             "{\"status_version\":3,\"battery\":85,\"state\":\"%.32s\",\"mode\":\"%.16s\",\"wifi_link_state\":\"%.16s\",\"lora_link_state\":\"%.16s\",\"last_cmd\":\"%.120s\",\"last_cmd_id\":\"%.60s\",\"last_cmd_status\":\"%.28s\",\"queue_depth\":%d,\"ack_count\":%lu,\"last_ack\":\"%.140s\",\"last_lora\":\"%.200s\",\"configured\":%s,\"backend_url\":\"%.150s\",\"ap_ssid\":\"%s\"}",
             current_state,
             current_mode,
             wifi_link_state,
             lora_link_state,
             last_cmd,
             last_cmd_id,
             last_cmd_status,
             (int)(lora_cmd_q ? uxQueueMessagesWaiting(lora_cmd_q) : 0),
             (unsigned long)ack_count,
             last_ack_rx,
             last_lora_rx,
             wifi_configured ? "true" : "false",
             provisioned_backend_url,
             AP_SSID);
}

static void start_mdns_service(void) {
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set(MDNS_INSTANCE));

    mdns_txt_item_t service_txt[] = {
        {"path", "/status"},
        {"role", "base_station"},
        {"service", "robot-http"},
    };
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, service_txt, sizeof(service_txt) / sizeof(service_txt[0])));
    ESP_LOGI(TAG, "mDNS started: http://%s.local", MDNS_HOSTNAME);
}

static void display_task(void *arg) {
    (void)arg;

    char mode[sizeof(current_mode)] = {0};
    char wifi[sizeof(wifi_link_state)] = {0};
    char lora[sizeof(lora_link_state)] = {0};
    char cmd[20] = {0};
    char ack[20] = {0};
    uint32_t local_ack_count = 0;
    int queue_depth = 0;

    while (1) {
        if (!display_available) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        xSemaphoreTake(status_lock, portMAX_DELAY);
        snprintf(mode, sizeof(mode), "%s", current_mode);
        snprintf(wifi, sizeof(wifi), "%s", wifi_link_state);
        snprintf(lora, sizeof(lora), "%s", lora_link_state);
        snprintf(cmd, sizeof(cmd), "%.18s", last_cmd);
        snprintf(ack, sizeof(ack), "%.18s", last_ack_rx[0] ? last_ack_rx : "-");
        local_ack_count = ack_count;
        queue_depth = (int)(lora_cmd_q ? uxQueueMessagesWaiting(lora_cmd_q) : 0);
        xSemaphoreGive(status_lock);

        Display_ShowStatus(mode, wifi, lora, queue_depth, cmd, ack, local_ack_count);
        vTaskDelay(pdMS_TO_TICKS(750));
    }
}

static bool extract_json_string(const char *json, const char *key, char *out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) return false;
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *found = strstr(json, needle);
    if (!found) return false;
    found = strchr(found + strlen(needle), ':');
    if (!found) return false;
    found++;
    while (*found && isspace((unsigned char)*found)) found++;
    if (*found != '"') return false;
    found++;
    size_t i = 0;
    while (*found && *found != '"' && i + 1 < out_size) {
        if (*found == '\\' && found[1] != '\0') {
            found++;
        }
        out[i++] = *found++;
    }
    out[i] = '\0';
    return i > 0;
}

static esp_err_t read_request_body(httpd_req_t *req, char *buffer, size_t buffer_size) {
    if (!req || !buffer || buffer_size == 0) return ESP_ERR_INVALID_ARG;
    if (req->content_len <= 0 || (size_t)req->content_len >= buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (r <= 0) {
            return ESP_FAIL;
        }
        received += r;
    }
    buffer[received] = '\0';
    return ESP_OK;
}

static void load_provisioned_network_config(void) {
    nvs_handle_t nvs = 0;
    size_t len = 0;

    provisioned_ssid[0] = '\0';
    provisioned_pass[0] = '\0';
    snprintf(provisioned_backend_url, sizeof(provisioned_backend_url), "%s", DEFAULT_BACKEND_URL);
    wifi_configured = false;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    len = sizeof(provisioned_ssid);
    if (nvs_get_str(nvs, NVS_KEY_WIFI_SSID, provisioned_ssid, &len) == ESP_OK && provisioned_ssid[0] != '\0') {
        wifi_configured = true;
    }

    len = sizeof(provisioned_pass);
    (void)nvs_get_str(nvs, NVS_KEY_WIFI_PASS, provisioned_pass, &len);

    len = sizeof(provisioned_backend_url);
    if (nvs_get_str(nvs, NVS_KEY_BACKEND_URL, provisioned_backend_url, &len) != ESP_OK || provisioned_backend_url[0] == '\0') {
        snprintf(provisioned_backend_url, sizeof(provisioned_backend_url), "%s", DEFAULT_BACKEND_URL);
    }

    nvs_close(nvs);
}

static esp_err_t save_provisioned_network_config(const char *ssid, const char *password, const char *backend_url) {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs, NVS_KEY_WIFI_SSID, ssid ? ssid : "");
    if (err == ESP_OK) err = nvs_set_str(nvs, NVS_KEY_WIFI_PASS, password ? password : "");
    if (err == ESP_OK) err = nvs_set_str(nvs, NVS_KEY_BACKEND_URL, (backend_url && backend_url[0]) ? backend_url : DEFAULT_BACKEND_URL);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        snprintf(provisioned_ssid, sizeof(provisioned_ssid), "%s", ssid ? ssid : "");
        snprintf(provisioned_pass, sizeof(provisioned_pass), "%s", password ? password : "");
        snprintf(provisioned_backend_url, sizeof(provisioned_backend_url), "%s", (backend_url && backend_url[0]) ? backend_url : DEFAULT_BACKEND_URL);
        wifi_configured = provisioned_ssid[0] != '\0';
    }

    return err;
}

static void delayed_restart_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    xSemaphoreTake(status_lock, portMAX_DELAY);
    refresh_status_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, status_json, HTTPD_RESP_USE_STRLEN);
    xSemaphoreGive(status_lock);
    return ESP_OK;
}

static esp_err_t last_lora_get_handler(httpd_req_t *req) {
    xSemaphoreTake(status_lock, portMAX_DELAY);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, last_lora_rx, HTTPD_RESP_USE_STRLEN);
    xSemaphoreGive(status_lock);
    return ESP_OK;
}

static esp_err_t setup_status_get_handler(httpd_req_t *req) {
    char body[512];
    xSemaphoreTake(status_lock, portMAX_DELAY);
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"configured\":%s,\"mode\":\"%.16s\",\"state\":\"%.32s\",\"apSsid\":\"%s\",\"savedSsid\":\"%.32s\",\"backendUrl\":\"%.150s\",\"wifiLinkState\":\"%.16s\"}",
             wifi_configured ? "true" : "false",
             current_mode,
             current_state,
             AP_SSID,
             provisioned_ssid,
             provisioned_backend_url,
             wifi_link_state);
    xSemaphoreGive(status_lock);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

static esp_err_t setup_network_post_handler(httpd_req_t *req) {
    char body[512] = {0};
    char ssid[MAX_WIFI_SSID_LEN + 1] = {0};
    char password[MAX_WIFI_PASS_LEN + 1] = {0};
    char backend_url[MAX_BACKEND_URL_LEN] = {0};

    esp_err_t body_err = read_request_body(req, body, sizeof(body));
    if (body_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid setup body");
        return ESP_FAIL;
    }

    if (!extract_json_string(body, "ssid", ssid, sizeof(ssid))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid is required");
        return ESP_FAIL;
    }
    (void)extract_json_string(body, "password", password, sizeof(password));
    (void)extract_json_string(body, "backendUrl", backend_url, sizeof(backend_url));

    esp_err_t save_err = save_provisioned_network_config(ssid, password, backend_url);
    if (save_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save Wi-Fi config");
        return ESP_FAIL;
    }

    xSemaphoreTake(status_lock, portMAX_DELAY);
    snprintf(current_state, sizeof(current_state), "SETUP_SAVED");
    snprintf(current_mode, sizeof(current_mode), "AP");
    snprintf(wifi_link_state, sizeof(wifi_link_state), "restarting");
    refresh_status_json();
    xSemaphoreGive(status_lock);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Wi-Fi saved. Restarting base station.\"}");
    xTaskCreate(delayed_restart_task, "restart", 2048, NULL, 3, NULL);
    return ESP_OK;
}

static esp_err_t command_post_handler(httpd_req_t *req) {
    const int max_body = (int)sizeof(last_cmd) - 1;
    int len = req->content_len;
    char command_id[sizeof(last_cmd_id)] = "";

    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    if (len > max_body) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Body too large");
        return ESP_FAIL;
    }

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
    if (httpd_req_get_hdr_value_str(req, "x-command-id", command_id, sizeof(command_id)) == ESP_OK) {
        snprintf(last_cmd_id, sizeof(last_cmd_id), "%s", command_id);
    } else {
        last_cmd_id[0] = '\0';
    }
    snprintf(last_cmd_status, sizeof(last_cmd_status), "queued");
    snprintf(lora_link_state, sizeof(lora_link_state), "idle");

    ESP_LOGI(TAG, "Received /command: %s", last_cmd);
    snprintf(current_state, sizeof(current_state), "CMD_QUEUED");
    snprintf(current_mode, sizeof(current_mode), "HTTP");

    if (lora_cmd_q) {
        lora_cmd_t c = {0};
        snprintf(c.payload, sizeof(c.payload), "%s", last_cmd);
        c.len = (uint16_t)strnlen(c.payload, sizeof(c.payload));

        if (xQueueSend(lora_cmd_q, &c, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "LoRa cmd queue full; returning 503");
            snprintf(last_cmd_status, sizeof(last_cmd_status), "failed");
            snprintf(lora_link_state, sizeof(lora_link_state), "degraded");
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_sendstr(req, "queue_full");
            return ESP_OK;
        }
    }
    snprintf(last_cmd_status, sizeof(last_cmd_status), "forwarded");

    xSemaphoreTake(status_lock, portMAX_DELAY);
    refresh_status_json();
    xSemaphoreGive(status_lock);

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
        httpd_uri_t last_lora_uri = {
            .uri = "/last_lora",
            .method = HTTP_GET,
            .handler = last_lora_get_handler
        };
        httpd_uri_t setup_status_uri = {
            .uri = "/setup/status",
            .method = HTTP_GET,
            .handler = setup_status_get_handler
        };
        httpd_uri_t setup_network_uri = {
            .uri = "/setup/network",
            .method = HTTP_POST,
            .handler = setup_network_post_handler
        };

        httpd_register_uri_handler(server, &status_uri);
        httpd_register_uri_handler(server, &cmd_uri);
        httpd_register_uri_handler(server, &last_lora_uri);
        httpd_register_uri_handler(server, &setup_status_uri);
        httpd_register_uri_handler(server, &setup_network_uri);

        ESP_LOGI(TAG, "HTTP server ready: /status, /command, /last_lora, /setup/status, /setup/network");
    }
    return server;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        snprintf(wifi_link_state, sizeof(wifi_link_state), "connecting");
        ESP_LOGW(TAG, "STA disconnected, retrying...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_GOT_IP_BIT);
    }
}

static void start_ap_mode(void) {
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid));
    strncpy((char *)ap_cfg.ap.password, AP_PASS, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(AP_SSID);
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = strlen(AP_PASS) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ap_setup_mode = true;
    snprintf(current_state, sizeof(current_state), wifi_configured ? "IDLE" : "SETUP");
    snprintf(current_mode, sizeof(current_mode), "AP");
    snprintf(wifi_link_state, sizeof(wifi_link_state), wifi_configured ? "degraded" : "setup");
    refresh_status_json();
    ESP_LOGI(TAG, "AP started. SSID=%s", AP_SSID);
}

static bool try_sta_mode(const char *ssid, const char *password) {
    esp_netif_create_default_wifi_sta();

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Trying STA connect to %s...", ssid);
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group, WIFI_GOT_IP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(12000));

    if (bits & WIFI_GOT_IP_BIT) {
        ap_setup_mode = false;
        snprintf(current_state, sizeof(current_state), "IDLE");
        snprintf(current_mode, sizeof(current_mode), "STA");
        snprintf(wifi_link_state, sizeof(wifi_link_state), "online");
        refresh_status_json();
        ESP_LOGI(TAG, "STA connected (got IP).");
        return true;
    }

    ESP_LOGW(TAG, "STA failed for %s", ssid);
    ESP_ERROR_CHECK(esp_wifi_stop());
    return false;
}

static void start_sta_then_fallback_ap(void) {
    wifi_event_group = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    load_provisioned_network_config();

    if (wifi_configured && try_sta_mode(provisioned_ssid, provisioned_pass)) {
        return;
    }

    start_ap_mode();
}

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

static void lora_tx_task(void *arg) {
    (void)arg;
    lora_cmd_t c;

    while (1) {
        if (xQueueReceive(lora_cmd_q, &c, portMAX_DELAY) == pdTRUE) {
            if (c.len > 0) {
                ESP_LOGI(TAG, "LoRa TX: %.*s", c.len, c.payload);
                bool sent = false;
                int send_result = 0;
                for (int retry = 0; retry < 3; retry++) {
                    send_result = LoRaSend((uint8_t *)c.payload, c.len, SX126x_TXMODE_SYNC);
                    if (send_result == 0) {
                        sent = true;
                        break;
                    }
                    ESP_LOGW(TAG, "LoRaSend retry %d/3 failed with code %d", retry + 1, send_result);
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                if (!sent) {
                    ESP_LOGE(TAG, "LoRaSend failed after retries with code: %d", send_result);
                    snprintf(last_cmd_status, sizeof(last_cmd_status), "failed");
                    snprintf(lora_link_state, sizeof(lora_link_state), "degraded");
                } else {
                    snprintf(last_cmd_status, sizeof(last_cmd_status), "sent");
                    snprintf(lora_link_state, sizeof(lora_link_state), "online");
                }
            }
        }
    }
}

static void lora_rx_task(void *arg) {
    (void)arg;
    uint8_t rx[255];

    while (1) {
        uint8_t n = LoRaReceive(rx, sizeof(rx) - 1);
        if (n > 0) {
            rx[n] = '\0';

            xSemaphoreTake(status_lock, portMAX_DELAY);
            snprintf(last_lora_rx, sizeof(last_lora_rx), "%s", (char *)rx);
            capture_ack_if_present((char *)rx);
            snprintf(current_state, sizeof(current_state), "IDLE");
            snprintf(current_mode, sizeof(current_mode), "LORA");
            snprintf(lora_link_state, sizeof(lora_link_state), "online");
            refresh_status_json();
            xSemaphoreGive(status_lock);

            ESP_LOGI(TAG, "LoRa RX (%d): %s", n, (char *)rx);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    status_lock = xSemaphoreCreateMutex();

    display_available = Display_Init();
    if (display_available) {
        Display_ShowSplash("BOOTING", "CONNECTING...");
    }

    start_sta_then_fallback_ap();
    start_mdns_service();
    start_http_server();

    lora_cmd_q = xQueueCreate(LORA_QUEUE_DEPTH, sizeof(lora_cmd_t));
    lora_init();
    xTaskCreate(lora_tx_task, "lora_tx", 4096, NULL, 5, NULL);
    xTaskCreate(lora_rx_task, "lora_rx", 4096, NULL, 5, NULL);
    xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);
}
