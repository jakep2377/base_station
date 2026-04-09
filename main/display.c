#include <stdio.h>
#include <string.h>
#include "display.h"
#include "ssd1306.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"

<<<<<<< HEAD
#define OLED_SDA 17
#define OLED_SCL 18
#define OLED_RST 21
#define OLED_VEXT 36
#define I2C_NUM I2C_NUM_0

static SSD1306_t dev;
static const char *TAG = "DISPLAY";
=======
#define OLED_I2C_PORT           I2C_NUM_0
#define OLED_SDA_GPIO           GPIO_NUM_17
#define OLED_SCL_GPIO           GPIO_NUM_18
#define OLED_RST_GPIO           GPIO_NUM_21
#define OLED_PWR_GPIO           GPIO_NUM_36
#define OLED_I2C_ADDR           0x3C
#define OLED_WIDTH              128
#define OLED_HEIGHT             64
#define OLED_PAGE_COUNT         8
#define OLED_BUFFER_SIZE        (OLED_WIDTH * OLED_PAGE_COUNT)
#define OLED_TAG                "OLED"

static uint8_t s_oled_buffer[OLED_BUFFER_SIZE];
static bool s_display_ready = false;
static bool s_oled_write_error_logged = false;
>>>>>>> a35027d7654c6a0f56e5bba4a608925e6073f0eb

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

