//doomgeneric for a bare Linux VirtualTerminal
// Copyright (C) 2025 Techflash
// Based on doomgeneric_sdl.c

#include "doomkeys.h"
#include "m_argv.h"
#include "doomgeneric.h"
#include "i_system.h"

// XXX: HACK
// Linux's input-event-codes.h and doomkeys.h have many collisions.
// Redefine some of doomkeys.h's names here to work around this.
// I could try to redefine Linux's... but that sounds incredibly
// fragile, and is very likely not a good idea.
#undef KEY_TAB
#undef KEY_ENTER
#undef KEY_BACKSPACE
#undef KEY_MINUS
#undef KEY_F1
#undef KEY_F2
#undef KEY_F3
#undef KEY_F4
#undef KEY_F5
#undef KEY_F6
#undef KEY_F7
#undef KEY_F8
#undef KEY_F9
#undef KEY_F10
#undef KEY_F11
#define DOOM_KEY_TAB		9
#define DOOM_KEY_ENTER		13
#define DOOM_KEY_MINUS		0x2d
#define DOOM_KEY_BACKSPACE	0x7f
#define DOOM_KEY_F1		(0x80+0x3b)
#define DOOM_KEY_F2		(0x80+0x3c)
#define DOOM_KEY_F3		(0x80+0x3d)
#define DOOM_KEY_F4		(0x80+0x3e)
#define DOOM_KEY_F5		(0x80+0x3f)
#define DOOM_KEY_F6		(0x80+0x40)
#define DOOM_KEY_F7		(0x80+0x41)
#define DOOM_KEY_F8		(0x80+0x42)
#define DOOM_KEY_F9		(0x80+0x43)
#define DOOM_KEY_F10		(0x80+0x44)
#define DOOM_KEY_F11		(0x80+0x57)
#define DOOM_KEY_F12		(0x80+0x58)


#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdbool.h>

#define KEYQUEUE_SIZE 16

#define MAX_INPUT_DEVS 16
#define INPUT_TYPE_KEYBOARD 0
#define INPUT_TYPE_JOYSTICK 1
#define INPUT_TYPE_MOUSE    2
#define INPUT_TYPE_TOUCH    3

// timing stuff
static struct timeval startTime;

// framebuffer stuff 
static uint8_t *fbPtr;
static int fbFd;
static int ttyFd;
static unsigned int fbWidth, fbHeight, fbStride, fbBytesPerPixel, fbOffsetX, fbOffsetY;

// input stuff
static int numInputFds = 0;
static int inputFds[MAX_INPUT_DEVS];
static bool shiftPressed = false;
static struct pollfd pollfds[MAX_INPUT_DEVS];

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

// XXX: HACK
// Linux's evdev system doesn't make it feasible to just use
// tolower(key) like the existing conversions did, so we
// use a few ranges of maps here to avoid an obscenely long switch/case
static char evdevKeysToASCII1[10] = {
	'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'
};
static char evdevKeysToASCII2[12] = {
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']'
};
static char evdevKeysToASCII3[12] = {
	'a', 's', 'd', 'f', 'g', 'h', 'i', 'j', 'k', 'l', ';', '\''
};
static char evdevKeysToASCII4[11] = {
	'\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/'
};


static char evdevShiftKeysToASCII1[12] = {
	'!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '{', '}'
};
static char evdevShiftKeysToASCII2[12] = {
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '{', '}'
};
static char evdevShiftKeysToASCII3[12] = {
	'a', 's', 'd', 'f', 'g', 'h', 'i', 'j', 'k', 'l', ':', '"'
};
static char evdevShiftKeysToASCII4[11] = {
	'|', 'z', 'x', 'c', 'v', 'b', 'n', 'm', '<', '>', '?'
};


