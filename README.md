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

### Complete level 1

```bash  
./doomgeneric -iwad doom1.wad -input ../input/solve1.txt -nrecord 3500 -framerate 35 -render_frame -render_input -render_username -output solve1.mp4
vlc solve1.mp4
```

https://user-images.githubusercontent.com/1991296/135767411-da6e3d65-afcd-400c-bd25-9c21ae0057d4.mp4

### Turn 360

```bash
./doomgeneric -iwad doom1.wad -input ../input/turn.txt -nrecord 3500 -framerate 35 -render_frame -render_input -render_username -output turn.mp4
vlc turn.mp4
```

https://user-images.githubusercontent.com/1991296/135767466-0c798f42-5aeb-4fa8-b687-3d4544013c97.mp4

### Straferun SR40

```bash
./doomgeneric -iwad doom1.wad -input ../input/sr40.txt -nrecord 3500 -framerate 35 -render_frame -render_input -render_username -output sr40.mp4
vlc sr40.mp4
```

https://user-images.githubusercontent.com/1991296/135767531-fc5fca69-3967-4f81-bd6e-8eda45e34560.mp4

### Straferun SR50

```bash
./doomgeneric -iwad doom1.wad -input ../input/sr50.txt -nrecord 3500 -framerate 35 -render_frame -render_input -render_username -output sr50.mp4
vlc sr50.mp4
```

https://user-images.githubusercontent.com/1991296/135767533-39c34be2-0677-46e3-8463-36fef38ce134.mp4
