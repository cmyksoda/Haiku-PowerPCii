/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef GECKO_INPUT_DEVICE_H
#define GECKO_INPUT_DEVICE_H


#include <InputServerDevice.h>
#include <InterfaceDefs.h>

#include <wii_gecko_input.h>


class GeckoInputDevice : public BInputServerDevice {
public:
							GeckoInputDevice();
	virtual					~GeckoInputDevice();

	virtual status_t		InitCheck();

	virtual status_t		Start(const char* name, void* cookie);
	virtual status_t		Stop(const char* name, void* cookie);

private:
	static	int32			_ReaderEntry(void* data);
			void			_Reader();

			void			_HandlePointer(const wii_gecko_input_packet& packet);
			void			_HandleKey(const wii_gecko_input_packet& packet);

			thread_id		fReader;
			volatile bool	fActive;
			int32			fRunning;

			uint32			fButtons;
			bigtime_t		fLastClick;
			int32			fClicks;
			float			fX;
			float			fY;

			uint32			fModifiers;
			uint8			fKeyStates[16];
};


extern "C" BInputServerDevice* instantiate_input_device();


#endif	// GECKO_INPUT_DEVICE_H
