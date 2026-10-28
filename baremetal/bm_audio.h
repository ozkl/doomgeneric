/* The sound cards the kernel found (kernel.cpp), for the sound-card item
   in Doom's Sound Volume menu (m_menu.c, built with -DBAREMETAL). */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int bm_audio_count(void);               /* outputs found at boot, the last "off" */
int bm_audio_current(void);
const char* bm_audio_label(int i);      /* short, for the menu */
int bm_audio_select(int i);             /* switch now; 0 if it wouldn't start */
#ifdef __cplusplus
}
#endif
