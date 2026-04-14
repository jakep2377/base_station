#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"
#include "ssd1306.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define OLED_SDA 17
#define OLED_SCL 18
#define OLED_RST 21
#define OLED_VEXT 36
#define OLED_I2C_PORT I2C_NUM_0
#define OLED_TEXT_LINE_LEN 21
#define OLED_PAGE_SWITCH_MS 7000

static SSD1306_t dev;
static const char *TAG = "DISPLAY";

static void write_line_ex(int row, const char *text, char *cache, size_t cache_size, bool invert) {
    char buffer[OLED_TEXT_LINE_LEN + 1];
    snprintf(buffer, sizeof(buffer), "%-21.21s", text ? text : "");
    if (!invert && strncmp(buffer, cache, cache_size) == 0) {
        return;
    }

    ssd1306_display_text(&dev, row, buffer, strlen(buffer), invert);

    snprintf(cache, cache_size, "%s", buffer);
}

static void write_line(int row, const char *text, char *cache, size_t cache_size) {
    write_line_ex(row, text, cache, cache_size, false);
}

static void compact_backend_label(const char *backend_url, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!backend_url || !backend_url[0]) {
        snprintf(out, out_size, "none");
        return;
    }

    const char *start = strstr(backend_url, "://");
    start = start ? start + 3 : backend_url;
    const char *end = strchr(start, '/');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len == 0) {
        snprintf(out, out_size, "none");
        return;
    }

    if (len >= out_size && strchr(start, '.') != NULL) {
        const char *last_dot = start + len;
        const char *prev_dot = NULL;
        for (const char *p = start; p < start + len; ++p) {
            if (*p == '.') {
                prev_dot = last_dot;
                last_dot = p;
            }
        }
        if (prev_dot && prev_dot < start + len) {
            start = prev_dot + 1;
            len = end ? (size_t)(end - start) : strlen(start);
        }
    }

    snprintf(out, out_size, "%.*s", (int)((len < out_size - 1) ? len : out_size - 1), start);
}

static const char *display_value_start(const char *input) {
    static const char empty[] = "none";

    if (!input || !input[0]) {
        return empty;
    }

    const char *start = input;
    if (strncmp(start, "CMD:", 4) == 0 || strncmp(start, "ACK:", 4) == 0) {
        start += 4;
    }
    while (*start == ' ') {
        start++;
    }

    return *start ? start : empty;
}

static void compact_token_label(const char *input, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }

    out[0] = '\0';
    const char *start = display_value_start(input);

    size_t len = 0;
    while (start[len] && start[len] != ',' && start[len] != ' ' && len < out_size - 1) {
        len++;
    }

    snprintf(out, out_size, "%.*s", (int)len, start);
    if (out[0] == '\0') {
        snprintf(out, out_size, "none");
    }
}

static void copy_windowed_text(const char *input, char *out, size_t out_size, size_t offset) {
    if (!out || out_size == 0) {
        return;
    }

    out[0] = '\0';
    const char *start = display_value_start(input);
    size_t width = out_size - 1;
    size_t len = strlen(start);

    if (len <= width) {
        snprintf(out, out_size, "%s", start);
        return;
    }

    size_t max_start = len - width;
    size_t begin = offset % (max_start + 1);
    snprintf(out, out_size, "%.*s", (int)width, start + begin);
}

static void compact_state_label(const char *input, char *out, size_t out_size) {
    char token[16];
    compact_token_label(input, token, sizeof(token));

    char lower[16];
    size_t i = 0;
    for (; token[i] && i < sizeof(lower) - 1; ++i) {
        lower[i] = (char)tolower((unsigned char)token[i]);
    }
    lower[i] = '\0';

    if (strcmp(lower, "connected") == 0 || strcmp(lower, "online") == 0 || strcmp(lower, "ready") == 0 || strcmp(lower, "ok") == 0 || strcmp(lower, "committed") == 0) {
        snprintf(out, out_size, "OK");
    } else if (strstr(lower, "setup") || strstr(lower, "config") || strcmp(lower, "ap") == 0) {
        snprintf(out, out_size, "SETUP");
    } else if (strstr(lower, "degrad") || strstr(lower, "stale") || strstr(lower, "warn")) {
        snprintf(out, out_size, "WARN");
    } else if (strcmp(lower, "none") == 0 || strstr(lower, "off") != NULL || strstr(lower, "disconn") != NULL) {
        snprintf(out, out_size, "OFF");
    } else {
        size_t j = 0;
        for (; token[j] && j < out_size - 1; ++j) {
            out[j] = (char)toupper((unsigned char)token[j]);
        }
        out[j] = '\0';
        if (out[0] == '\0') {
            snprintf(out, out_size, "--");
        }
    }
}