<<<<<<< HEAD
bool display_init(void) {
    // Turn Vext ON for Heltec V3.x: GPIO36 LOW enables OLED/I2C power
    gpio_config_t vext_conf = {
        .pin_bit_mask = 1ULL << OLED_VEXT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
=======
static void oled_reset(void)
{
    gpio_reset_pin(OLED_RST_GPIO);
    gpio_set_direction(OLED_RST_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(OLED_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(OLED_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static void oled_enable_power(void)
{
    gpio_reset_pin(OLED_PWR_GPIO);
    gpio_set_direction(OLED_PWR_GPIO, GPIO_MODE_OUTPUT);
    // Heltec V3 boards commonly use LOW to enable Vext/OLED rail.
    gpio_set_level(OLED_PWR_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void oled_clear_buffer(void)
{
    memset(s_oled_buffer, 0, sizeof(s_oled_buffer));
}

static void oled_draw_char(int x, int page, char c)
{
    if (page < 0 || page >= OLED_PAGE_COUNT || x < 0 || x > (OLED_WIDTH - 6)) {
        return;
    }

    unsigned char uc = (unsigned char)c;
    if (uc > 127) {
        uc = '?';
    }

    const uint8_t *glyph = font5x7[uc];
    int offset = page * OLED_WIDTH + x;
    for (int i = 0; i < 5; i++) {
        s_oled_buffer[offset + i] = glyph[i];
    }
    s_oled_buffer[offset + 5] = 0x00;
}

static void oled_draw_text(int x, int page, const char *text)
{
    int cursor = x;
    while (text && *text && cursor <= (OLED_WIDTH - 6)) {
        char c = *text++;
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        oled_draw_char(cursor, page, c);
        cursor += 6;
    }
}

static void oled_flush(void)
{
    if (!s_display_ready) {
        return;
    }

    for (int page = 0; page < OLED_PAGE_COUNT; page++) {
        esp_err_t err = oled_write_cmd((uint8_t)(0xB0 + page));
        if (err == ESP_OK) err = oled_write_cmd(0x00);
        if (err == ESP_OK) err = oled_write_cmd(0x10);
        if (err == ESP_OK) err = oled_write_data(&s_oled_buffer[page * OLED_WIDTH], OLED_WIDTH);
        if (err != ESP_OK && !s_oled_write_error_logged) {
            ESP_LOGE(OLED_TAG, "OLED flush failed on page %d: %s", page, esp_err_to_name(err));
            s_oled_write_error_logged = true;
            return;
        }
    }
}

bool Display_Init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
>>>>>>> a35027d7654c6a0f56e5bba4a608925e6073f0eb
    };
    ESP_ERROR_CHECK(gpio_config(&vext_conf));
    ESP_ERROR_CHECK(gpio_set_level(OLED_VEXT, 0));
    vTaskDelay(pdMS_TO_TICKS(100));

<<<<<<< HEAD
    // Optional: hardware reset pulse on OLED reset line
    gpio_config_t rst_conf = {
        .pin_bit_mask = 1ULL << OLED_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
=======
    ESP_ERROR_CHECK(i2c_param_config(OLED_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(OLED_I2C_PORT, conf.mode, 0, 0, 0));

    oled_enable_power();
    oled_reset();

    const uint8_t init_cmds[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x02, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0xFF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
        0x2E, 0xAF
>>>>>>> a35027d7654c6a0f56e5bba4a608925e6073f0eb
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

<<<<<<< HEAD
    ssd1306_init(&dev, 128, 64);
    ssd1306_clear_screen(&dev, false);
    ESP_LOGI(TAG, "Display initialized");
=======
    s_display_ready = true;
    oled_clear_buffer();
    oled_flush();
    memset(s_oled_buffer, 0xFF, sizeof(s_oled_buffer));
    oled_flush();
    vTaskDelay(pdMS_TO_TICKS(180));
    oled_clear_buffer();
    oled_flush();
    ESP_LOGI(OLED_TAG, "OLED ready");
>>>>>>> a35027d7654c6a0f56e5bba4a608925e6073f0eb
    return true;
}

void display_show_splash(const char *line1, const char *line2) {
    ssd1306_clear_screen(&dev, false);
    ssd1306_display_text(&dev, 2, line1, strlen(line1), false);
    ssd1306_display_text(&dev, 5, line2, strlen(line2), false);
}

<<<<<<< HEAD
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
=======
static void format_mode_label(const char *mode, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    if (!mode || mode[0] == '\0') {
        snprintf(out, out_size, "STATE:-");
        return;
    }
    if (strcmp(mode, "AP") == 0) {
        snprintf(out, out_size, "STATE:SETUP");
    } else if (strcmp(mode, "APSTA") == 0) {
        snprintf(out, out_size, "STATE:JOINING");
    } else if (strcmp(mode, "STA") == 0) {
        snprintf(out, out_size, "STATE:ONLINE");
    } else {
        snprintf(out, out_size, "STATE:%-.12s", mode);
    }
}

static void format_wifi_label(const char *wifiState, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    if (!wifiState || wifiState[0] == '\0') {
        snprintf(out, out_size, "WIFI:-");
        return;
    }
    if (strcmp(wifiState, "setup") == 0) {
        snprintf(out, out_size, "WIFI:READY");
    } else if (strcmp(wifiState, "connecting") == 0) {
        snprintf(out, out_size, "WIFI:JOINING");
    } else if (strcmp(wifiState, "online") == 0) {
        snprintf(out, out_size, "WIFI:OK");
    } else if (strcmp(wifiState, "degraded") == 0) {
        snprintf(out, out_size, "WIFI:FAILED");
    } else if (strcmp(wifiState, "restarting") == 0) {
        snprintf(out, out_size, "WIFI:RESTART");
    } else {
        snprintf(out, out_size, "WIFI:%-.11s", wifiState);
    }
}

static void format_backend_label(const char *backendUrl, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }

    if (!backendUrl || backendUrl[0] == '\0') {
        snprintf(out, out_size, "BACK:-");
        return;
    }

    const char *host = strstr(backendUrl, "://");
    host = host ? host + 3 : backendUrl;
    const char *end = host;
    while (*end && *end != '/' && *end != ':') {
        end++;
    }

    size_t host_len = (size_t)(end - host);
    if (host_len > 13) host_len = 13;
    snprintf(out, out_size, "BACK:%.*s", (int)host_len, host);
}

void Display_ShowStatus(const char *mode,
                        const char *wifiState,
                        const char *loraState,
                        const char *targetNetwork,
                        const char *backendUrl,
                        int queueDepth,
                        const char *lastCmd,
                        const char *lastAck,
                        uint32_t ackCount)
{
    if (!s_display_ready) {
        return;
    }

    char line[24];
    oled_clear_buffer();
    oled_draw_text(0, 0, "BASE STATION");

    format_mode_label(mode, line, sizeof(line));
    oled_draw_text(0, 1, line);

    format_wifi_label(wifiState, line, sizeof(line));
    oled_draw_text(0, 2, line);

    snprintf(line, sizeof(line), "NET:%-.14s", targetNetwork && targetNetwork[0] ? targetNetwork : "-");
    oled_draw_text(0, 3, line);

    format_backend_label(backendUrl, line, sizeof(line));
    oled_draw_text(0, 4, line);

    snprintf(line, sizeof(line), "L:%-.6s Q:%d", loraState ? loraState : "-", queueDepth);
    oled_draw_text(0, 5, line);

    snprintf(line, sizeof(line), "CMD:%-.18s", lastCmd ? lastCmd : "-");
    oled_draw_text(0, 6, line);

    snprintf(line, sizeof(line), "ACK:%lu %-.12s", (unsigned long)ackCount, lastAck && lastAck[0] ? lastAck : "-");
    oled_draw_text(0, 7, line);

    oled_flush();
}
>>>>>>> a35027d7654c6a0f56e5bba4a608925e6073f0eb
