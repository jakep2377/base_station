#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool Display_Init(void);
void Display_ShowSplash(const char *title, const char *subtitle);
void Display_ShowStatus(const char *mode,
                        const char *wifiState,
                        const char *loraState,
                        int queueDepth,
                        const char *lastCmd,
                        const char *lastAck,
                        uint32_t ackCount);

#ifdef __cplusplus
}
#endif
