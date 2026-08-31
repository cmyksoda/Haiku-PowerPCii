/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <boot/platform.h>
#include <boot/platform/generic/text_console.h>
#include <boot/stdio.h>
#include <boot/stage2.h>
#include <boot/menu.h>
#include <drivers/driver_settings.h>
#include <string.h>

#include <gccore.h>
#include <wiiuse/wpad.h>
#include <wiikeyboard/keyboard.h>

#include "console.h"
#include "debug.h"

// not declared by any libogc header, but exported by the library
extern "C" void udelay(int microSeconds);
extern "C" void* SYS_AllocArena2MemLo(u32 size, u32 align);

FILE *stdin, *stdout, *stderr;

static GXRModeObj *sVideoMode;
static GXRModeObj sOverscanMode;
static void *sFrameBuffer;

static const int kMaxOverscan = 48;
	// fbWidth has to stay a multiple of 16, and viYOrigin/viHeight even
static const int kOverscanGranularityX = 8;
static const int kOverscanGranularityY = 2;


class Console : public ConsoleNode {
	public:
		Console() : ConsoleNode() {}

		virtual ssize_t ReadAt(void *cookie, off_t pos, void *buffer,
			size_t bufferSize);
		virtual ssize_t WriteAt(void *cookie, off_t pos, const void *buffer,
			size_t bufferSize);
		virtual void ClearScreen();
		virtual int32 Width();
		virtual int32 Height();
		virtual void SetCursor(int32 x, int32 y);
		virtual void SetCursorVisible(bool visible) {}
		virtual void SetColors(int32 foreground, int32 background);
};

static Console sConsole;


ssize_t
Console::ReadAt(void *cookie, off_t pos, void *buffer, size_t bufferSize)
{
	char *string = (char *)buffer;

	for (size_t i = 0; i < bufferSize; i++)
		string[i] = console_wait_for_key();

	return bufferSize;
}


ssize_t
Console::WriteAt(void *cookie, off_t pos, const void *buffer, size_t bufferSize)
{
	debug_write((const char *)buffer, bufferSize);
	return bufferSize;
}


void
Console::ClearScreen()
{
	console_clear_screen();
}


int32
Console::Width()
{
	return console_width();
}


int32
Console::Height()
{
	return console_height();
}


void
Console::SetCursor(int32 x, int32 y)
{
	console_set_cursor(x, y);
}


void
Console::SetColors(int32 foreground, int32 background)
{
	console_set_color(foreground, background);
}


//	#pragma mark - video


void
video_init(void)
{
	VIDEO_Init();

	sVideoMode = VIDEO_GetPreferredMode(NULL);
	sFrameBuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(sVideoMode));

	VIDEO_ClearFrameBuffer(sVideoMode, sFrameBuffer, COLOR_BLACK);
	CON_Init(sFrameBuffer, 20, 20, sVideoMode->fbWidth, sVideoMode->xfbHeight,
		sVideoMode->fbWidth * VI_DISPLAY_PIX_SZ);

	VIDEO_Configure(sVideoMode);
	VIDEO_SetNextFramebuffer(sFrameBuffer);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if ((sVideoMode->viTVMode & VI_NON_INTERLACE) != 0)
		VIDEO_WaitVSync();
}


GXRModeObj*
video_mode(void)
{
	return sVideoMode;
}


void*
video_frame_buffer(void)
{
	return sFrameBuffer;
}


//! The loader stubs out strtoul(), so the decimal is parsed by hand here.
static int
overscan_parameter(void *settings, const char *key, int granularity)
{
	const char *value = get_driver_parameter(settings, key, "0", "0");
	if (value == NULL)
		return 0;

	int pixels = 0;
	for (; *value >= '0' && *value <= '9'; value++)
		pixels = pixels * 10 + (*value - '0');

	if (pixels > kMaxOverscan)
		pixels = kMaxOverscan;

	return pixels - pixels % granularity;
}


