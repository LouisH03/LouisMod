#ifndef LOUISMOD_RENDERER_H
#define LOUISMOD_RENDERER_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void louismod_renderer_start(HMODULE module);
void louismod_renderer_stop(void);
void louismod_renderer_set_visible(BOOL visible);
uint32_t louismod_renderer_get_instance_count(void);
BOOL louismod_native_replay_button_is_visible(void);

#ifdef __cplusplus
}
#endif

#endif
