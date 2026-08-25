/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "GeckoInputDevice.h"

#include <fcntl.h>
#include <new>
#include <string.h>
#include <unistd.h>

#include <Message.h>
#include <OS.h>
#include <View.h>


// Mouse and keyboard events injected by the host over the USB Gecko serial
// link, so the desktop can be driven while the port has no real input device.

#define POINTER_NAME	"Gecko Bridge Pointer"
#define KEYBOARD_NAME	"Gecko Bridge Keyboard"

// Enough for a burst of queued events without a large stack buffer.
#define READ_PACKETS	16

#define RETRY_INTERVAL	1000000

// Haiku's default. Asking the input server for the real one would mean a
// synchronous round trip to our own host process from the event path.
#define CLICK_SPEED		500000


extern "C" BInputServerDevice*
instantiate_input_device()
{
	return new(std::nothrow) GeckoInputDevice();
}


GeckoInputDevice::GeckoInputDevice()
	:
	fReader(-1),
	fActive(false),
	fRunning(0),
	fButtons(0),
	fLastClick(0),
	fClicks(0),
	fX(-1.0f),
	fY(-1.0f),
	fModifiers(0)
{
	memset(fKeyStates, 0, sizeof(fKeyStates));
}


GeckoInputDevice::~GeckoInputDevice()
{
	if (atomic_get(&fRunning) > 0) {
		atomic_set(&fRunning, 1);
		Stop(NULL, NULL);
	}
}


status_t
GeckoInputDevice::InitCheck()
{
	// Registered unconditionally: the reader waits for the driver's node, which
	// devfs only publishes once something looks it up.
	input_device_ref pointer = { (char*)POINTER_NAME, B_POINTING_DEVICE, NULL };
	input_device_ref keyboard
		= { (char*)KEYBOARD_NAME, B_KEYBOARD_DEVICE, NULL };
	input_device_ref* devices[3] = { &pointer, &keyboard, NULL };

	return RegisterDevices(devices);
}


status_t
GeckoInputDevice::Start(const char* name, void* cookie)
{
	// Both registered devices share one reader.
	if (atomic_add(&fRunning, 1) != 0)
		return B_OK;

	fActive = true;
	fReader = spawn_thread(_ReaderEntry, "gecko input reader",
		B_REAL_TIME_DISPLAY_PRIORITY, this);
	if (fReader < B_OK) {
		fActive = false;
		atomic_add(&fRunning, -1);
		return fReader;
	}

	return resume_thread(fReader);
}


status_t
GeckoInputDevice::Stop(const char* name, void* cookie)
{
	if (atomic_add(&fRunning, -1) != 1)
		return B_OK;

	fActive = false;

	if (fReader >= 0) {
		status_t result;
		wait_for_thread(fReader, &result);
		fReader = -1;
	}

	return B_OK;
}


int32
GeckoInputDevice::_ReaderEntry(void* data)
{
	((GeckoInputDevice*)data)->_Reader();
	return B_OK;
}


void
GeckoInputDevice::_Reader()
{
	wii_gecko_input_packet packets[READ_PACKETS];
	int fd = -1;

	while (fActive) {
		if (fd < 0) {
			fd = open(WII_GECKO_INPUT_DEVICE, O_RDONLY);
			if (fd < 0) {
				snooze(RETRY_INTERVAL);
				continue;
			}
		}

		// The driver returns empty on a quiet link so this loop keeps checking
		// whether the input server has stopped us.
		ssize_t bytesRead = read(fd, packets, sizeof(packets));
		if (bytesRead < 0) {
			close(fd);
			fd = -1;
			snooze(RETRY_INTERVAL);
			continue;
		}

		int32 count = bytesRead / WII_GECKO_INPUT_PACKET_SIZE;
		for (int32 i = 0; i < count; i++) {
			switch (packets[i].type) {
				case WII_GECKO_INPUT_POINTER:
					_HandlePointer(packets[i]);
					break;
				case WII_GECKO_INPUT_KEY:
					_HandleKey(packets[i]);
					break;
			}
		}
	}

	if (fd >= 0)
		close(fd);
}


