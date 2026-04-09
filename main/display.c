#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"
#include "ssd1306.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"

#define OLED_SDA 17
#define OLED_SCL 18
#define OLED_RST 21
#define OLED_VEXT 36
#define OLED_I2C_PORT I2C_NUM_0
#define OLED_TEXT_LINE_LEN 21

static SSD1306_t dev;
static const char *TAG = "DISPLAY";

static void write_line(int row, const char *text, char *cache, size_t cache_size) {
    char buffer[OLED_TEXT_LINE_LEN + 1];
    snprintf(buffer, sizeof(buffer), "%-21.21s", text ? text : "");
    if (strncmp(buffer, cache, cache_size) == 0) {
        return;
    }
    ssd1306_display_text(&dev, row, buffer, strlen(buffer), false);
    snprintf(cache, cache_size, "%s", buffer);
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
    snprintf(out, out_size, "%.*s", (int)((len < out_size - 1) ? len : out_size - 1), start);
}

static bool oled_probe(void) {
    const uint8_t addresses[] = {0x3C, 0x3D};
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
    ssd1306_display_text(&dev, 1, "SALT ROBOT BASE", 15, false);
    ssd1306_display_text(&dev, 3, line1 ? line1 : "BOOTING", strlen(line1 ? line1 : "BOOTING"), false);
    ssd1306_display_text(&dev, 5, line2 ? line2 : "CONNECTING", strlen(line2 ? line2 : "CONNECTING"), false);
}

void display_show_status(const char *mode,
                         const char *wifi,
                         const char *lora,
                         const char *target_network,
                         const char *backend_url,
                         int queue_depth,
                         const char *cmd,
                         const char *ack,
                         uint32_t ack_count) {
    static char cache[8][OLED_TEXT_LINE_LEN + 1] = {{0}};
    char line[OLED_TEXT_LINE_LEN + 1];
    char backend_label[17];

    compact_backend_label(backend_url, backend_label, sizeof(backend_label));

    write_line(0, "SALT ROBOT BASE", cache[0], sizeof(cache[0]));

    snprintf(line, sizeof(line), "MODE:%-.15s", mode ? mode : "none");
    write_line(1, line, cache[1], sizeof(cache[1]));

    snprintf(line, sizeof(line), "WIFI:%-.15s", wifi ? wifi : "none");
    write_line(2, line, cache[2], sizeof(cache[2]));

    snprintf(line, sizeof(line), "NET:%-.16s", target_network ? target_network : "none");
    write_line(3, line, cache[3], sizeof(cache[3]));

    snprintf(line, sizeof(line), "BACK:%-.15s", backend_label);
    write_line(4, line, cache[4], sizeof(cache[4]));

    snprintf(line, sizeof(line), "L:%-.6s Q:%d", lora ? lora : "none", queue_depth);
    write_line(5, line, cache[5], sizeof(cache[5]));

    snprintf(line, sizeof(line), "CMD:%-.17s", cmd ? cmd : "none");
    write_line(6, line, cache[6], sizeof(cache[6]));

    snprintf(line, sizeof(line), "ACK:%-.11s %lu", ack ? ack : "-", (unsigned long)ack_count);
    write_line(7, line, cache[7], sizeof(cache[7]));
}
