#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ws_start(const char* uri);
void ws_send_pcm(const int16_t* samples, int n);
void ws_send_text(const char* text);   // for "start"/"end" control frames
bool ws_is_connected(void);

#ifdef __cplusplus
}
#endif
