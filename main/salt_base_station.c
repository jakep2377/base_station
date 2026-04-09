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
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "esp_http_server.h"
#include "mdns.h"
#include "display.h"

#include "ra01s.h"

// ---------------- Wi-Fi Credentials ----------------
#define STA_SSID "JakesiPhone"
#define STA_PASS "password123"

#define AP_SSID  "SaltRobot_Base"
#define AP_PASS  "saltrobot123"
#define MDNS_HOSTNAME "base-station"
#define MDNS_INSTANCE "Salt Robot Base Station"
#define DEFAULT_BACKEND_URL "https://robot-lora-server.onrender.com"

#define NVS_NAMESPACE "config"
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_BACKEND_URL "backend_url"
#define NVS_KEY_BOARD_API_KEY "board_api_key"

#define MAX_WIFI_SSID_LEN 32
#define MAX_WIFI_PASS_LEN 64
#define MAX_BACKEND_URL_LEN 160
#define MAX_BOARD_API_KEY_LEN 128
#define TELEMETRY_POST_INTERVAL_MS 2500
#define BASE_STATUS_POST_INTERVAL_MS 5000
#define REMOTE_COMMAND_POLL_INTERVAL_MS 250

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
static const int WIFI_FAIL_BIT = BIT1;
static int sta_retry_count = 0;
static bool sta_bootstrap_in_progress = false;
#define STA_BOOTSTRAP_MAX_RETRIES 3
static SemaphoreHandle_t status_lock;

// Keep these reasonably sized so they fit in RAM.
static char status_json[1280] = "{\"battery\":85,\"state\":\"IDLE\",\"mode\":\"BOOT\"}";
static char last_cmd[192]     = "none";
static char last_cmd_id[64]   = "";
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
static char provisioned_board_api_key[MAX_BOARD_API_KEY_LEN] = "";
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
    char target_network[MAX_WIFI_SSID_LEN + 1] = {0};
    char backend_url[MAX_BACKEND_URL_LEN] = {0};
    uint32_t local_ack_count = 0;
    int queue_depth = 0;
    uint32_t loop_count = 0;

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
        snprintf(target_network, sizeof(target_network), "%s", provisioned_ssid[0] ? provisioned_ssid : AP_SSID);
        snprintf(backend_url, sizeof(backend_url), "%s", provisioned_backend_url);
        local_ack_count = ack_count;
        queue_depth = (int)(lora_cmd_q ? uxQueueMessagesWaiting(lora_cmd_q) : 0);
        xSemaphoreGive(status_lock);

        if ((loop_count++ % 12U) == 0U) {
            ESP_LOGI(TAG, "OLED tick mode=%s wifi=%s lora=%s net=%s backend=%s q=%d ack=%lu",
                     mode, wifi, lora, target_network, backend_url, queue_depth, (unsigned long)local_ack_count);
        }

        display_show_status(mode, wifi, lora, target_network, backend_url, queue_depth, cmd, ack, local_ack_count);
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

static bool json_flag_is_true(const char *json, const char *key) {
    if (!json || !key) return false;
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *found = strstr(json, needle);
    if (!found) return false;
    found = strchr(found + strlen(needle), ':');
    if (!found) return false;
    found++;
    while (*found && isspace((unsigned char)*found)) found++;
    return strncmp(found, "true", 4) == 0 || *found == '1';
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
    provisioned_board_api_key[0] = '\0';
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

    len = sizeof(provisioned_board_api_key);
    (void)nvs_get_str(nvs, NVS_KEY_BOARD_API_KEY, provisioned_board_api_key, &len);

    nvs_close(nvs);
}

