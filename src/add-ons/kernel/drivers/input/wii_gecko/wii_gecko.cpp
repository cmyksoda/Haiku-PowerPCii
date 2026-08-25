/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <Drivers.h>
#include <KernelExport.h>
#include <string.h>

#include <platform/wii/wii.h>
#include <wii_gecko_input.h>


// Host injected mouse and keyboard events, demultiplexed out of the USB Gecko
// debug console by the platform layer. Emulator-era stand-in for real input.

#define DEVICE_NAME			"input/wii_gecko/0"

// Guest time runs far slower than wall clock under Dolphin, so keep the idle
// poll short enough to stay responsive.
#define POLL_INTERVAL		2000

// A read returns empty rather than blocking forever, so the reader can notice
// that the input server is shutting it down.
#define POLL_TIMEOUT		100000


int32 api_version = B_CUR_DRIVER_API_VERSION;

static int32 sOpenCount = 0;
static bool sClosing = false;


static status_t
wii_gecko_open(const char *name, uint32 flags, void **cookie)
{
	*cookie = NULL;

	if (atomic_add(&sOpenCount, 1) != 0) {
		atomic_add(&sOpenCount, -1);
		return B_BUSY;
	}

	sClosing = false;
	return B_OK;
}


static status_t
wii_gecko_close(void *cookie)
{
	// Releases the reader blocked in wii_gecko_read().
	sClosing = true;
	return B_OK;
}


static status_t
wii_gecko_free(void *cookie)
{
	atomic_add(&sOpenCount, -1);
	return B_OK;
}


static status_t
wii_gecko_control(void *cookie, uint32 op, void *argument, size_t length)
{
	return B_DEV_INVALID_IOCTL;
}


static status_t
wii_gecko_read(void *cookie, off_t pos, void *buffer, size_t *_length)
{
	size_t length = *_length;
	*_length = 0;

	if (buffer == NULL)
		return B_BAD_VALUE;
	if (length < WII_GECKO_INPUT_PACKET_SIZE)
		return B_BUFFER_OVERFLOW;

	uint8 *out = (uint8 *)buffer;
	bigtime_t deadline = system_time() + POLL_TIMEOUT;

	// Wait for the first event, then hand over whatever else is already queued.
	while (*_length + WII_GECKO_INPUT_PACKET_SIZE <= length) {
		wii_gecko_input_packet packet;
		if (!wii_gecko_input_poll(&packet)) {
			if (*_length > 0 || sClosing)
				break;
			if (system_time() >= deadline)
				break;

			snooze(POLL_INTERVAL);
			continue;
		}

		status_t status = user_memcpy(out, &packet,
			WII_GECKO_INPUT_PACKET_SIZE);
		if (status != B_OK)
			return status;

		out += WII_GECKO_INPUT_PACKET_SIZE;
		*_length += WII_GECKO_INPUT_PACKET_SIZE;
	}

	return B_OK;
}


static status_t
wii_gecko_write(void *cookie, off_t pos, const void *buffer, size_t *_length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


//	#pragma mark - driver hooks


status_t
init_hardware(void)
{
	return B_OK;
}


const char **
publish_devices(void)
{
	static const char *devices[] = {
		DEVICE_NAME,
		NULL
	};

	return devices;
}


device_hooks *
find_device(const char *name)
{
	static device_hooks hooks = {
		&wii_gecko_open,
		&wii_gecko_close,
		&wii_gecko_free,
		&wii_gecko_control,
		&wii_gecko_read,
		&wii_gecko_write,
	};

	if (strcmp(name, DEVICE_NAME) == 0)
		return &hooks;

	return NULL;
}


status_t
init_driver(void)
{
	return B_OK;
}


void
uninit_driver(void)
{
}
