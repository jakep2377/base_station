#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>

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
#include "esp_sntp.h"

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

static const char ONRENDER_CA_CHAIN_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICjjCCAjOgAwIBAgIQf/NXaJvCTjAtkOGKQb0OHzAKBggqhkjOPQQDAjBQMSQw\n"
    "IgYDVQQLExtHbG9iYWxTaWduIEVDQyBSb290IENBIC0gUjQxEzARBgNVBAoTCkds\n"
    "b2JhbFNpZ24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMjMxMjEzMDkwMDAwWhcN\n"
    "MjkwMjIwMTQwMDAwWjA7MQswCQYDVQQGEwJVUzEeMBwGA1UEChMVR29vZ2xlIFRy\n"
    "dXN0IFNlcnZpY2VzMQwwCgYDVQQDEwNXRTEwWTATBgcqhkjOPQIBBggqhkjOPQMB\n"
    "BwNCAARvzTr+Z1dHTCEDhUDCR127WEcPQMFcF4XGGTfn1XzthkubgdnXGhOlCgP4\n"
    "mMTG6J7/EFmPLCaY9eYmJbsPAvpWo4IBAjCB/zAOBgNVHQ8BAf8EBAMCAYYwHQYD\n"
    "VR0lBBYwFAYIKwYBBQUHAwEGCCsGAQUFBwMCMBIGA1UdEwEB/wQIMAYBAf8CAQAw\n"
    "HQYDVR0OBBYEFJB3kjVnxP+ozKnme9mAeXvMk/k4MB8GA1UdIwQYMBaAFFSwe61F\n"
    "uOJAf/sKbvu+M8k8o4TVMDYGCCsGAQUFBwEBBCowKDAmBggrBgEFBQcwAoYaaHR0\n"
    "cDovL2kucGtpLmdvb2cvZ3NyNC5jcnQwLQYDVR0fBCYwJDAioCCgHoYcaHR0cDov\n"
    "L2MucGtpLmdvb2cvci9nc3I0LmNybDATBgNVHSAEDDAKMAgGBmeBDAECATAKBggq\n"
    "hkjOPQQDAgNJADBGAiEAokJL0LgR6SOLR02WWxccAq3ndXp4EMRveXMUVUxMWSMC\n"
    "IQDspFWa3fj7nLgouSdkcPy1SdOR2AGm9OQWs7veyXsBwA==\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIIB3DCCAYOgAwIBAgINAgPlfvU/k/2lCSGypjAKBggqhkjOPQQDAjBQMSQwIgYD\n"
    "VQQLExtHbG9iYWxTaWduIEVDQyBSb290IENBIC0gUjQxEzARBgNVBAoTCkdsb2Jh\n"
    "bFNpZ24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMTIxMTEzMDAwMDAwWhcNMzgw\n"
    "MTE5MDMxNDA3WjBQMSQwIgYDVQQLExtHbG9iYWxTaWduIEVDQyBSb290IENBIC0g\n"
    "UjQxEzARBgNVBAoTCkdsb2JhbFNpZ24xEzARBgNVBAMTCkdsb2JhbFNpZ24wWTAT\n"
    "BgcqhkjOPQIBBggqhkjOPQMBBwNCAAS4xnnTj2wlDp8uORkcA6SumuU5BwkWymOx\n"
    "uYb4ilfBV85C+nOh92VC/x7BALJucw7/xyHlGKSq2XE/qNS5zowdo0IwQDAOBgNV\n"
    "HQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUVLB7rUW44kB/\n"
    "+wpu+74zyTyjhNUwCgYIKoZIzj0EAwIDRwAwRAIgIk90crlgr/HmnKAWBVBfw147\n"
    "bmF0774BxL4YSFlhgjICICadVGNA3jdgUM/I2O2dgq43mLyjj0xMqTQrbO/7lZsm\n"
    "-----END CERTIFICATE-----\n";

#define NVS_NAMESPACE "config"
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_BACKEND_URL "backend_url"
#define NVS_KEY_BOARD_API_KEY "board_api_key"

#define MAX_WIFI_SSID_LEN 32
#define MAX_WIFI_PASS_LEN 64
#define MAX_BACKEND_URL_LEN 160
#define MAX_BOARD_API_KEY_LEN 128
#define TELEMETRY_POST_INTERVAL_MS 650
#define BASE_STATUS_POST_INTERVAL_MS 1800
#define REMOTE_COMMAND_POLL_INTERVAL_MS 100
#define REMOTE_COMMAND_DUPLICATE_SUPPRESS_MS 120
#define LOCAL_MOTION_DUPLICATE_SUPPRESS_MS 45
#define BASE_MOTION_LOG_THROTTLE_MS 1000
#define BACKEND_HTTP_TASK_STACK_SIZE 16384
#define BACKEND_HTTP_TIMEOUT_MS 900
#define LORA_TASK_STACK_SIZE 6144
#define DISPLAY_TASK_STACK_SIZE 6144
#define RESTART_TASK_STACK_SIZE 3072

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
#define GFSK_BITRATE_BPS 100000U
#define GFSK_FREQ_DEV_HZ 50000U
#define GFSK_RX_BW       SX126X_GFSK_RX_BW_156_2
#define GFSK_PREAMBLE_BITS 32U
#define GFSK_SYNC_WORD_BITS 32U
#define GFSK_FIXED_PAYLOAD_LEN 64U

#define LORA_CMD_MAX     384
#define LORA_QUEUE_DEPTH 16
#define LORA_RX_REASSEMBLY_MAX 1024
#define BASE_AUTO_RADIO_MODE_SWITCH 1

static const char *TAG = "BASE";

static EventGroupHandle_t wifi_event_group;
static const int WIFI_GOT_IP_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;
static int sta_retry_count = 0;
static bool sta_bootstrap_in_progress = false;
#define STA_BOOTSTRAP_MAX_RETRIES 3
static SemaphoreHandle_t status_lock;
static SemaphoreHandle_t backend_http_lock;
static SemaphoreHandle_t lora_lock;
static int s_last_backend_http_status = -1;