static unsigned char convertToDoomKey(unsigned int key){
	switch (key) {
		case KEY_ENTER:
			key = DOOM_KEY_ENTER;
			break;
		case KEY_ESC:
			key = KEY_ESCAPE;
			break;
		case KEY_LEFT:
			key = KEY_LEFTARROW;
			break;
		case KEY_RIGHT:
			key = KEY_RIGHTARROW;
			break;
		case KEY_UP:
			key = KEY_UPARROW;
			break;
		case KEY_DOWN:
			key = KEY_DOWNARROW;
			break;
		case KEY_LEFTCTRL:
		case KEY_RIGHTCTRL:
			key = KEY_FIRE;
			break;
		case KEY_SPACE:
			key = KEY_USE;
			break;
		case KEY_LEFTSHIFT:
		case KEY_RIGHTSHIFT:
			key = KEY_RSHIFT;
			break;
		case KEY_LEFTALT:
		case KEY_RIGHTALT:
			key = KEY_LALT;
			break;
		case KEY_F2:
			key = DOOM_KEY_F2;
			break;
		case KEY_F3:
			key = DOOM_KEY_F3;
			break;
		case KEY_F4:
			key = DOOM_KEY_F4;
			break;
		case KEY_F5:
			key = DOOM_KEY_F5;
			break;
		case KEY_F6:
			key = DOOM_KEY_F6;
			break;
		case KEY_F7:
			key = DOOM_KEY_F7;
			break;
		case KEY_F8:
			key = DOOM_KEY_F8;
			break;
		case KEY_F9:
			key = DOOM_KEY_F9;
			break;
		case KEY_F10:
			key = DOOM_KEY_F10;
			break;
		case KEY_F11:
			key = DOOM_KEY_F11;
			break;
		case KEY_EQUAL:
			key = KEY_EQUALS;
			break;
		case KEY_MINUS:
			key = DOOM_KEY_MINUS;
			break;
		case KEY_BACKSPACE:
			key = DOOM_KEY_BACKSPACE;
			break;
		case KEY_TAB:
			key = DOOM_KEY_TAB;
			break;

		// sadly, yes, we need to handle every single alphanumeric
		// key here, since evdev doesn't spit out keys in anything
		// remotely resembling ASCII.....
		// though, we can take many shortcuts
		case KEY_1:
		case KEY_2:
		case KEY_3:
		case KEY_4:
		case KEY_5:
		case KEY_6:
		case KEY_7:
		case KEY_8:
		case KEY_9:
		case KEY_0:
			if (shiftPressed)
				key = evdevShiftKeysToASCII1[key - KEY_1];
			else
				key = evdevKeysToASCII1[key - KEY_1];
			break;
		case KEY_Q:
		case KEY_W:
		case KEY_E:
		case KEY_R:
		case KEY_T:
		case KEY_Y:
		case KEY_U:
		case KEY_I:
		case KEY_O:
		case KEY_P:
		case KEY_LEFTBRACE:
		case KEY_RIGHTBRACE:
			if (shiftPressed)
				key = evdevShiftKeysToASCII2[key - KEY_Q];
			else
				key = evdevKeysToASCII2[key - KEY_Q];
			break;
		case KEY_A:
		case KEY_S:
		case KEY_D:
		case KEY_F:
		case KEY_G:
		case KEY_H:
		case KEY_J:
		case KEY_K:
		case KEY_L:
		case KEY_SEMICOLON:
		case KEY_APOSTROPHE:
			if (shiftPressed)
				key = evdevShiftKeysToASCII3[key - KEY_A];
			else
				key = evdevKeysToASCII3[key - KEY_A];
			break;
		case KEY_BACKSLASH:
		case KEY_Z:
		case KEY_X:
		case KEY_C:
		case KEY_V:
		case KEY_B:
		case KEY_N:
		case KEY_M:
		case KEY_COMMA:
		case KEY_DOT:
		case KEY_SLASH:
			if (shiftPressed)
				key = evdevShiftKeysToASCII4[key - KEY_BACKSLASH];
			else
				key = evdevKeysToASCII4[key - KEY_BACKSLASH];
			break;
		default:
			key = 0xFF;
			break;
	}

	return key;
}