/*!	Insets the picture so a CRT's overscan cannot swallow the Deskbar. The VI
	is programmed once and the kernel never touches it again, so this is the
	last chance to change the geometry the desktop is built for.
*/
void
video_apply_overscan(void)
{
	if (sVideoMode == NULL || !gKernelArgs.frame_buffer.enabled)
		return;

	void *settings = load_driver_settings("wii_video");
	if (settings == NULL)
		return;

	int insetX = overscan_parameter(settings, "overscan_x",
		kOverscanGranularityX);
	int insetY = overscan_parameter(settings, "overscan_y",
		kOverscanGranularityY);
	unload_driver_settings(settings);

	if (insetX == 0 && insetY == 0)
		return;

	sOverscanMode = *sVideoMode;
	sOverscanMode.fbWidth -= 2 * insetX;
	sOverscanMode.viWidth -= 2 * insetX;
	sOverscanMode.viXOrigin += insetX;
	sOverscanMode.xfbHeight -= 2 * insetY;
	sOverscanMode.viHeight -= 2 * insetY;
	sOverscanMode.viYOrigin += insetY;
	sVideoMode = &sOverscanMode;

	VIDEO_ClearFrameBuffer(sVideoMode, sFrameBuffer, COLOR_BLACK);
	VIDEO_Configure(sVideoMode);
	VIDEO_SetNextFramebuffer(sFrameBuffer);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if ((sVideoMode->viTVMode & VI_NON_INTERLACE) != 0)
		VIDEO_WaitVSync();

	// the console's stride came from the old mode, so re-lay it out
	CON_Init(sFrameBuffer, 20, 20, sVideoMode->fbWidth, sVideoMode->xfbHeight,
		sVideoMode->fbWidth * VI_DISPLAY_PIX_SZ);

	uint32 width = sVideoMode->fbWidth;
	uint32 height = sVideoMode->xfbHeight;

	gKernelArgs.frame_buffer.width = width;
	gKernelArgs.frame_buffer.height = height;
	gKernelArgs.frame_buffer.bytes_per_row = width * 4;
	gKernelArgs.frame_buffer.physical_buffer.size = width * height * 4;
	gKernelArgs.arch_args.wii_hardware_framebuffer.size
		= width * height * VI_DISPLAY_PIX_SZ;

	dprintf("frame buffer overscan: %d/%d per side, frame buffer: %" B_PRIu32
		"x%" B_PRIu32 "\n", insetX, insetY, width, height);
}


//! The VI only scans out YUY2, so app_server draws into this shadow buffer
//! and the kernel driver converts.
status_t
platform_init_video(void)
{
	stdout = stderr = (FILE *)&sConsole;

	if (sVideoMode == NULL)
		return B_NO_INIT;

	uint32 width = sVideoMode->fbWidth;
	uint32 height = sVideoMode->xfbHeight;
	size_t size = width * height * 4;

	void *shadow = NULL;
	if (platform_allocate_region(&shadow, size, B_READ_AREA | B_WRITE_AREA)
			!= B_OK) {
		dprintf("failed to allocate the %" B_PRIuSIZE " byte frame buffer\n",
			size);
		return B_NO_MEMORY;
	}
	memset(shadow, 0, size);

	gKernelArgs.frame_buffer.enabled = true;
	gKernelArgs.frame_buffer.physical_buffer.start
		= (addr_t)MEM_VIRTUAL_TO_PHYSICAL(shadow);
	gKernelArgs.frame_buffer.physical_buffer.size = size;
	gKernelArgs.frame_buffer.width = width;
	gKernelArgs.frame_buffer.height = height;
	gKernelArgs.frame_buffer.depth = 32;
	gKernelArgs.frame_buffer.bytes_per_row = width * 4;

	gKernelArgs.arch_args.wii_hardware_framebuffer.start
		= (addr_t)MEM_VIRTUAL_TO_PHYSICAL(sFrameBuffer);
	gKernelArgs.arch_args.wii_hardware_framebuffer.size
		= width * height * VI_DISPLAY_PIX_SZ;

	gKernelArgs.arch_args.wii_aspect = CONF_GetAspectRatio() > 0 ? 1 : 0;

	dprintf("frame buffer: %" B_PRIu32 "x%" B_PRIu32 ", shadow at %p, "
		"scanout at %p\n", width, height,
		(void *)(addr_t)gKernelArgs.frame_buffer.physical_buffer.start,
		(void *)(addr_t)gKernelArgs.arch_args.wii_hardware_framebuffer.start);
	dprintf("video mode: tv 0x%" B_PRIx32 " fb %ux%u vi %ux%u origin %u/%u "
		"xfbMode %" B_PRIu32 " aa %u\n", sVideoMode->viTVMode,
		sVideoMode->fbWidth, sVideoMode->xfbHeight, sVideoMode->viWidth,
		sVideoMode->viHeight, sVideoMode->viXOrigin, sVideoMode->viYOrigin,
		sVideoMode->xfbMode, sVideoMode->aa);
	dprintf("video conf: video %d aspect %d progressive %d eurgb60 %d "
		"component %d\n", CONF_GetVideo(), CONF_GetAspectRatio(),
		CONF_GetProgressiveScan(), CONF_GetEuRGB60(),
		VIDEO_HaveComponentCable());

	return B_OK;
}