// Leave headroom for the escaped LoRa payload and backend URL in the status JSON.
static char status_json[4096] = "{\"battery\":85,\"state\":\"IDLE\",\"mode\":\"BOOT\"}";
static char last_cmd[192]     = "none";
static char last_cmd_id[64]   = "";
static char last_cmd_status[32] = "idle";
static char last_lora_rx[LORA_RX_REASSEMBLY_MAX] = "";
static char last_lora_json[LORA_RX_REASSEMBLY_MAX] = "";
static char last_ack_rx[160] = "";
static char current_state[32] = "BOOT";
static char current_mode[16] = "BOOT";
static char wifi_link_state[16] = "connecting";
static char lora_link_state[16] = "idle";
static uint32_t ack_count = 0;
static uint32_t last_lora_rx_count = 0;
static uint32_t last_lora_json_count = 0;
static bool lora_stream_seq_init = false;
static uint32_t lora_stream_expected_seq = 0;
static char lora_stream_buf[LORA_RX_REASSEMBLY_MAX] = "";
static size_t lora_stream_len = 0;
static uint32_t lora_rx_chain_id = 0;

static char provisioned_ssid[MAX_WIFI_SSID_LEN + 1] = "";
static char provisioned_pass[MAX_WIFI_PASS_LEN + 1] = "";
static char provisioned_backend_url[MAX_BACKEND_URL_LEN] = DEFAULT_BACKEND_URL;
static char provisioned_board_api_key[MAX_BOARD_API_KEY_LEN] = "";
static bool wifi_configured = false;
static bool ap_setup_mode = false;
static bool sta_netif_created = false;
static bool ap_netif_created = false;

typedef enum {
    RADIO_MODE_LORA = 0,
    RADIO_MODE_GFSK = 1,
} radio_mode_t;

typedef struct {
    uint16_t len;
    char payload[LORA_CMD_MAX];
} lora_cmd_t;

static QueueHandle_t lora_cmd_q;
static bool display_available = false;
static volatile radio_mode_t s_radio_mode = RADIO_MODE_LORA;

static const char RADIO_SWITCH_TO_GFSK_FRAME[] = "RADIO:GFSK";
static const char RADIO_SWITCH_TO_LORA_FRAME[] = "RADIO:LORA";

static bool is_motion_command(const char *command) {
    if (!command || command[0] == '\0') {
        return false;
    }

    if (strncmp(command, "D:", 2) == 0) {
        return true;
    }

    if (strncmp(command, "J:", 2) == 0) {
        return true;
    }

    if ((command[1] == '\0') &&
        (command[0] == 'F' || command[0] == 'B' || command[0] == 'L' || command[0] == 'R' || command[0] == 'S')) {
        return true;
    }

    return strncmp(command, "DRIVE,", 6) == 0 ||
           strcmp(command, "STOP") == 0 ||
           strcmp(command, "FORWARD") == 0 ||
           strcmp(command, "BACKWARD") == 0 ||
           strcmp(command, "LEFT") == 0 ||
           strcmp(command, "RIGHT") == 0;
}

static bool command_has_drive_sequence(const char *command) {
    return command && (strstr(command, ",S:") != NULL || strncmp(command, "J:", 2) == 0);
}

static bool is_mode_command(const char *command) {
    if (!command || command[0] == '\0') {
        return false;
    }

    if ((command[1] == '\0') &&
        (command[0] == 'A' || command[0] == 'M' || command[0] == 'P' || command[0] == 'E' || command[0] == 'X')) {
        return true;
    }

    return strcmp(command, "MANUAL") == 0 ||
           strcmp(command, "AUTO") == 0 ||
           strcmp(command, "PAUSE") == 0 ||
           strcmp(command, "ESTOP") == 0 ||
           strcmp(command, "RESET") == 0 ||
           strcmp(command, "CMD:M") == 0 ||
           strcmp(command, "CMD:A") == 0 ||
           strcmp(command, "CMD:P") == 0 ||
           strcmp(command, "CMD:E") == 0 ||
           strcmp(command, "CMD:X") == 0 ||
           strcmp(command, "CMD:MANUAL") == 0 ||
           strcmp(command, "CMD:AUTO") == 0 ||
           strcmp(command, "CMD:PAUSE") == 0 ||
           strcmp(command, "CMD:ESTOP") == 0 ||
           strcmp(command, "CMD:RESET") == 0;
}

static bool is_manual_entry_command(const char *command) {
    if (!command) {
        return false;
    }
    return strcmp(command, "MANUAL") == 0 ||
           strcmp(command, "M") == 0 ||
           strcmp(command, "CMD:M") == 0 ||
           strcmp(command, "CMD:MANUAL") == 0;
}

static bool is_lora_restore_command(const char *command) {
    if (!command) {
        return false;
    }
    return strcmp(command, "AUTO") == 0 ||
           strcmp(command, "A") == 0 ||
           strcmp(command, "PAUSE") == 0 ||
           strcmp(command, "P") == 0 ||
           strcmp(command, "ESTOP") == 0 ||
           strcmp(command, "E") == 0 ||
           strcmp(command, "RESET") == 0 ||
           strcmp(command, "X") == 0 ||
           strcmp(command, "CMD:A") == 0 ||
           strcmp(command, "CMD:AUTO") == 0 ||
           strcmp(command, "CMD:P") == 0 ||
           strcmp(command, "CMD:PAUSE") == 0 ||
           strcmp(command, "CMD:E") == 0 ||
           strcmp(command, "CMD:ESTOP") == 0 ||
           strcmp(command, "CMD:X") == 0 ||
           strcmp(command, "CMD:RESET") == 0;
}

static const char *radio_mode_name(radio_mode_t mode) {
    return mode == RADIO_MODE_GFSK ? "GFSK" : "LORA";
}

static bool radio_apply_mode_locked(radio_mode_t mode) {
    if (mode == RADIO_MODE_LORA) {
        LoRaConfig(LORA_SF, LORA_BW, LORA_CR,
                   LORA_PREAMBLE, LORA_PAYLOAD_LEN, LORA_CRC_ON, LORA_INVERT_IRQ);
    } else {
        GFSKConfig(GFSK_BITRATE_BPS, GFSK_FREQ_DEV_HZ, GFSK_RX_BW,
                   GFSK_PREAMBLE_BITS, GFSK_SYNC_WORD_BITS,
                   GFSK_FIXED_PAYLOAD_LEN, false);
    }
    s_radio_mode = mode;
    ESP_LOGI(TAG, "Radio modem active: %s", radio_mode_name(mode));
    return true;
}

static bool radio_apply_mode(radio_mode_t mode) {
    bool ok = true;
    if (lora_lock) {
        xSemaphoreTake(lora_lock, portMAX_DELAY);
    }
    ok = radio_apply_mode_locked(mode);
    if (lora_lock) {
        xSemaphoreGive(lora_lock);
    }
    return ok;
}