static void addKeyToQueue(int pressed, unsigned char keyCode) {
	if ((keyCode == KEY_LEFTSHIFT || keyCode == KEY_RIGHTSHIFT) &&
		(pressed == 1 || pressed == 0))
		shiftPressed = pressed;

		
	unsigned char key = convertToDoomKey(keyCode);
	if (key == 0xFF) // unknown, don't process it
		return;

	if (pressed > 1 || pressed < 0) // bogus value
		return;

	unsigned short keyData = (pressed << 8) | key;

	s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
	s_KeyQueueWriteIndex++;
	s_KeyQueueWriteIndex %= KEYQUEUE_SIZE;
}



static void checkKeys() {
	int ret, i;
	bool keepGoing;
	struct input_event ev;

	keepGoing = true;
	while (keepGoing) {
		keepGoing = false; // exit if we haven't gotten anything
		ret = poll(pollfds, numInputFds, 0);  // don't wait at all, just give anything we've got

		if (ret < 0) {
			// borked
			return;
		}

		for (i = 0; i < MAX_INPUT_DEVS; i++) {
			if (pollfds[i].revents & POLLIN) {
				read(inputFds[i], &ev, sizeof(ev));
				keepGoing = true; // we read something, so keep trying
				addKeyToQueue(ev.value, ev.code);
			}
		}
	}

	// we read the entire backlog, get back to doom
	return;
}


#define TEST_KEY(k) (keybits[(k)/8] & (1 << ((k)%8)))
static int isKeyboard(const char *devPath) {
	unsigned long evbits;
	unsigned char keybits[KEY_MAX/8 + 1];
	int fd = open(devPath, O_RDONLY);
	if (fd < 0) {
		perror("Failed to open device");
		return 0;
	}

	evbits = 0;
	if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0) {
		close(fd);
		return 0;
	}

	/* Must support EV_KEY */
	if (!(evbits & (1 << EV_KEY))) {
		close(fd);
		return 0;
	}


	memset(keybits, 0, sizeof(keybits));
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) {
		close(fd);
		return 0;
	}

	if (TEST_KEY(KEY_A) && TEST_KEY(KEY_ENTER)) {
		close(fd);
		return 1;  /* looks like a keyboard */
	}

	close(fd);
	return 0;
}

static void checkInputDevs() {
	struct dirent *dp;
	DIR *dir;

	/* check for devices */
	dir = opendir("/dev/input");

	while ((dp = readdir(dir)) != NULL) {
		char fullpath[268]; /* d_name is 256 bytes */
		int fd;

		if (numInputFds >= MAX_INPUT_DEVS)
			I_Error("Out of room in input devices array");

		printf("checking %s\n", dp->d_name);
		if (strncmp(dp->d_name, "event", 5) != 0)
			continue;

		sprintf(fullpath, "/dev/input/%s", dp->d_name);

		printf("%s is a valid event device\n", fullpath);
		if (!isKeyboard(fullpath)) {
			printf("%s is not a keyboard, moving on\n", fullpath);
			continue;
		}

		fd = open(fullpath, O_RDONLY | O_NONBLOCK);
		if (fd < 0) {
			// not necessarily fatal
			perror("Failed to open device");
			continue;
		}

		pollfds[numInputFds].fd = fd;
		pollfds[numInputFds].events = POLLIN;
		inputFds[numInputFds++] = fd;
		printf("adding %s keyboard\n", fullpath);
		ioctl(fd, EVIOCGRAB, 1); // grab exclusive access to the device
	}
	closedir(dir);
}


void RestoreVtMode(void)
{
	ioctl(ttyFd, KDSETMODE, KD_TEXT);
	close(ttyFd);
	ttyFd = -1;
}

void signal_handler(int sig)
{
	RestoreVtMode();
	signal(sig, SIG_DFL);
	raise(sig);
}