static void build_alert_headline(const char *wifi_label,
                                 const char *lora_label,
                                 int queue_depth,
                                 const char *cmd_status_label,
                                 char *out,
                                 size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }

    if (strcmp(wifi_label, "OK") != 0) {
        snprintf(out, out_size, "WIFI %s", wifi_label);
    } else if (strcmp(lora_label, "OK") != 0) {
        snprintf(out, out_size, "RADIO %s", lora_label);
    } else if (queue_depth > 0) {
        snprintf(out, out_size, "QUEUE %02d", queue_depth);
    } else if (cmd_status_label && strcmp(cmd_status_label, "OK") != 0 && strcmp(cmd_status_label, "IDLE") != 0) {
        snprintf(out, out_size, "CMD %s", cmd_status_label);
    } else {
        snprintf(out, out_size, "CHECK LINKS");
    }
}

static bool oled_probe(void) {
    const uint8_t addresses[] = {0x3C, 0x3D};

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    if (dev._i2c_bus_handle == NULL) {
        ESP_LOGW(TAG, "OLED probe skipped because the I2C bus handle is not ready");
        return false;
    }

    for (size_t i = 0; i < sizeof(addresses); ++i) {
        esp_err_t ret = i2c_master_probe(dev._i2c_bus_handle, addresses[i], 50);
        if (ret == ESP_OK) {
            dev._address = addresses[i];
            ESP_LOGI(TAG, "OLED found at address 0x%02X", addresses[i]);
            return true;
        }
    }
#else
    for (size_t i = 0; i < sizeof(addresses); ++i) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addresses[i] << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(OLED_I2C_PORT, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            dev._address = addresses[i];
            ESP_LOGI(TAG, "OLED found at address 0x%02X", addresses[i]);
            return true;
        }
    }
#endif

    ESP_LOGW(TAG, "OLED probe failed at 0x3C and 0x3D");
    return false;
}