static bool lora_send_payload_with_retries_locked(const char *payload, int burst_count) {
    size_t payload_len = strnlen(payload, LORA_CMD_MAX);
    bool sent = false;

    if (payload_len == 0) {
        return false;
    }

    for (int burst = 0; burst < burst_count; burst++) {
        bool burst_ok = false;
        for (int retry = 0; retry < 3; retry++) {
            burst_ok = LoRaSend((uint8_t *)payload, (uint8_t)payload_len, SX126x_TXMODE_SYNC);
            if (burst_ok) {
                sent = true;
                break;
            }
            ESP_LOGW(TAG, "LoRaSend retry %d/3 failed for %s", retry + 1, payload);
            vTaskDelay(pdMS_TO_TICKS(35));
        }
        if (!burst_ok) {
            return false;
        }
        if (burst + 1 < burst_count) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    return sent;
}

static void refresh_status_json(void);

static bool system_time_is_valid(void) {
    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    return timeinfo.tm_year >= (2024 - 1900);
}

static void sync_time_with_sntp(void) {
    if (system_time_is_valid()) {
        return;
    }

    ESP_LOGI(TAG, "System time not set; syncing with SNTP...");
    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "time.google.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();

    for (int attempt = 0; attempt < 30; ++attempt) {
        if (system_time_is_valid()) {
            time_t now = 0;
            struct tm timeinfo = {0};
            char time_buf[32] = {0};
            time(&now);
            gmtime_r(&now, &timeinfo);
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
            ESP_LOGI(TAG, "Time synchronized via SNTP: %s UTC", time_buf);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGW(TAG, "SNTP time sync timed out; TLS may still fail");
}

static const char *backend_cert_pem_for_url(const char *url) {
    if (!url || url[0] == '\0') {
        return NULL;
    }

    if (strstr(url, ".onrender.com") || strstr(url, "onrender.com")) {
        return ONRENDER_CA_CHAIN_PEM;
    }

    return NULL;
}

typedef struct {
    bool is_stream;
    bool has_end;
    uint32_t seq;
    const char *payload;
} lora_stream_frame_t;

static lora_stream_frame_t parse_lora_stream_frame(char *text) {
    lora_stream_frame_t out = {
        .is_stream = false,
        .has_end = true,
        .seq = 0,
        .payload = text,
    };

    if (!text || strncmp(text, "S:", 2) != 0) {
        return out;
    }

    char *p = text + 2;
    char *endptr = NULL;
    unsigned long seq = strtoul(p, &endptr, 10);
    if (endptr == p || !endptr || *endptr != ':') {
        return out;
    }

    out.is_stream = true;
    out.seq = (uint32_t)seq;

    char *payload = endptr + 1;
    if ((payload[0] == 'M' || payload[0] == 'E') && payload[1] == ':') {
        out.has_end = (payload[0] == 'E');
        out.payload = payload + 2;
    } else {
        out.has_end = true;
        out.payload = payload;
    }

    return out;
}

static bool looks_like_supported_telemetry_frame(const char *payload) {
    if (!payload) {
        return false;
    }

    while (*payload == ' ' || *payload == '\t' || *payload == '\r' || *payload == '\n') {
        payload++;
    }

    if ((payload[0] == 'S' || payload[0] == 'T' || payload[0] == 'M' || payload[0] == 'F') && payload[1] == ':') {
        return true;
    }

    if (*payload != '{') {
        return false;
    }

    if (strstr(payload, "\"fault\"") != NULL) {
        return true;
    }
    if (strstr(payload, "\"s\":\"MANUAL\"") != NULL) {
        return true;
    }
    if (strstr(payload, "\"robot\"") != NULL && (strstr(payload, "\"lat\"") != NULL || strstr(payload, "\"latitude\"") != NULL)) {
        return true;
    }
    if (strstr(payload, "\"gps\"") != NULL && (strstr(payload, "\"lat\"") != NULL || strstr(payload, "\"latitude\"") != NULL)) {
        return true;
    }
    if (strstr(payload, "\"state\"") != NULL &&
        (strstr(payload, "\"motor\"") != NULL || strstr(payload, "\"heading\"") != NULL ||
         strstr(payload, "\"disp\"") != NULL || strstr(payload, "\"prox\"") != NULL)) {
        return true;
    }

    return false;
}

static bool looks_like_complete_json_document(const char *payload) {
    if (!payload) {
        return false;
    }

    while (*payload == ' ' || *payload == '\t' || *payload == '\r' || *payload == '\n') {
        payload++;
    }
    size_t len = strlen(payload);
    while (len > 0) {
        char c = payload[len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            len--;
        } else {
            break;
        }
    }
    if (len < 2) {
        return false;
    }
    if (!((payload[0] == '{' && payload[len - 1] == '}') || (payload[0] == '[' && payload[len - 1] == ']'))) {
        return false;
    }

    int brace_depth = 0;
    int bracket_depth = 0;
    bool in_string = false;
    bool escape = false;
    for (size_t i = 0; i < len; i++) {
        char c = payload[i];
        if (in_string) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '{') brace_depth++;
        else if (c == '}') brace_depth--;
        else if (c == '[') bracket_depth++;
        else if (c == ']') bracket_depth--;
        if (brace_depth < 0 || bracket_depth < 0) {
            return false;
        }
    }

    return !in_string && brace_depth == 0 && bracket_depth == 0;
}

static bool looks_like_complete_supported_telemetry_frame(const char *payload) {
    if (!payload) {
        return false;
    }

    while (*payload == ' ' || *payload == '\t' || *payload == '\r' || *payload == '\n') {
        payload++;
    }

    if ((payload[0] == 'S' || payload[0] == 'T' || payload[0] == 'M' || payload[0] == 'F') && payload[1] == ':') {
        size_t len = strlen(payload);
        while (len > 0) {
            char c = payload[len - 1];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                len--;
                continue;
            }
            return c != ',';
        }
        return false;
    }

    return looks_like_complete_json_document(payload);
}

static bool is_gateway_sideband_message(const char *payload) {
    if (!payload) {
        return false;
    }

    while (*payload == ' ' || *payload == '\t' || *payload == '\r' || *payload == '\n') {
        payload++;
    }

    return strncmp(payload, "GW:", 3) == 0 || strncmp(payload, "ACK:", 4) == 0;
}

static const char *skip_ascii_ws(const char *text) {
    if (!text) {
        return "";
    }

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }

    return text;
}