//	#pragma mark - console


extern "C" void
console_clear_screen(void)
{
	debug_write("\x1b[2J", 4);
}


extern "C" int32
console_width(void)
{
	int columns = 80, rows = 25;
	CON_GetMetrics(&columns, &rows);
	return columns;
}


extern "C" int32
console_height(void)
{
	int columns = 80, rows = 25;
	CON_GetMetrics(&columns, &rows);
	return rows;
}


extern "C" void
console_set_cursor(int32 x, int32 y)
{
	char buffer[16];
	int length = snprintf(buffer, sizeof(buffer), "\x1b[%" B_PRId32 ";%"
		B_PRId32 "H", y + 1, x + 1);
	debug_write(buffer, length);
}


extern "C" void
console_show_cursor(void)
{
}


extern "C" void
console_hide_cursor(void)
{
}


extern "C" void
console_set_color(int32 foreground, int32 background)
{
	char buffer[24];
	int length = snprintf(buffer, sizeof(buffer), "\x1b[%" B_PRId32 ";%"
		B_PRId32 "m", 30 + (foreground & 7), 40 + (background & 7));
	debug_write(buffer, length);
}


//! Menu-only need, and KEYBOARD_Init() never returns under Dolphin MMU=True.
static void
console_init_input(void)
{
	static bool sInputInitialized = false;
	if (sInputInitialized)
		return;
	sInputInitialized = true;

	WPAD_Init();
	if (KEYBOARD_Init(NULL) != 0)
		dprintf("no USB keyboard found\n");
}


extern "C" int
console_wait_for_key(void)
{
	console_init_input();

	while (true) {
		WPAD_ScanPads();
		u32 pressed = WPAD_ButtonsDown(0);
		if ((pressed & WPAD_BUTTON_UP) != 0)
			return TEXT_CONSOLE_KEY_UP;
		if ((pressed & WPAD_BUTTON_DOWN) != 0)
			return TEXT_CONSOLE_KEY_DOWN;
		if ((pressed & WPAD_BUTTON_A) != 0)
			return TEXT_CONSOLE_KEY_RETURN;
		if ((pressed & WPAD_BUTTON_B) != 0)
			return TEXT_CONSOLE_KEY_ESCAPE;
		if ((pressed & WPAD_BUTTON_HOME) != 0)
			return TEXT_CONSOLE_KEY_ESCAPE;

		keyboard_event event;
		if (KEYBOARD_GetEvent(&event) != 0
			&& event.type == KEYBOARD_PRESSED) {
			switch (event.keycode) {
				case 103:
					return TEXT_CONSOLE_KEY_UP;
				case 108:
					return TEXT_CONSOLE_KEY_DOWN;
				case 105:
					return TEXT_CONSOLE_KEY_LEFT;
				case 106:
					return TEXT_CONSOLE_KEY_RIGHT;
				case 28:
					return TEXT_CONSOLE_KEY_RETURN;
				case 1:
					return TEXT_CONSOLE_KEY_ESCAPE;
				default:
					if (event.symbol > 0 && event.symbol < 128)
						return event.symbol;
					break;
			}
		}

		VIDEO_WaitVSync();
	}

	return 0;
}