static esp_err_t save_provisioned_network_config(const char *ssid, const char *password, const char *backend_url, const char *board_api_key) {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs, NVS_KEY_WIFI_SSID, ssid ? ssid : "");
    if (err == ESP_OK) err = nvs_set_str(nvs, NVS_KEY_WIFI_PASS, password ? password : "");
    if (err == ESP_OK) err = nvs_set_str(nvs, NVS_KEY_BACKEND_URL, (backend_url && backend_url[0]) ? backend_url : DEFAULT_BACKEND_URL);
    if (err == ESP_OK) err = nvs_set_str(nvs, NVS_KEY_BOARD_API_KEY, board_api_key ? board_api_key : "");
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        snprintf(provisioned_ssid, sizeof(provisioned_ssid), "%s", ssid ? ssid : "");
        snprintf(provisioned_pass, sizeof(provisioned_pass), "%s", password ? password : "");
        snprintf(provisioned_backend_url, sizeof(provisioned_backend_url), "%s", (backend_url && backend_url[0]) ? backend_url : DEFAULT_BACKEND_URL);
        snprintf(provisioned_board_api_key, sizeof(provisioned_board_api_key), "%s", board_api_key ? board_api_key : "");
        wifi_configured = provisioned_ssid[0] != '\0';
    }

    return err;
}

static bool build_backend_endpoint(const char *backend_url, const char *path, char *out, size_t out_size) {
    if (!backend_url || !backend_url[0] || !path || !path[0] || !out || out_size == 0) {
        return false;
    }

    size_t url_len = strlen(backend_url);
    while (url_len > 0 && backend_url[url_len - 1] == '/') {
        url_len--;
    }

    if (url_len == 0 || (url_len + strlen(path) + 1) >= out_size) {
        return false;
    }

    snprintf(out, out_size, "%.*s%s", (int)url_len, backend_url, path);
    return true;
}