static void log_lora_rx_chain_event(const char *event, uint32_t chain_id, const char *text, size_t total_len) {
    ESP_LOGI(TAG, "LoRa chain %lu %s total=%u text=%.180s",
             (unsigned long)chain_id,
             event ? event : "event",
             (unsigned)total_len,
             text ? text : "");
}

static bool store_lora_rx_payload(char *text) {
    if (!text) {
        return false;
    }

    lora_stream_frame_t frame = parse_lora_stream_frame(text);
    const char *payload = frame.payload ? frame.payload : "";

    if (!frame.is_stream) {
        const char *p = skip_ascii_ws(payload);

        bool partial_json_in_progress =
            lora_stream_len > 0 && (lora_stream_buf[0] == '{' || lora_stream_buf[0] == '[');

        if (partial_json_in_progress && !is_gateway_sideband_message(p)) {
            if (p[0] == '{' || p[0] == '[') {
                // A fresh JSON frame arrived while an older frame was incomplete.
                // Drop the stale partial frame to avoid unbounded append chatter.
                log_lora_rx_chain_event("reset-newframe", lora_rx_chain_id, p, lora_stream_len);
                lora_stream_len = 0;
                lora_stream_buf[0] = '\0';
                partial_json_in_progress = false;
            } else {
                size_t payload_len = strlen(p);
                size_t remaining = sizeof(lora_stream_buf) - 1 - lora_stream_len;
                if (payload_len > remaining) {
                    payload_len = remaining;
                }
                if (payload_len > 0) {
                    memcpy(&lora_stream_buf[lora_stream_len], p, payload_len);
                    lora_stream_len += payload_len;
                    lora_stream_buf[lora_stream_len] = '\0';
                }
                log_lora_rx_chain_event("append", lora_rx_chain_id, p, lora_stream_len);

                if (!looks_like_complete_supported_telemetry_frame(lora_stream_buf)) {
                    return false;
                }

                log_lora_rx_chain_event("complete", lora_rx_chain_id, lora_stream_buf, lora_stream_len);
                snprintf(last_lora_rx, sizeof(last_lora_rx), "%s", lora_stream_buf);
                last_lora_rx_count++;
                if (looks_like_supported_telemetry_frame(lora_stream_buf)) {
                    snprintf(last_lora_json, sizeof(last_lora_json), "%s", lora_stream_buf);
                    last_lora_json_count++;
                }
                lora_stream_len = 0;
                lora_stream_buf[0] = '\0';
                return true;
            }
        }

        if (partial_json_in_progress && is_gateway_sideband_message(p)) {
            log_lora_rx_chain_event("sideband", lora_rx_chain_id, p, lora_stream_len);
        }

        if (((*p == '{' || *p == '[') && !looks_like_complete_json_document(p))
            || (looks_like_supported_telemetry_frame(p) && !looks_like_complete_supported_telemetry_frame(p))) {
            lora_rx_chain_id++;
            snprintf(lora_stream_buf, sizeof(lora_stream_buf), "%s", p);
            lora_stream_len = strlen(lora_stream_buf);
            log_lora_rx_chain_event("start", lora_rx_chain_id, p, lora_stream_len);
            return false;
        }

        snprintf(last_lora_rx, sizeof(last_lora_rx), "%s", payload);
        last_lora_rx_count++;
        if (looks_like_supported_telemetry_frame(p) && looks_like_complete_supported_telemetry_frame(p)) {
            snprintf(last_lora_json, sizeof(last_lora_json), "%s", payload);
            last_lora_json_count++;
        }
        return true;
    }

    if (!lora_stream_seq_init) {
        lora_stream_expected_seq = frame.seq;
        lora_stream_seq_init = true;
    } else if (frame.seq != lora_stream_expected_seq) {
        log_lora_rx_chain_event("reset-seq", lora_rx_chain_id, payload, lora_stream_len);
        lora_stream_len = 0;
        lora_stream_buf[0] = '\0';
    }
    lora_stream_expected_seq = frame.seq + 1;

    size_t payload_len = strlen(payload);
    if (payload_len > 0 && payload[payload_len - 1] == '\n') {
        payload_len--;
    }

    size_t remaining = sizeof(lora_stream_buf) - 1 - lora_stream_len;
    if (payload_len > remaining) {
        payload_len = remaining;
    }
    if (payload_len > 0) {
        memcpy(&lora_stream_buf[lora_stream_len], payload, payload_len);
        lora_stream_len += payload_len;
        lora_stream_buf[lora_stream_len] = '\0';
    }
    if (frame.seq == 0 || lora_stream_len == payload_len) {
        lora_rx_chain_id++;
        log_lora_rx_chain_event(frame.has_end ? "stream-single" : "stream-start", lora_rx_chain_id, payload, lora_stream_len);
    } else {
        log_lora_rx_chain_event(frame.has_end ? "stream-end" : "stream-append", lora_rx_chain_id, payload, lora_stream_len);
    }

    if (!frame.has_end) {
        return false;
    }

    const char *assembled = skip_ascii_ws(lora_stream_buf);
    if (((*assembled == '{' || *assembled == '[') && !looks_like_complete_json_document(assembled))
        || (looks_like_supported_telemetry_frame(assembled) && !looks_like_complete_supported_telemetry_frame(assembled))) {
        log_lora_rx_chain_event("stream-incomplete", lora_rx_chain_id, assembled, lora_stream_len);
        return false;
    }

    log_lora_rx_chain_event("stream-complete", lora_rx_chain_id, assembled, lora_stream_len);
    snprintf(last_lora_rx, sizeof(last_lora_rx), "%s", lora_stream_buf);
    last_lora_rx_count++;
    const char *p = assembled;
    if (looks_like_supported_telemetry_frame(p)) {
        snprintf(last_lora_json, sizeof(last_lora_json), "%s", lora_stream_buf);
        last_lora_json_count++;
    }
    lora_stream_len = 0;
    lora_stream_buf[0] = '\0';
    return true;
}

static void capture_ack_if_present(const char *text) {
    if (!text) return;
    const char *ack = strstr(text, "ACK:");
    if (!ack || ack[0] == '\0') return;
    snprintf(last_ack_rx, sizeof(last_ack_rx), "%.150s", ack);
    snprintf(last_cmd_status, sizeof(last_cmd_status), "acknowledged");
    snprintf(lora_link_state, sizeof(lora_link_state), "online");
    ack_count++;
}

