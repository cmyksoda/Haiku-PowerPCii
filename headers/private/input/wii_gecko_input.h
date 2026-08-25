/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _WII_GECKO_INPUT_H
#define _WII_GECKO_INPUT_H

#include <SupportDefs.h>


// Host injected input framed onto the USB Gecko console: ESC 'I' <type>
// <6 payload bytes> <checksum>, ten bytes. ESC 'I' never occurs in KDL text
// and the checksum lets the kernel resynchronize on plain console output.

#define WII_GECKO_INPUT_DEVICE		"/dev/input/wii_gecko/0"

#define WII_GECKO_FRAME_ESCAPE		0x1b
#define WII_GECKO_FRAME_TAG			'I'
#define WII_GECKO_FRAME_SIZE		10
#define WII_GECKO_CHECKSUM_SEED		0x5a

#define WII_GECKO_INPUT_NOP			0	// resynchronizes a confused decoder
#define WII_GECKO_INPUT_POINTER		1
#define WII_GECKO_INPUT_KEY			2

// Pointer buttons, matching B_PRIMARY_MOUSE_BUTTON and friends.
#define WII_GECKO_BUTTON_PRIMARY	0x01
#define WII_GECKO_BUTTON_SECONDARY	0x02
#define WII_GECKO_BUTTON_TERTIARY	0x04

typedef struct wii_gecko_input_packet {
	uint8	type;
	uint8	data[6];
} wii_gecko_input_packet;

#define WII_GECKO_INPUT_PACKET_SIZE	7

/*	Payloads, multi byte fields big endian. POINTER: data[0..1] x and
	data[2..3] y normalized 0..65535 across the screen (the host never needs
	the guest resolution), data[4] button mask, data[5] signed wheel delta.
	KEY: data[0] Haiku key code, data[1] 1 while pressed, data[2..3] modifier
	mask, data[4] produced character or 0, data[5] unmodified "raw_char".
*/

#endif	// _WII_GECKO_INPUT_H
