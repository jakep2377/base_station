#pragma once
#include <stdbool.h>

bool display_init(void);   
void display_show_status(const char *mode, const char *wifi, const char *lora, int queue_depth, const char *cmd, const char *ack, int local_ack_count);
void display_show_splash(const char *line1, const char *line2);
