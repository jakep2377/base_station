#include <stdio.h>
#include <string.h>
#include "display.h"
#include "ssd1306.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define OLED_SDA 17
#define OLED_SCL 18
#define OLED_RST 21
#define OLED_VEXT 36
#define I2C_NUM I2C_NUM_0

static SSD1306_t dev;
static const char *TAG = "DISPLAY";

bool oled_probe() {
    uint8_t addresses[] = {0x3C, 0x3D};
    for (int i = 0; i < 2; i++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addresses[i] << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_NUM, cmd, 50 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            dev._address = addresses[i];
            ESP_LOGI(TAG, "OLED found at address 0x%02X", addresses[i]);
            return true;
        } else {
            ESP_LOGW(TAG, "OLED probe failed at 0x%02X, error: 0x%X", addresses[i], ret);
        }
    }
    ESP_LOGW(TAG, "OLED not found");
    return false;
}

bool display_init(void) {
    // Turn Vext ON for Heltec V3.x: GPIO36 LOW enables OLED/I2C power
    gpio_config_t vext_conf = {
        .pin_bit_mask = 1ULL << OLED_VEXT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&vext_conf));
    ESP_ERROR_CHECK(gpio_set_level(OLED_VEXT, 0));
    vTaskDelay(pdMS_TO_TICKS(100));

    // Optional: hardware reset pulse on OLED reset line
    gpio_config_t rst_conf = {
        .pin_bit_mask = 1ULL << OLED_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&rst_conf));
    ESP_ERROR_CHECK(gpio_set_level(OLED_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_ERROR_CHECK(gpio_set_level(OLED_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(20));

    // Let the SSD1306 library init I2C without trying to own reset/power
    i2c_master_init(&dev, OLED_SDA, OLED_SCL, -1);
    vTaskDelay(pdMS_TO_TICKS(20));

    if (!oled_probe()) {
        return false;
    }

    ssd1306_init(&dev, 128, 64);
    ssd1306_clear_screen(&dev, false);
    ESP_LOGI(TAG, "Display initialized");
    return true;
}

void display_show_splash(const char *line1, const char *line2) {
    ssd1306_clear_screen(&dev, false);
    ssd1306_display_text(&dev, 2, line1, strlen(line1), false);
    ssd1306_display_text(&dev, 5, line2, strlen(line2), false);
}

void display_show_status(const char *mode, const char *wifi, const char *lora, int queue_depth, const char *cmd, const char *ack, int local_ack_count) {
    char line1[32];
    char line2[32];
    char line3[32];
    char line4[32];
    char line5[32];
    char line6[32];

    static char prev1[32] = "";
    static char prev2[32] = "";
    static char prev3[32] = "";
    static char prev4[32] = "";
    static char prev5[32] = "";
    static char prev6[32] = "";

    snprintf(line1, sizeof(line1), "Salt Robot Base");
    snprintf(line2, sizeof(line2), "Mode: %.20s", mode ? mode : "none");
    snprintf(line3, sizeof(line3), "WiFi: %.20s", wifi ? wifi : "none");
    snprintf(line4, sizeof(line4), "LoRa: %.20s", lora ? lora : "none");
    snprintf(line5, sizeof(line5), "Q:%d Cmd:%.10s", queue_depth, cmd ? cmd : "none");
    snprintf(line6, sizeof(line6), "Ack:%.10s (%d)", ack ? ack : "-", local_ack_count);

    if (strcmp(line1, prev1) != 0) {
        ssd1306_display_text(&dev, 0, "                    ", 20, false);
        ssd1306_display_text(&dev, 0, line1, strlen(line1), false);
        snprintf(prev1, sizeof(prev1), "%s", line1);
    }
    if (strcmp(line2, prev2) != 0) {
        ssd1306_display_text(&dev, 1, "                    ", 20, false);
        ssd1306_display_text(&dev, 1, line2, strlen(line2), false);
        snprintf(prev2, sizeof(prev2), "%s", line2);
    }
    if (strcmp(line3, prev3) != 0) {
        ssd1306_display_text(&dev, 2, "                    ", 20, false);
        ssd1306_display_text(&dev, 2, line3, strlen(line3), false);
        snprintf(prev3, sizeof(prev3), "%s", line3);
    }
    if (strcmp(line4, prev4) != 0) {
        ssd1306_display_text(&dev, 3, "                    ", 20, false);
        ssd1306_display_text(&dev, 3, line4, strlen(line4), false);
        snprintf(prev4, sizeof(prev4), "%s", line4);
    }
    if (strcmp(line5, prev5) != 0) {
        ssd1306_display_text(&dev, 4, "                    ", 20, false);
        ssd1306_display_text(&dev, 4, line5, strlen(line5), false);
        snprintf(prev5, sizeof(prev5), "%s", line5);
    }
    if (strcmp(line6, prev6) != 0) {
        ssd1306_display_text(&dev, 5, "                    ", 20, false);
        ssd1306_display_text(&dev, 5, line6, strlen(line6), false);
        snprintf(prev6, sizeof(prev6), "%s", line6);
    }
}