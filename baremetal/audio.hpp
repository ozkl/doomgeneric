#pragma once
#include <stdint.h>

enum AudioDriver { AUDIO_NONE, AUDIO_HDA, AUDIO_AC97, AUDIO_SB };

// Detects Intel HD Audio, then AC97, then a Sound Blaster (Pro 2.0 or
// later, on the ISA bus). The boot command line can force one: audio=hda,
// audio=ac97, audio=sb or audio=off; sb=220,1 gives the Sound Blaster's
// port and 8-bit DMA channel (the defaults; BLASTER's A220 D1).
AudioDriver audio_init(const char* cmdline);
uint32_t audio_play_pos();
void audio_submit(const int16_t* samples, int n);   // signed 16-bit mono at audio_rate()
int audio_frames_wanted(int nominal);               // frames to submit for `nominal` frames of game time
uint32_t audio_delay_ms();                          // queued ahead of the speaker right now
uint32_t audio_underruns();                         // times the card ran out of sound since boot
constexpr int audio_rate() { return 48000; }
const char* audio_name();

// The outputs found at boot (speakers/headphones, HDMI/DisplayPort per
// controller, AC'97, Sound Blaster, PC speaker) and switching between them.
// Hold the note player (player_hold) while switching.
int audio_output_count();
int audio_output_current();
const char* audio_output_name(int i);
bool audio_select(int i);
// Volume, 0..100 %, and mute (the PC speaker can only mute).
int audio_volume();                       // what's applied: 0 when muted
int audio_volume_setting();
bool audio_muted();
void audio_set_volume(int v);
void audio_set_muted(bool m);
