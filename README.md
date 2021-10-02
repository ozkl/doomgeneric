# doomreplay

Replay Doom by providing keyboard input from a file and optionally storing the replay as a video file

Forked from: https://github.com/ozkl/doomgeneric

## Build

```bash
cd doomgeneric
make -f Makefile.dr
```

## Notes

The ability to read a text file with keyboard input has been added. To achieve this, several parts in the
Doom source code have been modified - changes are enabled via the `DOOMREPLAY` macro. The modifications
are mainly to introduce an artificial timer in order to make the game not wait for new ticks.

The keyboard input is then pushed to the game as if the player had pressed the keys. The replay can be
optionally recorded to an `.mp4` file using `ffmpeg`.

When not recoring frames, the simulation can reach up to several thousand game frames per second - i.e. it
is really fast! This allows to simulate huge amounts of input. The game is fully deterministic, so every
time the result will be identical.

## Input

```
,   - new frame
x   - escape
e   - enter
l   - left
r   - right
u   - up
d   - down
a   - alt
s   - shift
p   - use
f   - fire
t   - tab
y   - yes
n   - no
<   - strafe left
>   - strafe right
0-9 - weapons
```

## Examples

```bash
# Start a new game and complete level 1
./doomgeneric -iwad doom1.wad -input ../input/solve1.txt -nrecord 1200 -framerate 35 -render_frame -render_input -render_username -output replay.mp4
vlc replay.mp4
```