void
GeckoInputDevice::_HandlePointer(const wii_gecko_input_packet& packet)
{
	bigtime_t now = system_time();

	// The host sends the position normalized against the screen, which is
	// what the input server expects from an absolute pointing device.
	float x = ((packet.data[0] << 8) | packet.data[1]) / 65535.0f;
	float y = ((packet.data[2] << 8) | packet.data[3]) / 65535.0f;
	uint32 buttons = packet.data[4] & (B_PRIMARY_MOUSE_BUTTON
		| B_SECONDARY_MOUSE_BUTTON | B_TERTIARY_MOUSE_BUTTON);
	int8 wheel = (int8)packet.data[5];

	// Button transitions go out before the move, the order a real mouse uses;
	// querying click speed here would block this thread on input_server IPC.
	if (buttons != fButtons) {
		bool pressed = (buttons & ~fButtons) != 0;
		if (pressed) {
			fClicks = (now - fLastClick < CLICK_SPEED) ? fClicks + 1 : 1;
			fLastClick = now;
		}

		status_t status = B_NO_MEMORY;
		BMessage* message = new(std::nothrow) BMessage(pressed
			? B_MOUSE_DOWN : B_MOUSE_UP);
		if (message != NULL) {
			message->AddInt64("when", now);
			message->AddFloat("x", x);
			message->AddFloat("y", y);
			message->AddInt32("buttons", buttons);
			if (pressed)
				message->AddInt32("clicks", fClicks);
			message->AddInt32("be:device_subtype", B_TABLET_POINTING_DEVICE);
			status = EnqueueMessage(message);
			if (status != B_OK)
				delete message;
		}

		// Bring-up trace: shows on the gecko console whether a click got out.
		debug_printf("gecko_input: %s %" B_PRIx32 " at %d/%d clicks %" B_PRId32
			" -> %" B_PRIx32 "\n", pressed ? "down" : "up", buttons,
			(int)(x * 1000), (int)(y * 1000), fClicks, status);

		fButtons = buttons;
	}

	if (x != fX || y != fY) {
		BMessage* message = new(std::nothrow) BMessage(B_MOUSE_MOVED);
		if (message != NULL) {
			message->AddInt64("when", now);
			message->AddFloat("x", x);
			message->AddFloat("y", y);
			message->AddInt32("buttons", buttons);
			message->AddInt32("be:device_subtype", B_TABLET_POINTING_DEVICE);
			if (EnqueueMessage(message) != B_OK)
				delete message;
		}

		fX = x;
		fY = y;
	}

	if (wheel != 0) {
		BMessage* message = new(std::nothrow) BMessage(B_MOUSE_WHEEL_CHANGED);
		if (message != NULL) {
			message->AddInt64("when", now);
			message->AddFloat("be:wheel_delta_x", 0.0f);
			message->AddFloat("be:wheel_delta_y", (float)wheel);
			if (EnqueueMessage(message) != B_OK)
				delete message;
		}
	}
}


void
GeckoInputDevice::_HandleKey(const wii_gecko_input_packet& packet)
{
	// Haiku key codes fit in 7 bits; anything larger would index past the
	// 16-byte states bitfield.
	uint32 key = packet.data[0] & 0x7f;
	bool pressed = packet.data[1] != 0;
	uint32 modifiers = (packet.data[2] << 8) | packet.data[3];
	uint8 character = packet.data[4];
	uint8 rawCharacter = packet.data[5];
	bigtime_t now = system_time();

	if (pressed)
		fKeyStates[key >> 3] |= 0x80 >> (key & 7);
	else
		fKeyStates[key >> 3] &= ~(0x80 >> (key & 7));

	if (modifiers != fModifiers) {
		BMessage* message = new(std::nothrow) BMessage(B_MODIFIERS_CHANGED);
		if (message != NULL) {
			message->AddInt64("when", now);
			message->AddInt32("be:old_modifiers", fModifiers);
			message->AddInt32("modifiers", modifiers);
			message->AddData("states", B_UINT8_TYPE, fKeyStates,
				sizeof(fKeyStates));
			if (EnqueueMessage(message) != B_OK)
				delete message;
		}

		fModifiers = modifiers;
	}

	// A key with no character is unmapped as far as the interface kit goes.
	uint32 what;
	if (character != 0)
		what = pressed ? B_KEY_DOWN : B_KEY_UP;
	else
		what = pressed ? B_UNMAPPED_KEY_DOWN : B_UNMAPPED_KEY_UP;

	BMessage* message = new(std::nothrow) BMessage(what);
	if (message == NULL)
		return;

	message->AddInt64("when", now);
	message->AddInt32("key", key);
	message->AddInt32("modifiers", modifiers);
	message->AddData("states", B_UINT8_TYPE, fKeyStates, sizeof(fKeyStates));

	if (character != 0) {
		char string[2] = { (char)character, '\0' };
		message->AddInt8("byte", (int8)character);
		message->AddData("bytes", B_STRING_TYPE, string, 2);
	}
	if (rawCharacter != 0)
		message->AddInt32("raw_char", rawCharacter & 0x7f);

	if (EnqueueMessage(message) != B_OK)
		delete message;
}