static esp_err_t post_json_to_backend(const char *url, const char *json_body) {
    if (!url || !url[0] || !json_body || !json_body[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (provisioned_board_api_key[0] != '\0') {
        esp_http_client_set_header(client, "x-api-key", provisioned_board_api_key);
    }
    esp_http_client_set_post_field(client, json_body, (int)strlen(json_body));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "Backend POST %s returned HTTP %d", url, status_code);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t get_json_from_backend(const char *url, char *response_body, size_t response_size, int *status_code) {
    if (!url || !url[0] || !response_body || response_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    response_body[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    if (provisioned_board_api_key[0] != '\0') {
        esp_http_client_set_header(client, "x-api-key", provisioned_board_api_key);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        int read_len = esp_http_client_read_response(client, response_body, (int)response_size - 1);
        if (read_len < 0) {
            err = ESP_FAIL;
        } else {
            response_body[read_len] = '\0';
        }
    }

    if (status_code) {
        *status_code = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t queue_command_for_lora(const char *command, const char *command_id) {
    if (!command || !command[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(status_lock, portMAX_DELAY);
    snprintf(last_cmd, sizeof(last_cmd), "%s", command);
    if (command_id && command_id[0]) {
        snprintf(last_cmd_id, sizeof(last_cmd_id), "%s", command_id);
    } else {
        last_cmd_id[0] = '\0';
    }
    snprintf(last_cmd_status, sizeof(last_cmd_status), "queued");
    snprintf(lora_link_state, sizeof(lora_link_state), "idle");
    snprintf(current_state, sizeof(current_state), "CMD_QUEUED");
    snprintf(current_mode, sizeof(current_mode), "HTTP");
    refresh_status_json();
    xSemaphoreGive(status_lock);

    if (!lora_cmd_q) {
        return ESP_ERR_INVALID_STATE;
    }

    lora_cmd_t c = {0};
    snprintf(c.payload, sizeof(c.payload), "%s", command);
    c.len = (uint16_t)strnlen(c.payload, sizeof(c.payload));

    if (xQueueSend(lora_cmd_q, &c, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreTake(status_lock, portMAX_DELAY);
        snprintf(last_cmd_status, sizeof(last_cmd_status), "failed");
        snprintf(lora_link_state, sizeof(lora_link_state), "degraded");
        refresh_status_json();
        xSemaphoreGive(status_lock);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(status_lock, portMAX_DELAY);
    snprintf(last_cmd_status, sizeof(last_cmd_status), "forwarded");
    refresh_status_json();
    xSemaphoreGive(status_lock);
    return ESP_OK;
}

static esp_err_t post_remote_command_ack(const char *command_id, const char *status, const char *error_message) {
    if (!command_id || !command_id[0]) {
        return ESP_OK;
    }

    char backend_url[MAX_BACKEND_URL_LEN] = {0};
    char endpoint[MAX_BACKEND_URL_LEN + 64] = {0};
    char body[384] = {0};

    xSemaphoreTake(status_lock, portMAX_DELAY);
    snprintf(backend_url, sizeof(backend_url), "%s", provisioned_backend_url);
    xSemaphoreGive(status_lock);

    if (backend_url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (!build_backend_endpoint(backend_url, "/api/base-station/command-ack", endpoint, sizeof(endpoint))) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (error_message && error_message[0]) {
        snprintf(body, sizeof(body),
                 "{\"commandId\":\"%.60s\",\"status\":\"%.24s\",\"error\":\"%.120s\"}",
                 command_id,
                 status ? status : "forwarded",
                 error_message);
    } else {
        snprintf(body, sizeof(body),
                 "{\"commandId\":\"%.60s\",\"status\":\"%.24s\"}",
                 command_id,
                 status ? status : "forwarded");
    }

    return post_json_to_backend(endpoint, body);
}

static bool fetch_remote_command_from_backend(char *cmd_out, size_t cmd_out_size, char *command_id_out, size_t command_id_size) {
    char backend_url[MAX_BACKEND_URL_LEN] = {0};
    char endpoint[MAX_BACKEND_URL_LEN + 64] = {0};
    char response[512] = {0};
    int status_code = -1;

    if (!cmd_out || cmd_out_size == 0) {
        return false;
    }

    cmd_out[0] = '\0';
    if (command_id_out && command_id_size > 0) {
        command_id_out[0] = '\0';
    }

    xSemaphoreTake(status_lock, portMAX_DELAY);
    snprintf(backend_url, sizeof(backend_url), "%s", provisioned_backend_url);
    xSemaphoreGive(status_lock);

    if (backend_url[0] == '\0') {
        return false;
    }
    if (!build_backend_endpoint(backend_url, "/api/base-station/command", endpoint, sizeof(endpoint))) {
        return false;
    }

    esp_err_t err = get_json_from_backend(endpoint, response, sizeof(response), &status_code);
    if (err != ESP_OK || status_code != 200) {
        return false;
    }
    if (!json_flag_is_true(response, "pending")) {
        return false;
    }
    if (!extract_json_string(response, "cmd", cmd_out, cmd_out_size)) {
        return false;
    }
    if (command_id_out && command_id_size > 0) {
        (void)extract_json_string(response, "commandId", command_id_out, command_id_size);
    }
    return true;
}

static void backend_command_poll_task(void *arg) {
    (void)arg;
    char wifi_state[sizeof(wifi_link_state)] = {0};
    char command[LORA_CMD_MAX] = {0};
    char command_id[sizeof(last_cmd_id)] = {0};
    char last_remote_command_id[sizeof(last_cmd_id)] = {0};

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(REMOTE_COMMAND_POLL_INTERVAL_MS));

        xSemaphoreTake(status_lock, portMAX_DELAY);
        snprintf(wifi_state, sizeof(wifi_state), "%s", wifi_link_state);
        xSemaphoreGive(status_lock);

        if (strcmp(wifi_state, "online") != 0) continue;
        if (!fetch_remote_command_from_backend(command, sizeof(command), command_id, sizeof(command_id))) continue;

        if (command_id[0] != '\0' && strcmp(command_id, last_remote_command_id) == 0) {
            (void)post_remote_command_ack(command_id, "forwarded", NULL);
            continue;
        }

        esp_err_t queue_err = queue_command_for_lora(command, command_id);
        if (queue_err == ESP_OK) {
            ESP_LOGI(TAG, "Remote backend command queued: %s", command);
            if (command_id[0] != '\0') {
                snprintf(last_remote_command_id, sizeof(last_remote_command_id), "%s", command_id);
                (void)post_remote_command_ack(command_id, "forwarded", NULL);
            }
        } else {
            ESP_LOGW(TAG, "Remote backend command queue failed: %s", esp_err_to_name(queue_err));
            if (command_id[0] != '\0') {
                (void)post_remote_command_ack(command_id, "retry", esp_err_to_name(queue_err));
            }
        }
    }
}

static void telemetry_post_task(void *arg) {
    (void)arg;
    char payload[sizeof(last_lora_rx)] = {0};
    char backend_url[MAX_BACKEND_URL_LEN] = {0};
    char endpoint[MAX_BACKEND_URL_LEN + 48] = {0};
    char last_sent[sizeof(last_lora_rx)] = {0};
    char wifi_state[sizeof(wifi_link_state)] = {0};

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_POST_INTERVAL_MS));

        xSemaphoreTake(status_lock, portMAX_DELAY);
        snprintf(payload, sizeof(payload), "%s", last_lora_rx);
        snprintf(backend_url, sizeof(backend_url), "%s", provisioned_backend_url);
        snprintf(wifi_state, sizeof(wifi_state), "%s", wifi_link_state);
        xSemaphoreGive(status_lock);

        if (strcmp(wifi_state, "online") != 0) continue;
        if (backend_url[0] == '\0') continue;
        if (payload[0] != '{') continue;
        if (strcmp(payload, last_sent) == 0) continue;
        if (!build_backend_endpoint(backend_url, "/api/telemetry", endpoint, sizeof(endpoint))) continue;

        esp_err_t err = post_json_to_backend(endpoint, payload);
        if (err == ESP_OK) {
            snprintf(last_sent, sizeof(last_sent), "%s", payload);
            ESP_LOGI(TAG, "Telemetry pushed to backend");
        } else {
            ESP_LOGW(TAG, "Telemetry push failed: %s", esp_err_to_name(err));
        }
    }
}

static void base_status_post_task(void *arg) {
    (void)arg;
    char payload[sizeof(status_json)] = {0};
    char backend_url[MAX_BACKEND_URL_LEN] = {0};
    char endpoint[MAX_BACKEND_URL_LEN + 64] = {0};
    char wifi_state[sizeof(wifi_link_state)] = {0};

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BASE_STATUS_POST_INTERVAL_MS));

        xSemaphoreTake(status_lock, portMAX_DELAY);
        refresh_status_json();
        snprintf(payload, sizeof(payload), "%s", status_json);
        snprintf(backend_url, sizeof(backend_url), "%s", provisioned_backend_url);
        snprintf(wifi_state, sizeof(wifi_state), "%s", wifi_link_state);
        xSemaphoreGive(status_lock);

        if (strcmp(wifi_state, "online") != 0) continue;
        if (backend_url[0] == '\0') continue;
        if (!build_backend_endpoint(backend_url, "/api/base-station/status", endpoint, sizeof(endpoint))) continue;

        esp_err_t err = post_json_to_backend(endpoint, payload);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Base status pushed to backend");
        } else {
            ESP_LOGW(TAG, "Base status push failed: %s", esp_err_to_name(err));
        }
    }
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
             "{\"ok\":true,\"configured\":%s,\"mode\":\"%.16s\",\"state\":\"%.32s\",\"apSsid\":\"%s\",\"savedSsid\":\"%.32s\",\"backendUrl\":\"%.150s\",\"wifiLinkState\":\"%.16s\",\"boardApiKeySet\":%s}",
             wifi_configured ? "true" : "false",
             current_mode,
             current_state,
             AP_SSID,
             provisioned_ssid,
             provisioned_backend_url,
             wifi_link_state,
             provisioned_board_api_key[0] ? "true" : "false");
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
    char board_api_key[MAX_BOARD_API_KEY_LEN] = {0};

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
    (void)extract_json_string(body, "boardApiKey", board_api_key, sizeof(board_api_key));

    esp_err_t save_err = save_provisioned_network_config(ssid, password, backend_url, board_api_key);
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
    if (httpd_req_get_hdr_value_str(req, "x-command-id", command_id, sizeof(command_id)) != ESP_OK) {
        command_id[0] = '\0';
    }

    ESP_LOGI(TAG, "Received /command: %s", last_cmd);

    esp_err_t queue_err = queue_command_for_lora(last_cmd, command_id);
    if (queue_err != ESP_OK) {
        ESP_LOGW(TAG, "LoRa cmd queue full; returning 503");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "queue_full");
        return ESP_OK;
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

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        sta_retry_count = 0;
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)data;
        uint8_t reason = disc ? disc->reason : 0;
        snprintf(wifi_link_state, sizeof(wifi_link_state), "connecting");
        if (sta_bootstrap_in_progress) {
            sta_retry_count++;
            ESP_LOGW(TAG, "STA disconnected during bootstrap (%d/%d), reason=%u",
                     sta_retry_count, STA_BOOTSTRAP_MAX_RETRIES, reason);
            if (sta_retry_count >= STA_BOOTSTRAP_MAX_RETRIES) {
                xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
                return;
            }
        } else {
            ESP_LOGW(TAG, "STA disconnected, retrying (reason=%u)...", reason);
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        sta_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_GOT_IP_BIT);
    }
}

