#include "display.h"

#include <string.h>
#include <stdio.h>

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OLED_I2C_PORT           I2C_NUM_0
#define OLED_SDA_GPIO           GPIO_NUM_17
#define OLED_SCL_GPIO           GPIO_NUM_18
#define OLED_RST_GPIO           GPIO_NUM_21
#define OLED_I2C_ADDR           0x3C
#define OLED_WIDTH              128
#define OLED_HEIGHT             64
#define OLED_PAGE_COUNT         8
#define OLED_BUFFER_SIZE        (OLED_WIDTH * OLED_PAGE_COUNT)
#define OLED_TAG                "OLED"

static uint8_t s_oled_buffer[OLED_BUFFER_SIZE];
static bool s_display_ready = false;

static const uint8_t font5x7[][5] = {
    [32] = {0x00,0x00,0x00,0x00,0x00},
    [45] = {0x08,0x08,0x08,0x08,0x08},
    [46] = {0x00,0x00,0x00,0x00,0x04},
    [47] = {0x20,0x10,0x08,0x04,0x02},
    [48] = {0x3E,0x51,0x49,0x45,0x3E},
    [49] = {0x00,0x42,0x7F,0x40,0x00},
    [50] = {0x42,0x61,0x51,0x49,0x46},
    [51] = {0x21,0x41,0x45,0x4B,0x31},
    [52] = {0x18,0x14,0x12,0x7F,0x10},
    [53] = {0x27,0x45,0x45,0x45,0x39},
    [54] = {0x3C,0x4A,0x49,0x49,0x30},
    [55] = {0x01,0x71,0x09,0x05,0x03},
    [56] = {0x36,0x49,0x49,0x49,0x36},
    [57] = {0x06,0x49,0x49,0x29,0x1E},
    [58] = {0x00,0x00,0x14,0x00,0x00},
    [65] = {0x7E,0x11,0x11,0x11,0x7E},
    [66] = {0x7F,0x49,0x49,0x49,0x36},
    [67] = {0x3E,0x41,0x41,0x41,0x22},
    [68] = {0x7F,0x41,0x41,0x22,0x1C},
    [69] = {0x7F,0x49,0x49,0x49,0x41},
    [70] = {0x7F,0x09,0x09,0x09,0x01},
    [71] = {0x3E,0x41,0x49,0x49,0x7A},
    [72] = {0x7F,0x08,0x08,0x08,0x7F},
    [73] = {0x00,0x41,0x7F,0x41,0x00},
    [74] = {0x20,0x40,0x41,0x3F,0x01},
    [75] = {0x7F,0x08,0x14,0x22,0x41},
    [76] = {0x7F,0x40,0x40,0x40,0x40},
    [77] = {0x7F,0x02,0x0C,0x02,0x7F},
    [78] = {0x7F,0x04,0x08,0x10,0x7F},
    [79] = {0x3E,0x41,0x41,0x41,0x3E},
    [80] = {0x7F,0x09,0x09,0x09,0x06},
    [81] = {0x3E,0x41,0x51,0x21,0x5E},
    [82] = {0x7F,0x09,0x19,0x29,0x46},
    [83] = {0x46,0x49,0x49,0x49,0x31},
    [84] = {0x01,0x01,0x7F,0x01,0x01},
    [85] = {0x3F,0x40,0x40,0x40,0x3F},
    [86] = {0x1F,0x20,0x40,0x20,0x1F},
    [87] = {0x3F,0x40,0x38,0x40,0x3F},
    [88] = {0x63,0x14,0x08,0x14,0x63},
    [89] = {0x07,0x08,0x70,0x08,0x07},
    [90] = {0x61,0x51,0x49,0x45,0x43}
};

static esp_err_t oled_write_cmd(uint8_t cmd)
{
    uint8_t buffer[2] = {0x00, cmd};
    return i2c_master_write_to_device(OLED_I2C_PORT, OLED_I2C_ADDR, buffer, sizeof(buffer), pdMS_TO_TICKS(100));
}

static esp_err_t oled_write_data(const uint8_t *data, size_t len)
{
    uint8_t buffer[17];
    buffer[0] = 0x40;
    while (len > 0) {
        size_t chunk = len > 16 ? 16 : len;
        memcpy(&buffer[1], data, chunk);
        esp_err_t err = i2c_master_write_to_device(OLED_I2C_PORT, OLED_I2C_ADDR, buffer, chunk + 1, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            return err;
        }
        data += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

static void oled_reset(void)
{
    gpio_reset_pin(OLED_RST_GPIO);
    gpio_set_direction(OLED_RST_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(OLED_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(OLED_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
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
        oled_write_cmd((uint8_t)(0xB0 + page));
        oled_write_cmd(0x00);
        oled_write_cmd(0x10);
        oled_write_data(&s_oled_buffer[page * OLED_WIDTH], OLED_WIDTH);
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
    };

    ESP_ERROR_CHECK(i2c_param_config(OLED_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(OLED_I2C_PORT, conf.mode, 0, 0, 0));

    oled_reset();

    const uint8_t init_cmds[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
        0x2E, 0xAF
    };

    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        if (oled_write_cmd(init_cmds[i]) != ESP_OK) {
            ESP_LOGE(OLED_TAG, "OLED init failed at cmd %u", (unsigned)i);
            s_display_ready = false;
            return false;
        }
    }

    s_display_ready = true;
    oled_clear_buffer();
    oled_flush();
    ESP_LOGI(OLED_TAG, "OLED ready");
    return true;
}

void Display_ShowSplash(const char *title, const char *subtitle)
{
    if (!s_display_ready) {
        return;
    }

    oled_clear_buffer();
    oled_draw_text(0, 0, "SALT ROBOT BASE");
    oled_draw_text(0, 2, title ? title : "BOOTING");
    oled_draw_text(0, 4, subtitle ? subtitle : "PLEASE WAIT");
    oled_flush();
}

void Display_ShowStatus(const char *mode,
                        const char *wifiState,
                        const char *loraState,
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

    snprintf(line, sizeof(line), "MODE:%-.14s", mode ? mode : "-");
    oled_draw_text(0, 1, line);

    snprintf(line, sizeof(line), "WIFI:%-.14s", wifiState ? wifiState : "-");
    oled_draw_text(0, 2, line);

    snprintf(line, sizeof(line), "LORA:%-.14s", loraState ? loraState : "-");
    oled_draw_text(0, 3, line);

    snprintf(line, sizeof(line), "QUEUE:%d ACK:%lu", queueDepth, (unsigned long)ackCount);
    oled_draw_text(0, 4, line);

    snprintf(line, sizeof(line), "CMD:%-.18s", lastCmd ? lastCmd : "-");
    oled_draw_text(0, 5, line);

    snprintf(line, sizeof(line), "ACK:%-.18s", lastAck && lastAck[0] ? lastAck : "-");
    oled_draw_text(0, 6, line);

    oled_flush();
}