extern "C" void
console_put_char(char c)
{
	debug_write(&c, 1);
}


void
platform_switch_to_logo(void)
{
}


void
platform_switch_to_text_mode(void)
{
}


void
platform_cleanup_devices()
{
}


void
platform_run_menu(Menu *menu)
{
}


void
platform_update_menu_item(Menu *menu, MenuItem *item)
{
}


size_t
platform_get_user_input_text(Menu *menu, MenuItem *item, char *buffer,
	size_t bufferSize)
{
	return 0;
}


void
platform_add_menus(Menu *menu)
{
}


void
platform_debug_syslog_early(const char *buffer, size_t length)
{
}


void
platform_load_ucode(BootVolume &volume)
{
}


//	#pragma mark - libogc support
//	The newlib and libsysbase pieces libogc actually reaches.


extern "C" {
	float sqrtf(float x) { return 0.0f; }
	float atan2f(float y, float x) { return 0.0f; }
	void sincosf(float x, float *s, float *c) { *s = 0.0f; *c = 1.0f; }
	float atanf(float x) { return 0.0f; }
	float cosf(float x) { return 1.0f; }
	float hypotf(float x, float y) { return 0.0f; }
	void sincos(double x, double *s, double *c) { *s = 0.0; *c = 1.0; }
	double atan2(double y, double x) { return 0.0; }

	void* __sf[3];
	void* devoptab_list[8];
	int __errno;
	unsigned char _ctype_[257];

	int setvbuf(FILE *file, char *buff, int mode, size_t size) { return 0; }
	void* FindDevice(const char*) { return NULL; }
	int AddDevice(void*) { return -1; }
	int RemoveDevice(const char*) { return -1; }
	void build_argv() {}
	void __init() {}
	void _free_r(void*, void*) {}
	unsigned long _strtoul_r(void*, const char*, char**, int) { return 0; }
	unsigned long long _strtoull_r(void*, const char*, char**, int) { return 0; }


	char*
	strncpy(char* dest, const char* src, size_t n)
	{
		size_t i = 0;
		for (; i < n && src[i] != '\0'; i++)
			dest[i] = src[i];
		for (; i < n; i++)
			dest[i] = '\0';
		return dest;
	}


	int
	usleep(unsigned int usec)
	{
		udelay(usec);
		return 0;
	}


	//! IOS wants 32 byte aligned MEM2 buffers, long before the loader heap exists.
	void*
	memalign(size_t alignment, size_t size)
	{
		if (alignment < 32)
			alignment = 32;
		return SYS_AllocArena2MemLo(size, alignment);
	}


	void*
	_memalign_r(void*, size_t alignment, size_t size)
	{
		return memalign(alignment, size);
	}
}


//! newlib's ctype table, which libogc's IPC and keyboard code index directly.
void
ctype_init(void)
{
	const unsigned char kUpper = 0x01, kLower = 0x02, kDigit = 0x04,
		kSpace = 0x08, kPunct = 0x10, kControl = 0x20, kHex = 0x40,
		kBlank = 0x80;

	unsigned char *table = _ctype_ + 1;
	memset(_ctype_, 0, sizeof(_ctype_));

	for (int c = 0; c < 128; c++) {
		unsigned char value = 0;
		if (c >= 'A' && c <= 'Z')
			value |= kUpper;
		if (c >= 'a' && c <= 'z')
			value |= kLower;
		if (c >= '0' && c <= '9')
			value |= kDigit | kHex;
		if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
			value |= kHex;
		if (c == ' ' || (c >= '\t' && c <= '\r'))
			value |= kSpace;
		if (c == ' ' || c == '\t')
			value |= kBlank;
		if (c < 0x20 || c == 0x7f)
			value |= kControl;
		else if (value == 0 && c != ' ')
			value |= kPunct;

		table[c] = value;
	}
}
