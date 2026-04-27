// display.h — Public API for the SSD1306 OLED status display.
//
// The display module owns all hardware access to the 128×64 OLED panel.
// Callers supply high-level state strings; this module handles compaction,
// line caching, and page-cycling internally so the rest of the firmware
// never touches the SSD1306 driver directly.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Initialize GPIO power-rail and reset lines, probe the I2C bus for an SSD1306
// at address 0x3C or 0x3D, and configure the display for 128×64 operation.
// Returns true on success; if the OLED is absent the caller should skip all
// subsequent display calls rather than crashing.
bool display_init(void);

// Show a two-line splash screen while the system is booting.  Either or both
// strings may be NULL; the function falls back to built-in placeholder text.
// The splash is not cached and does not participate in page cycling.
void display_show_splash(const char *line1, const char *line2);

// Render the full system status across two or three auto-advancing 8-row pages.
//
//   mode          — current operating mode (e.g. "MANUAL", "AUTO").
//   state         — current robot state (e.g. "IDLE", "CMD_QUEUED").
//   wifi          — Wi-Fi link state string (e.g. "online", "connecting").
//   lora          — LoRa link state string (e.g. "online", "degraded").
//   target_network — SSID of the Wi-Fi network the station is joined to (or the
//                    AP SSID when in setup mode).  Up to 32 characters.
//   backend_url   — Full backend URL; displayed as a compacted hostname.
//   queue_depth   — Number of LoRa commands currently waiting in the TX queue.
//   cmd           — Most recent command string sent over LoRa.
//   cmd_status    — Delivery status of the last command ("queued", "sent", etc.)
//   ack           — Most recently received ACK string from the robot.
//   ack_count     — Running count of ACKs received since boot.
//
// All string parameters may be NULL; NULL is treated as empty / "none".
// Long strings are silently truncated to fit the 21-character display width.
// An alert page (page 2) is added automatically whenever any status is not OK.
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
                         uint32_t ack_count);
