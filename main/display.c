// display.c — SSD1306 OLED driver and status-page rendering for the base station.
//
// Responsibilities:
//   • Hardware init: GPIO power-rail (VEXT) and reset sequencing, I2C bring-up.
//   • OLED address probing (0x3C / 0x3D) to tolerate board variants.
//   • Line-level write caching: only rows whose content has changed are sent to
//     the driver, avoiding full-screen flicker on every update cycle.
//   • Page cycling: the status display cycles through two or three pages on a
//     fixed timer interval; an alert page is added whenever any link is not OK.
//   • Label compaction: long strings (URLs, state names, token payloads) are
//     trimmed to fit the 21-character display width before rendering.

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

// GPIO pin assignments for the Heltec WiFi LoRa 32 (V3) OLED circuit.
#define OLED_SDA 17
#define OLED_SCL 18
#define OLED_RST 21
#define OLED_VEXT 36           // Active-low external power rail for the panel.
#define OLED_I2C_PORT I2C_NUM_0

// The SSD1306 is 128 pixels wide; each 6-pixel-wide character gives 21 columns.
#define OLED_TEXT_LINE_LEN 21

// How long each page is displayed before advancing to the next (ms).
// 7 s gives operators time to read a full page before it cycles.
#define OLED_PAGE_SWITCH_MS 7000

static SSD1306_t dev;
static const char *TAG = "DISPLAY";

// Write a single row to the OLED, optionally with inverted colours.
// The row content is compared against `cache` before issuing any SPI/I2C
// traffic: if nothing changed and invert is off the write is skipped entirely,
// which prevents per-row flicker when only a few values update between calls.
// Inverted rows (alert page) are always redrawn so the highlight stays visible.
static void write_line_ex(int row, const char *text, char *cache, size_t cache_size, bool invert) {
    char buffer[OLED_TEXT_LINE_LEN + 1];
    snprintf(buffer, sizeof(buffer), "%-21.21s", text ? text : "");
    if (!invert && strncmp(buffer, cache, cache_size) == 0) {
        return;
    }

    ssd1306_display_text(&dev, row, buffer, strlen(buffer), invert);

    snprintf(cache, cache_size, "%s", buffer);
}

// Convenience wrapper for the common non-inverted case.
static void write_line(int row, const char *text, char *cache, size_t cache_size) {
    write_line_ex(row, text, cache, cache_size, false);
}

// Reduce a full backend URL to a short hostname label that fits `out_size`.
// Strategy:
//   1. Strip the scheme ("http://", "https://") and any trailing path.
//   2. If the bare hostname still exceeds the buffer, walk backward through
//      dots and keep only the last two labels (e.g. "api.example.com" → "example.com").
//   3. Return "none" for a NULL, empty, or scheme-only URL.
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

// Skip optional "CMD:" or "ACK:" prefix and leading whitespace so that raw
// protocol tokens (e.g. "CMD: FORWARD") surface only the payload portion.
// Returns a pointer into `input`; never returns NULL (falls back to "none").
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

// Extract the first space- or comma-delimited token from a (possibly prefixed)
// input string so that multi-field payloads like "ACK: FORWARD,seq=3" collapse
// to just "FORWARD" for display.  Writes "none" if nothing usable is found.
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

// Copy a width-limited substring of `input` starting at `offset` characters
// from the beginning.  When the string fits within `out_size - 1` characters
// it is copied in full; otherwise a sliding window of exactly that width is
// extracted.  Used to pan long strings (SSIDs, hostnames) across the 21-char
// display without truncating silently.
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

// Normalize an arbitrary state/link string to a short uppercase label that
// fits in the tight display columns.  Recognizes common "good" values
// (connected, online, ready, ok, committed) → "OK"; setup/config variants
// → "SETUP"; degraded/stale/warn variants → "WARN"; off/disconnected → "OFF";
// anything else is uppercased and passed through as-is (up to out_size - 1 chars).
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

// Build a single-line alert summary in priority order:
//   1. Wi-Fi not OK  →  "WIFI <label>"
//   2. LoRa not OK   →  "RADIO <label>"
//   3. Non-empty TX queue  →  "QUEUE <depth>"
//   4. Command status not idle/ok  →  "CMD <status>"
//   5. Fallback  →  "CHECK LINKS"
// This is displayed as the second inverted row on the alert page.
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

// Probe both common SSD1306 I2C addresses (0x3C and 0x3D) and record
// whichever responds so the rest of the driver uses the correct address.
// Returns false without configuring `dev._address` if neither address ACKs.
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

// Bring up the OLED hardware in the correct sequence:
//   1. Assert VEXT low to enable the panel power rail; wait 100 ms for rail to stabilize.
//   2. Toggle RST low→high to reset the controller; 20 ms each edge.
//   3. Initialize the I2C master and probe for the SSD1306 address.
//   4. Configure the display for 128×64 and run a brief self-test pattern.
// Returns false (without crashing) if the probe fails so the caller can
// choose to run headless.
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

// Display a fixed-layout boot splash.  Uses absolute row addresses (not the
// caching layer) so it renders correctly on a freshly cleared screen even
// before display_show_status has been called for the first time.
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
    // Per-row line cache: avoids sending unchanged rows to the SSD1306 driver.
    // Indexed by row number (0-7); cleared to zero on every page transition so
    // rows from the previous page don't bleed into the new layout.
    static char cache[8][OLED_TEXT_LINE_LEN + 1] = {{0}};
    // Track which page was rendered last so we can detect page transitions and
    // invalidate the cache when the layout changes entirely.
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

    // Determine whether any subsystem needs attention.  If so, add a third
    // page (page 2) that renders entirely in inverted video as a visual alarm.
    // cmd_status values "OK" and "IDLE" are treated as non-alerting so routine
    // idle state doesn't permanently enable the alert page.
    bool show_alert = (strcmp(wifi_label, "OK") != 0) ||
                      (strcmp(lora_label, "OK") != 0) ||
                      (queue_depth > 0) ||
                      (strcmp(cmd_status_label, "OK") != 0 && strcmp(cmd_status_label, "IDLE") != 0);
    int page_count = show_alert ? 3 : 2;
    // Derive the current page from the tick count so page advances are
    // time-driven and don't require any explicit state machine.
    int page = (int)((now / pdMS_TO_TICKS(OLED_PAGE_SWITCH_MS)) % page_count);

    // On a page boundary, wipe the cache and clear the physical screen so
    // content from the old layout doesn't linger behind the new one.
    if (page != last_page) {
        memset(cache, 0, sizeof(cache));
        ssd1306_clear_screen(&dev, false);
        last_page = page;
    }

    if (page == 0) {
        // Page 0 — overview: mode, state, link health, SSID, and API host.
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
        // Page 1 — detail: last command, delivery status, ACK, and queue depth.
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
        // Page 2 (alert) — all rows rendered inverted so the operator sees an
        // immediate visual alarm.  The headline row summarises the highest-priority
        // problem; subsequent rows expose the raw values for diagnosis.
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