static void json_escape_string(const char *src, char *dst, size_t dst_size) {
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 1 < dst_size; ++i) {
        const unsigned char ch = (unsigned char)src[i];
        const char *replacement = NULL;

        switch (ch) {
            case '\\': replacement = "\\\\"; break;
            case '"': replacement = "\\\""; break;
            case '\n': replacement = "\\n"; break;
            case '\r': replacement = "\\r"; break;
            case '\t': replacement = "\\t"; break;
            default: break;
        }

        if (replacement) {
            const size_t replacement_len = strlen(replacement);
            if (out + replacement_len >= dst_size) {
                break;
            }
            memcpy(dst + out, replacement, replacement_len);
            out += replacement_len;
            continue;
        }

        if (ch < 0x20) {
            dst[out++] = ' ';
            continue;
        }

        dst[out++] = (char)ch;
    }

    dst[out] = '\0';
}

static void refresh_status_json(void) {
    static char state_escaped[sizeof(current_state) * 2];
    static char mode_escaped[sizeof(current_mode) * 2];
    static char radio_mode_escaped[8];
    static char wifi_escaped[sizeof(wifi_link_state) * 2];
    static char lora_escaped[sizeof(lora_link_state) * 2];
    static char cmd_escaped[sizeof(last_cmd) * 2];
    static char cmd_id_escaped[sizeof(last_cmd_id) * 2];
    static char cmd_status_escaped[sizeof(last_cmd_status) * 2];
    static char ack_escaped[sizeof(last_ack_rx) * 2];
    static char backend_url_escaped[sizeof(provisioned_backend_url) * 2];
    static char ap_ssid_escaped[sizeof(AP_SSID) * 2];

    json_escape_string(current_state, state_escaped, sizeof(state_escaped));
    json_escape_string(current_mode, mode_escaped, sizeof(mode_escaped));
    json_escape_string(radio_mode_name(s_radio_mode), radio_mode_escaped, sizeof(radio_mode_escaped));
    json_escape_string(wifi_link_state, wifi_escaped, sizeof(wifi_escaped));
    json_escape_string(lora_link_state, lora_escaped, sizeof(lora_escaped));
    json_escape_string(last_cmd, cmd_escaped, sizeof(cmd_escaped));
    json_escape_string(last_cmd_id, cmd_id_escaped, sizeof(cmd_id_escaped));
    json_escape_string(last_cmd_status, cmd_status_escaped, sizeof(cmd_status_escaped));
    json_escape_string(last_ack_rx, ack_escaped, sizeof(ack_escaped));
    json_escape_string(provisioned_backend_url, backend_url_escaped, sizeof(backend_url_escaped));
    json_escape_string(AP_SSID, ap_ssid_escaped, sizeof(ap_ssid_escaped));

    snprintf(status_json, sizeof(status_json),
             "{\"status_version\":3,\"battery\":85,\"state\":\"%s\",\"mode\":\"%s\",\"radio_mode\":\"%s\",\"wifi_link_state\":\"%s\",\"lora_link_state\":\"%s\",\"last_cmd\":\"%s\",\"last_cmd_id\":\"%s\",\"last_cmd_status\":\"%s\",\"queue_depth\":%d,\"ack_count\":%lu,\"last_ack\":\"%s\",\"configured\":%s,\"backend_url\":\"%s\",\"ap_ssid\":\"%s\"}",
             state_escaped,
             mode_escaped,
             radio_mode_escaped,
             wifi_escaped,
             lora_escaped,
             cmd_escaped,
             cmd_id_escaped,
             cmd_status_escaped,
             (int)(lora_cmd_q ? uxQueueMessagesWaiting(lora_cmd_q) : 0),
             (unsigned long)ack_count,
             ack_escaped,
             wifi_configured ? "true" : "false",
             backend_url_escaped,
             ap_ssid_escaped);
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

static esp_err_t post_payload_to_backend(const char *url, const char *body) {
    if (!url || !url[0] || !body || !body[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    bool http_locked = false;
    if (backend_http_lock) {
        if (xSemaphoreTake(backend_http_lock, pdMS_TO_TICKS(BACKEND_HTTP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Backend HTTP busy, skipping POST %s", url);
            return ESP_ERR_TIMEOUT;
        }
        http_locked = true;
    }

    const char *cert_pem = backend_cert_pem_for_url(url);
    if (cert_pem) {
        ESP_LOGI(TAG, "Using pinned onrender CA chain for %s", url);
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = BACKEND_HTTP_TIMEOUT_MS,
        .cert_pem = cert_pem,
        .cert_len = cert_pem ? strlen(cert_pem) + 1 : 0,
        .crt_bundle_attach = cert_pem ? NULL : esp_crt_bundle_attach,
        .common_name = cert_pem ? "onrender.com" : NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        if (http_locked) {
            xSemaphoreGive(backend_http_lock);
        }
        return ESP_FAIL;
    }

    const char *trimmed = skip_ascii_ws(body);
    const bool looks_like_json = trimmed && (*trimmed == '{' || *trimmed == '[');
    esp_http_client_set_header(client, "Content-Type", looks_like_json ? "application/json" : "text/plain");
    if (provisioned_board_api_key[0] != '\0') {
        esp_http_client_set_header(client, "x-api-key", provisioned_board_api_key);
    }
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    s_last_backend_http_status = status_code;
    esp_http_client_cleanup(client);
    if (http_locked) {
        xSemaphoreGive(backend_http_lock);
    }

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

    bool http_locked = false;
    if (backend_http_lock) {
        if (xSemaphoreTake(backend_http_lock, pdMS_TO_TICKS(BACKEND_HTTP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Backend HTTP busy, skipping GET %s", url);
            return ESP_ERR_TIMEOUT;
        }
        http_locked = true;
    }

    const char *cert_pem = backend_cert_pem_for_url(url);
    if (cert_pem) {
        ESP_LOGI(TAG, "Using pinned onrender CA chain for %s", url);
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = BACKEND_HTTP_TIMEOUT_MS,
        .cert_pem = cert_pem,
        .cert_len = cert_pem ? strlen(cert_pem) + 1 : 0,
        .crt_bundle_attach = cert_pem ? NULL : esp_crt_bundle_attach,
        .common_name = cert_pem ? "onrender.com" : NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        if (http_locked) {
            xSemaphoreGive(backend_http_lock);
        }
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
    if (http_locked) {
        xSemaphoreGive(backend_http_lock);
    }
    return err;
}

static esp_err_t queue_command_for_lora(const char *command, const char *command_id) {
    static char last_motion_cmd[LORA_CMD_MAX] = "";
    static TickType_t last_motion_cmd_tick = 0;

    if (!command || !command[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    bool motion_cmd = is_motion_command(command);
    bool mode_cmd = is_mode_command(command);
    TickType_t now = xTaskGetTickCount();
    if (motion_cmd
        && !command_has_drive_sequence(command)
        && strcmp(last_motion_cmd, command) == 0
        && (now - last_motion_cmd_tick) < pdMS_TO_TICKS(LOCAL_MOTION_DUPLICATE_SUPPRESS_MS)) {
        return ESP_OK;
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

    bool enqueue_to_front = false;
    if (motion_cmd) {
        UBaseType_t pending = uxQueueMessagesWaiting(lora_cmd_q);
        if (pending > 0) {
            xQueueReset(lora_cmd_q);
            ESP_LOGW(TAG, "Dropped %u stale queued motion command(s)", (unsigned)pending);
        }
    } else if (mode_cmd) {
        enqueue_to_front = true;
        if (uxQueueSpacesAvailable(lora_cmd_q) == 0) {
            lora_cmd_t dropped = {0};
            if (xQueueReceive(lora_cmd_q, &dropped, 0) == pdTRUE) {
                ESP_LOGW(TAG, "Dropped oldest queued command to prioritize mode command: %s", command);
            }
        }
    }

    BaseType_t queued_ok = enqueue_to_front
        ? xQueueSendToFront(lora_cmd_q, &c, pdMS_TO_TICKS(30))
        : xQueueSend(lora_cmd_q, &c, pdMS_TO_TICKS(30));

    if (queued_ok != pdTRUE) {
        xSemaphoreTake(status_lock, portMAX_DELAY);
        snprintf(last_cmd_status, sizeof(last_cmd_status), "failed");
        snprintf(lora_link_state, sizeof(lora_link_state), "degraded");
        refresh_status_json();
        xSemaphoreGive(status_lock);
        return ESP_ERR_TIMEOUT;
    }

    if (motion_cmd) {
        snprintf(last_motion_cmd, sizeof(last_motion_cmd), "%s", command);
        last_motion_cmd_tick = now;
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
    char legacy_endpoint[MAX_BACKEND_URL_LEN + 64] = {0};
    char fallback_endpoint[MAX_BACKEND_URL_LEN + 64] = {0};
    char fallback2_endpoint[MAX_BACKEND_URL_LEN + 64] = {0};
    char fallback3_endpoint[MAX_BACKEND_URL_LEN + 64] = {0};
    static char body[384];
    body[0] = '\0';

    xSemaphoreTake(status_lock, portMAX_DELAY);
    snprintf(backend_url, sizeof(backend_url), "%s", provisioned_backend_url);
    xSemaphoreGive(status_lock);

    if (backend_url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (!build_backend_endpoint(backend_url, "/api/base-station/command-ack", endpoint, sizeof(endpoint))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!build_backend_endpoint(backend_url, "/api/base-station/command/ack", legacy_endpoint, sizeof(legacy_endpoint))) {
        legacy_endpoint[0] = '\0';
    }
    if (!build_backend_endpoint(backend_url, "/api/base-station/ack", fallback_endpoint, sizeof(fallback_endpoint))) {
        fallback_endpoint[0] = '\0';
    }
    if (!build_backend_endpoint(backend_url, "/api/base-station/commandAck", fallback2_endpoint, sizeof(fallback2_endpoint))) {
        fallback2_endpoint[0] = '\0';
    }
    if (!build_backend_endpoint(backend_url, "/api/base-station/cmd-ack", fallback3_endpoint, sizeof(fallback3_endpoint))) {
        fallback3_endpoint[0] = '\0';
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

    esp_err_t ack_err = post_payload_to_backend(endpoint, body);
    if (ack_err != ESP_OK && s_last_backend_http_status == 404) {
        if (legacy_endpoint[0] != '\0') {
            ESP_LOGW(TAG, "Primary command-ack endpoint returned 404, trying /command/ack");
            ack_err = post_payload_to_backend(legacy_endpoint, body);
        }
        if (ack_err != ESP_OK && s_last_backend_http_status == 404 && fallback_endpoint[0] != '\0') {
            ESP_LOGW(TAG, "Legacy ACK endpoint returned 404, trying /ack");
            ack_err = post_payload_to_backend(fallback_endpoint, body);
        }
        if (ack_err != ESP_OK && s_last_backend_http_status == 404 && fallback2_endpoint[0] != '\0') {
            ESP_LOGW(TAG, "ACK endpoint returned 404, trying /commandAck");
            ack_err = post_payload_to_backend(fallback2_endpoint, body);
        }
        if (ack_err != ESP_OK && s_last_backend_http_status == 404 && fallback3_endpoint[0] != '\0') {
            ESP_LOGW(TAG, "ACK endpoint returned 404, trying /cmd-ack");
            ack_err = post_payload_to_backend(fallback3_endpoint, body);
        }
    }
    return ack_err;
}

static bool fetch_remote_command_from_backend(char *cmd_out, size_t cmd_out_size, char *command_id_out, size_t command_id_size) {
    char backend_url[MAX_BACKEND_URL_LEN] = {0};
    char endpoint[MAX_BACKEND_URL_LEN + 64] = {0};
    static char response[512];
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

static void backend_sync_task(void *arg) {
    (void)arg;
    char wifi_state[sizeof(wifi_link_state)] = {0};
    char backend_url[MAX_BACKEND_URL_LEN] = {0};
    char command[LORA_CMD_MAX] = {0};
    char last_remote_command[LORA_CMD_MAX] = {0};
    char command_id[sizeof(last_cmd_id)] = {0};
    char last_remote_command_id[sizeof(last_cmd_id)] = {0};
    char telemetry_payload[sizeof(last_lora_json)] = {0};
    char status_payload[sizeof(status_json)] = {0};
    char endpoint[MAX_BACKEND_URL_LEN + 64] = {0};
    uint32_t telemetry_count = 0;
    uint32_t last_sent_telemetry_count = 0;
    TickType_t last_command_poll = 0;
    TickType_t last_remote_command_tick = 0;
    TickType_t last_telemetry_post = 0;
    TickType_t last_status_post = 0;

    while (1) {
        TickType_t now = xTaskGetTickCount();

        xSemaphoreTake(status_lock, portMAX_DELAY);
        snprintf(wifi_state, sizeof(wifi_state), "%s", wifi_link_state);
        snprintf(backend_url, sizeof(backend_url), "%s", provisioned_backend_url);
        xSemaphoreGive(status_lock);

        if (strcmp(wifi_state, "online") == 0 && backend_url[0] != '\0') {
            if ((now - last_command_poll) >= pdMS_TO_TICKS(REMOTE_COMMAND_POLL_INTERVAL_MS)) {
                last_command_poll = now;
                command[0] = '\0';
                command_id[0] = '\0';

                if (fetch_remote_command_from_backend(command, sizeof(command), command_id, sizeof(command_id))) {
                    if (command_id[0] != '\0' && strcmp(command_id, last_remote_command_id) == 0) {
                        (void)post_remote_command_ack(command_id, "forwarded", NULL);
                    } else {
                        bool same_cmd_recent = is_motion_command(command)
                            && (last_remote_command[0] != '\0')
                            && (strcmp(command, last_remote_command) == 0)
                            && ((now - last_remote_command_tick) < pdMS_TO_TICKS(REMOTE_COMMAND_DUPLICATE_SUPPRESS_MS));

                        if (same_cmd_recent) {
                            ESP_LOGI(TAG, "Remote backend command duplicate suppressed: %s", command);
                            if (command_id[0] != '\0') {
                                snprintf(last_remote_command_id, sizeof(last_remote_command_id), "%s", command_id);
                                (void)post_remote_command_ack(command_id, "forwarded", NULL);
                            }
                        } else {
                            esp_err_t queue_err = queue_command_for_lora(command, command_id);
                            if (queue_err == ESP_OK) {
                                ESP_LOGI(TAG, "Remote backend command queued: %s", command);
                                snprintf(last_remote_command, sizeof(last_remote_command), "%s", command);
                                last_remote_command_tick = now;
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
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }
            }

            if ((now - last_telemetry_post) >= pdMS_TO_TICKS(TELEMETRY_POST_INTERVAL_MS)) {
                last_telemetry_post = now;
                telemetry_payload[0] = '\0';
                telemetry_count = 0;

                xSemaphoreTake(status_lock, portMAX_DELAY);
                snprintf(telemetry_payload, sizeof(telemetry_payload), "%s", last_lora_json);
                telemetry_count = last_lora_json_count;
                xSemaphoreGive(status_lock);

                if (looks_like_supported_telemetry_frame(telemetry_payload)
                    && looks_like_complete_supported_telemetry_frame(telemetry_payload)
                    && telemetry_count != 0
                    && telemetry_count != last_sent_telemetry_count
                    && build_backend_endpoint(backend_url, "/api/telemetry", endpoint, sizeof(endpoint))) {
                    esp_err_t err = post_payload_to_backend(endpoint, telemetry_payload);
                    if (err == ESP_OK) {
                        last_sent_telemetry_count = telemetry_count;
                        ESP_LOGI(TAG, "Telemetry pushed to backend");
                    } else {
                        if (s_last_backend_http_status == 400) {
                            last_sent_telemetry_count = telemetry_count;
                            ESP_LOGW(TAG, "Telemetry rejected by backend (HTTP 400, len=%u), suppressing retries for payload: %.180s", (unsigned)strlen(telemetry_payload), telemetry_payload);
                        }
                        ESP_LOGW(TAG, "Telemetry push failed: %s", esp_err_to_name(err));
                    }
                } else if (looks_like_supported_telemetry_frame(telemetry_payload)
                           && !looks_like_complete_supported_telemetry_frame(telemetry_payload)) {
                    last_sent_telemetry_count = telemetry_count;
                    ESP_LOGW(TAG, "Telemetry payload incomplete or malformed locally (len=%u): %.180s", (unsigned)strlen(telemetry_payload), telemetry_payload);
                }
            }

            if ((now - last_status_post) >= pdMS_TO_TICKS(BASE_STATUS_POST_INTERVAL_MS)) {
                last_status_post = now;

                xSemaphoreTake(status_lock, portMAX_DELAY);
                refresh_status_json();
                snprintf(status_payload, sizeof(status_payload), "%s", status_json);
                xSemaphoreGive(status_lock);

                if (build_backend_endpoint(backend_url, "/api/base-station/status", endpoint, sizeof(endpoint))) {
                    esp_err_t err = post_payload_to_backend(endpoint, status_payload);
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG, "Base status pushed to backend");
                    } else {
                        ESP_LOGW(TAG, "Base status push failed: %s", esp_err_to_name(err));
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
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
             "{\"ok\":true,\"configured\":%s,\"mode\":\"%.16s\",\"radioMode\":\"%s\",\"state\":\"%.32s\",\"apSsid\":\"%s\",\"savedSsid\":\"%.32s\",\"backendUrl\":\"%.150s\",\"wifiLinkState\":\"%.16s\",\"boardApiKeySet\":%s}",
             wifi_configured ? "true" : "false",
             current_mode,
             radio_mode_name(s_radio_mode),
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
    xTaskCreate(delayed_restart_task, "restart", RESTART_TASK_STACK_SIZE, NULL, 3, NULL);
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
        sync_time_with_sntp();
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

    if (!sta_netif_created) {
        esp_netif_create_default_wifi_sta();
        sta_netif_created = true;
    }
    if (!ap_netif_created) {
        esp_netif_create_default_wifi_ap();
        ap_netif_created = true;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    load_provisioned_network_config();

    if (wifi_configured) {
        if (try_sta_mode(provisioned_ssid, provisioned_pass)) {
            return;
        }
        if (STA_SSID[0] != '\0' && strcmp(provisioned_ssid, STA_SSID) != 0) {
            ESP_LOGI(TAG, "Provisioned STA failed; trying built-in fallback SSID=%s", STA_SSID);
            if (try_sta_mode(STA_SSID, STA_PASS)) {
                return;
            }
        }
        return;
    }

    if (STA_SSID[0] != '\0') {
        ESP_LOGI(TAG, "No provisioned Wi-Fi; trying built-in fallback SSID=%s", STA_SSID);
        if (try_sta_mode(STA_SSID, STA_PASS)) {
            return;
        }
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

    radio_apply_mode(RADIO_MODE_LORA);
    ESP_LOGI(TAG, "LoRa/GFSK radio ready @ %d Hz", (int)LORA_FREQ_HZ);
}

static void lora_tx_task(void *arg) {
    (void)arg;
    lora_cmd_t c;
    TickType_t last_motion_log_tick = 0;

    while (1) {
        if (xQueueReceive(lora_cmd_q, &c, portMAX_DELAY) == pdTRUE) {
            if (c.len > 0) {
                const bool switch_to_gfsk = BASE_AUTO_RADIO_MODE_SWITCH && (s_radio_mode == RADIO_MODE_LORA) && is_manual_entry_command(c.payload);
                const bool switch_to_lora = BASE_AUTO_RADIO_MODE_SWITCH && (s_radio_mode == RADIO_MODE_GFSK) && is_lora_restore_command(c.payload);
                const int tx_burst = is_motion_command(c.payload) ? 2 : (is_mode_command(c.payload) ? 2 : 1);
                bool sent = false;
                bool switch_sent = false;
                const bool motion_command = is_motion_command(c.payload);
                const TickType_t now = xTaskGetTickCount();

                if (!motion_command || (now - last_motion_log_tick) >= pdMS_TO_TICKS(BASE_MOTION_LOG_THROTTLE_MS)) {
                    ESP_LOGI(TAG, "Radio TX (%s): %.*s", radio_mode_name(s_radio_mode), c.len, c.payload);
                    if (motion_command) {
                        last_motion_log_tick = now;
                    }
                }
                if (lora_lock) {
                    xSemaphoreTake(lora_lock, portMAX_DELAY);
                }
                sent = lora_send_payload_with_retries_locked(c.payload, tx_burst);
                if (sent && switch_to_gfsk) {
                    ESP_LOGI(TAG, "Radio TX (%s): %s", radio_mode_name(s_radio_mode), RADIO_SWITCH_TO_GFSK_FRAME);
                    switch_sent = lora_send_payload_with_retries_locked(RADIO_SWITCH_TO_GFSK_FRAME, 2);
                    if (switch_sent) {
                        radio_apply_mode_locked(RADIO_MODE_GFSK);
                    }
                } else if (sent && switch_to_lora) {
                    ESP_LOGI(TAG, "Radio TX (%s): %s", radio_mode_name(s_radio_mode), RADIO_SWITCH_TO_LORA_FRAME);
                    switch_sent = lora_send_payload_with_retries_locked(RADIO_SWITCH_TO_LORA_FRAME, 2);
                    if (switch_sent) {
                        radio_apply_mode_locked(RADIO_MODE_LORA);
                    }
                }
                if (lora_lock) {
                    xSemaphoreGive(lora_lock);
                }

                if (!sent || ((switch_to_gfsk || switch_to_lora) && !switch_sent)) {
                    ESP_LOGE(TAG, "LoRaSend failed after retries");
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
    char completed_payload[180];
    TickType_t last_manual_log_tick = 0;
    const TickType_t manual_log_interval_ticks = pdMS_TO_TICKS(1000);

    while (1) {
        uint8_t n = 0;
        completed_payload[0] = '\0';
        bool lock_acquired = false;
        if (lora_lock) {
            lock_acquired = (xSemaphoreTake(lora_lock, pdMS_TO_TICKS(2)) == pdTRUE);
        }
        if (!lora_lock || lock_acquired) {
            n = LoRaReceive(rx, sizeof(rx) - 1);
            if (lora_lock) {
                xSemaphoreGive(lora_lock);
            }
        }
        if (n > 0) {
            rx[n] = '\0';

            bool payload_complete = false;
            bool completed_json_is_valid = true;
            xSemaphoreTake(status_lock, portMAX_DELAY);
            payload_complete = store_lora_rx_payload((char *)rx);
            if (payload_complete) {
                const char *logged_payload = skip_ascii_ws(last_lora_rx);
                if ((*logged_payload == '{' || *logged_payload == '[') &&
                    !looks_like_complete_json_document(logged_payload)) {
                    payload_complete = false;
                    completed_json_is_valid = false;
                }
            }
            if (payload_complete) {
                snprintf(completed_payload, sizeof(completed_payload), "%.179s", last_lora_rx);
                capture_ack_if_present(last_lora_rx);
                snprintf(current_state, sizeof(current_state), "IDLE");
                snprintf(current_mode, sizeof(current_mode), "%s", radio_mode_name(s_radio_mode));
                snprintf(lora_link_state, sizeof(lora_link_state), "online");
                refresh_status_json();
            }
            xSemaphoreGive(status_lock);

            if (!completed_json_is_valid) {
                ESP_LOGW(TAG, "Suppressed incomplete LoRa JSON payload while waiting for more fragments");
            }
            if (payload_complete) {
                const bool compact_manual = strstr(completed_payload, "\"s\":\"MANUAL\"") != NULL;
                if (compact_manual) {
                    TickType_t now = xTaskGetTickCount();
                    if ((now - last_manual_log_tick) >= manual_log_interval_ticks) {
                        last_manual_log_tick = now;
                        ESP_LOGI(TAG, "LoRa RX (manual throttled): %s", completed_payload);
                    }
                } else {
                    ESP_LOGI(TAG, "LoRa RX: %s", completed_payload);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    status_lock = xSemaphoreCreateMutex();
    backend_http_lock = xSemaphoreCreateMutex();
    lora_lock = xSemaphoreCreateMutex();

    display_available = display_init();
    if (display_available) {
        display_show_splash("BOOTING", "CONNECTING...");
    }

    start_sta_then_fallback_ap();
    start_mdns_service();
    start_http_server();

    lora_cmd_q = xQueueCreate(LORA_QUEUE_DEPTH, sizeof(lora_cmd_t));
    lora_init();
    xTaskCreate(lora_tx_task, "lora_tx", LORA_TASK_STACK_SIZE, NULL, 5, NULL);
    xTaskCreate(lora_rx_task, "lora_rx", LORA_TASK_STACK_SIZE, NULL, 5, NULL);
    xTaskCreate(backend_sync_task, "backend_sync", BACKEND_HTTP_TASK_STACK_SIZE, NULL, 4, NULL);

    if (display_available) {
        xTaskCreate(display_task, "display", DISPLAY_TASK_STACK_SIZE, NULL, 3, NULL);
    }
}






