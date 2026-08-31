/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef WII_CONSOLE_H
#define WII_CONSOLE_H


#include <SupportDefs.h>

#include <gccore.h>


void ctype_init(void);
void video_init(void);
void video_apply_settings(void);
GXRModeObj* video_mode(void);
void* video_frame_buffer(void);


#endif	// WII_CONSOLE_H