bool display_init(void) {
    gpio_config_t vext_conf = {
        .pin_bit_mask = 1ULL << OLED_VEXT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&vext_conf));
    ESP_ERROR_CHECK(gpio_set_level(OLED_VEXT, 0));
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_config_t rst_conf = {
        .pin_bit_mask = 1ULL << OLED_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_conf));
    ESP_ERROR_CHECK(gpio_set_level(OLED_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_ERROR_CHECK(gpio_set_level(OLED_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(20));

    i2c_master_init(&dev, OLED_SDA, OLED_SCL, -1);
    vTaskDelay(pdMS_TO_TICKS(20));

    if (!oled_probe()) {
        return false;
    }

    ssd1306_init(&dev, 128, 64);
    ssd1306_clear_screen(&dev, false);
    ssd1306_display_text(&dev, 2, "DISPLAY TEST", 12, false);
    ssd1306_display_text(&dev, 4, "PLEASE WAIT", 11, false);
    vTaskDelay(pdMS_TO_TICKS(220));
    ssd1306_clear_screen(&dev, false);
    ESP_LOGI(TAG, "Display initialized");
    return true;
}

void display_show_splash(const char *line1, const char *line2) {
    ssd1306_clear_screen(&dev, false);
    ssd1306_display_text(&dev, 1, "NAVIGATOR BASE", 14, false);
    ssd1306_display_text(&dev, 3, line1 ? line1 : "FIELD READYING", strlen(line1 ? line1 : "FIELD READYING"), false);
    ssd1306_display_text(&dev, 5, line2 ? line2 : "CONNECTING", strlen(line2 ? line2 : "CONNECTING"), false);
}

void display_show_status(const char *mode,
                         const char *state,
                         const char *wifi,
                         const char *lora,
                         const char *target_network,
                         const char *backend_url,
                         int queue_depth,
                         const char *cmd,
                         const char *cmd_status,
                         const char *ack,
                         uint32_t ack_count) {
    static char cache[8][OLED_TEXT_LINE_LEN + 1] = {{0}};
    static int last_page = -1;
    char line[OLED_TEXT_LINE_LEN + 1];
    char detail[OLED_TEXT_LINE_LEN + 1];
    char backend_label[17];
    char mode_label[12];
    char state_label[12];
    char wifi_label[12];
    char lora_label[12];
    char cmd_label[14];
    char cmd_status_label[12];
    char ack_label[12];
    char alert_headline[OLED_TEXT_LINE_LEN + 1];
    TickType_t now = xTaskGetTickCount();

    compact_backend_label(backend_url, backend_label, sizeof(backend_label));
    compact_state_label(mode, mode_label, sizeof(mode_label));
    compact_state_label(state, state_label, sizeof(state_label));
    compact_state_label(wifi, wifi_label, sizeof(wifi_label));
    compact_state_label(lora, lora_label, sizeof(lora_label));
    compact_token_label(cmd, cmd_label, sizeof(cmd_label));
    compact_state_label(cmd_status, cmd_status_label, sizeof(cmd_status_label));
    compact_token_label(ack, ack_label, sizeof(ack_label));
    build_alert_headline(wifi_label, lora_label, queue_depth, cmd_status_label, alert_headline, sizeof(alert_headline));

    bool show_alert = (strcmp(wifi_label, "OK") != 0) ||
                      (strcmp(lora_label, "OK") != 0) ||
                      (queue_depth > 0) ||
                      (strcmp(cmd_status_label, "OK") != 0 && strcmp(cmd_status_label, "IDLE") != 0);
    int page_count = show_alert ? 3 : 2;
    int page = (int)((now / pdMS_TO_TICKS(OLED_PAGE_SWITCH_MS)) % page_count);

    if (page != last_page) {
        memset(cache, 0, sizeof(cache));
        ssd1306_clear_screen(&dev, false);
        last_page = page;
    }

    if (page == 0) {
        write_line(0, "BASE STATUS P1", cache[0], sizeof(cache[0]));

        snprintf(line, sizeof(line), "MODE:%-.10s", mode_label);
        write_line(1, line, cache[1], sizeof(cache[1]));

        snprintf(line, sizeof(line), "STATE:%-.9s", state_label);
        write_line(2, line, cache[2], sizeof(cache[2]));

        snprintf(line, sizeof(line), "W:%-.5s L:%-.5s", wifi_label, lora_label);
        write_line(3, line, cache[3], sizeof(cache[3]));

        write_line(4, "NETWORK", cache[4], sizeof(cache[4]));
        copy_windowed_text(target_network, detail, sizeof(detail), 0);
        write_line(5, detail, cache[5], sizeof(cache[5]));

        write_line(6, "API HOST", cache[6], sizeof(cache[6]));
        copy_windowed_text(backend_label, detail, sizeof(detail), 0);
        write_line(7, detail, cache[7], sizeof(cache[7]));
    } else if (page == 1) {
        write_line(0, "BASE DETAIL P2", cache[0], sizeof(cache[0]));

        write_line(1, "CMD", cache[1], sizeof(cache[1]));
        copy_windowed_text(cmd, detail, sizeof(detail), 0);
        write_line(2, detail, cache[2], sizeof(cache[2]));

        snprintf(line, sizeof(line), "STAT:%-.10s", cmd_status_label);
        write_line(3, line, cache[3], sizeof(cache[3]));
        snprintf(line, sizeof(line), "Q:%02d ACK#:%02lu", queue_depth, (unsigned long)(ack_count % 100));
        write_line(4, line, cache[4], sizeof(cache[4]));

        write_line(5, "LAST ACK", cache[5], sizeof(cache[5]));
        copy_windowed_text(ack, detail, sizeof(detail), 0);
        write_line(6, detail, cache[6], sizeof(cache[6]));
        snprintf(line, sizeof(line), "M:%.4s S:%.6s", mode_label, state_label);
        write_line(7, line, cache[7], sizeof(cache[7]));
    } else {
        write_line_ex(0, "!!! BASE ALERT", cache[0], sizeof(cache[0]), true);

        write_line_ex(1, alert_headline, cache[1], sizeof(cache[1]), true);

        snprintf(line, sizeof(line), "W:%-.5s L:%-.5s", wifi_label, lora_label);
        write_line_ex(2, line, cache[2], sizeof(cache[2]), true);

        snprintf(line, sizeof(line), "MODE:%-.5s ST:%-.6s", mode_label, state_label);
        write_line_ex(3, line, cache[3], sizeof(cache[3]), true);

        snprintf(line, sizeof(line), "Q:%02d CMD:%-.8s", queue_depth, cmd_label);
        write_line_ex(4, line, cache[4], sizeof(cache[4]), true);

        snprintf(line, sizeof(line), "STAT:%-.10s", cmd_status_label);
        write_line_ex(5, line, cache[5], sizeof(cache[5]), true);

        snprintf(line, sizeof(line), "ACK:%-.12s", ack_label);
        write_line_ex(6, line, cache[6], sizeof(cache[6]), true);

        copy_windowed_text(target_network, detail, sizeof(detail), 0);
        write_line_ex(7, detail, cache[7], sizeof(cache[7]), true);
    }
}
