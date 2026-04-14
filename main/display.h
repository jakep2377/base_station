#pragma once

#include <stdbool.h>
#include <stdint.h>

bool display_init(void);
void display_show_splash(const char *line1, const char *line2);
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
