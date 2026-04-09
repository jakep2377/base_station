#pragma once
#include <stdbool.h>

<<<<<<< HEAD
bool display_init(void);   
void display_show_status(const char *mode, const char *wifi, const char *lora, int queue_depth, const char *cmd, const char *ack, int local_ack_count);
void display_show_splash(const char *line1, const char *line2);
=======
#ifdef __cplusplus
extern "C" {
#endif

bool Display_Init(void);
void Display_ShowSplash(const char *title, const char *subtitle);
void Display_ShowStatus(const char *mode,
                        const char *wifiState,
                        const char *loraState,
                        const char *targetNetwork,
                        const char *backendUrl,
                        int queueDepth,
                        const char *lastCmd,
                        const char *lastAck,
                        uint32_t ackCount);

#ifdef __cplusplus
}
#endif
>>>>>>> a35027d7654c6a0f56e5bba4a608925e6073f0eb