void DG_Init() {
	int ret;
	struct fb_var_screeninfo info;
	struct fb_fix_screeninfo finfo;

	//
	// set up the framebuffer
	//
	fbFd = open("/dev/fb0", O_RDWR);
	if (fbFd < 0)
		I_Error("Failed to open /dev/fb0: %s", strerror(errno));

	// get info
	ret = ioctl(fbFd, FBIOGET_VSCREENINFO, &info);
	if (ret != 0)
		I_Error("Failed to get framebuffer info: %s", strerror(errno));

	// get other info (this can optionally fail, since we can guess the stride)
	ret = ioctl(fbFd, FBIOGET_FSCREENINFO, &finfo);
	if (ret != 0) {
		printf("Failed to get framebuffer info: %s", strerror(errno));
		fbStride = fbWidth * fbBytesPerPixel;
	}
	else {
		fbStride = finfo.line_length;
	}

	fbWidth = info.xres;
	fbHeight = info.yres;
	fbBytesPerPixel = info.bits_per_pixel / 8;

	// to center the image on screen
	fbOffsetX = ((fbWidth - DOOMGENERIC_RESX) / 2) * fbBytesPerPixel;
	fbOffsetY = ((fbHeight - DOOMGENERIC_RESY) / 2) * fbStride;

	fbPtr = mmap(NULL, fbStride * fbHeight, PROT_READ | PROT_WRITE,
			MAP_SHARED, fbFd, 0);

	if (!fbPtr)
		I_Error("Failed to mmap /dev/fb0: %s", strerror(errno));
	

	ttyFd = open("/dev/tty0", O_RDWR);
	if(ttyFd < 0)
	{
		I_Error("Failed to open TTY device: %s", strerror(errno));
	}
	
	ioctl(ttyFd, KDSETMODE, KD_GRAPHICS); // graphical mode, stops console accessing fb while we use it.
	ioctl(fbFd, FBIOBLANK, FB_BLANK_UNBLANK); // unblank some DRM fbcon drivers will turn off the display.


	// clear the screen
	memset(fbPtr, 0, fbStride * fbHeight);

	//
	// set up input
	//
	checkInputDevs();
	
	if (numInputFds == 0)
		I_Error("Failed to find any compatible input device, see the logs above for potential problems");

	// get the start time
	gettimeofday(&startTime, NULL);
	atexit(RestoreVtMode);
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGSEGV, signal_handler);
	signal(SIGABRT, signal_handler);
}

void DG_DrawFrame() {
	// we need to do it line-by-line like this to account for the
	// fact that the system framebuffer resolution is very likely
	// larger than the doomgeneric render resolution.
	for (int line = 0; line < DOOMGENERIC_RESY; line++) {
		memcpy(
			(void *)((uintptr_t)(fbPtr) + (fbStride * line) + fbOffsetY + fbOffsetX),
				(void *)(((uintptr_t)DG_ScreenBuffer) + (DOOMGENERIC_RESX * line * fbBytesPerPixel)),
			 (fbBytesPerPixel * DOOMGENERIC_RESX)
		);
	}

	checkKeys();
}

void DG_SleepMs(uint32_t ms) {
	usleep(ms * 1000);
}

uint32_t DG_GetTicksMs() {
	struct timeval curTime;
	long seconds, usec;

	gettimeofday(&curTime, NULL);
	seconds = curTime.tv_sec - startTime.tv_sec;
	usec = curTime.tv_usec - startTime.tv_usec;

	return (seconds * 1000) + (usec / 1000);
}

int DG_GetKey(int* pressed, unsigned char* doomKey) {
	checkKeys();

	if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex) {
		//key queue is empty

		return 0;
	}
	else {
		unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
		s_KeyQueueReadIndex++;
		s_KeyQueueReadIndex %= KEYQUEUE_SIZE;

		*pressed = keyData >> 8;
		*doomKey = keyData & 0xFF;

		return 1;
	}

}

void DG_SetWindowTitle(const char * title) {
	printf("Window Title: %s\n", title);
}

int main(int argc, char **argv) {
	doomgeneric_Create(argc, argv);

	while (1)
	{
		doomgeneric_Tick();
	}


	return 0;
}