static void configure_ap_settings(void) {
    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid));
    strncpy((char *)ap_cfg.ap.password, AP_PASS, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(AP_SSID);
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = strlen(AP_PASS) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
}

static void start_ap_mode(void) {
    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    configure_ap_settings();
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
    esp_netif_create_default_wifi_ap();

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));

    xEventGroupClearBits(wifi_event_group, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT);
    sta_retry_count = 0;
    sta_bootstrap_in_progress = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    configure_ap_settings();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Trying STA connect to %s while keeping setup AP online...", ssid);
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    sta_bootstrap_in_progress = false;
    ap_setup_mode = true;

    if (bits & WIFI_GOT_IP_BIT) {
        ESP_LOGI(TAG, "STA connected (got IP). Disabling setup AP.");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ap_setup_mode = false;
        snprintf(current_state, sizeof(current_state), "IDLE");
        snprintf(current_mode, sizeof(current_mode), "STA");
        snprintf(wifi_link_state, sizeof(wifi_link_state), "online");
        refresh_status_json();
        return true;
    }

    ESP_LOGW(TAG, "STA failed for %s; keeping setup AP available", ssid);
    snprintf(current_state, sizeof(current_state), "SETUP");
    snprintf(current_mode, sizeof(current_mode), "AP");
    snprintf(wifi_link_state, sizeof(wifi_link_state), "setup");
    refresh_status_json();
    return false;
}

static void start_sta_then_fallback_ap(void) {
    wifi_event_group = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    load_provisioned_network_config();

    if (wifi_configured) {
        if (try_sta_mode(provisioned_ssid, provisioned_pass)) {
            return;
        }
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

    display_available = display_init();
    if (display_available) {
        display_show_splash("BOOTING", "CONNECTING...");
    }

    start_sta_then_fallback_ap();
    start_mdns_service();
    start_http_server();

    lora_cmd_q = xQueueCreate(LORA_QUEUE_DEPTH, sizeof(lora_cmd_t));
    lora_init();
    xTaskCreate(lora_tx_task, "lora_tx", 4096, NULL, 5, NULL);
    xTaskCreate(lora_rx_task, "lora_rx", 4096, NULL, 5, NULL);
    xTaskCreate(telemetry_post_task, "telemetry_post", 6144, NULL, 4, NULL);
    xTaskCreate(base_status_post_task, "base_status_post", 6144, NULL, 4, NULL);
    xTaskCreate(backend_command_poll_task, "backend_poll", 6144, NULL, 4, NULL);

    if (display_available) {
        xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);
    }
}


