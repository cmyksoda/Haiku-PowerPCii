/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <platform/wii/wii.h>

#include <KernelExport.h>
#include <arch/cpu.h>
#include <debug.h>

#include <string.h>

#include <wii_gecko_input.h>


// EXI channel 0 device 1 is the RTC/SRAM/UART chip. The RTC is a free-running
// 32 bit second counter zeroed at 2000-01-01; user-visible time is that
// counter plus the bias word kept in SRAM, so we read the bias once and apply
// it in both directions rather than rewriting (and re-checksumming) SRAM.
// EXI channel 1 device 0 is the USB Gecko the debug console writes to.

#define EXI_CSR				0x00
#define EXI_CR				0x0c
#define EXI_DATA			0x10
#define EXI_CHANNEL_SIZE	0x14

#define EXI_CSR_CLK_8MHZ	(3 << 4)
#define EXI_CSR_CLK_32MHZ	(5 << 4)
#define EXI_CSR_CS(device)	(1 << (7 + (device)))

#define EXI_CR_TSTART		(1 << 0)
#define EXI_CR_READ			(0 << 2)
#define EXI_CR_WRITE		(1 << 2)
#define EXI_CR_READWRITE	(2 << 2)
#define EXI_CR_LEN(bytes)	(((bytes) - 1) << 4)

#define EXI_RTC_READ		0x20000000
#define EXI_RTC_WRITE		0xa0000000
#define EXI_SRAM_READ		0x20000100

#define EXI_GECKO_CHANNEL	1

#define SRAM_COUNTER_BIAS	0x0c

// Seconds between the Unix epoch and the console epoch (2000-01-01).
#define WII_RTC_EPOCH_OFFSET	946684800UL

#define EXI_TRANSFER_TIMEOUT	100000


// Console bytes set aside while looking for an input frame. Only the kernel
// debugger ever reads them, so a short ring is plenty.
#define GECKO_CONSOLE_RING_SIZE	64


static addr_t sEXIBase;
static uint32 sCounterBias;
static bool sInitialized;

static spinlock sGeckoLock = B_SPINLOCK_INITIALIZER;

static char sConsoleRing[GECKO_CONSOLE_RING_SIZE];
static uint32 sConsoleHead;
static uint32 sConsoleTail;

static uint8 sFrame[WII_GECKO_FRAME_SIZE - 2];
static uint32 sFrameLength;
static uint32 sFrameState;


static inline volatile uint32 *
exi_reg(uint32 channel, uint32 offset)
{
	return (volatile uint32 *)(sEXIBase + channel * EXI_CHANNEL_SIZE + offset);
}


static bool
exi_wait(uint32 channel)
{
	for (int i = 0; i < EXI_TRANSFER_TIMEOUT; i++) {
		if ((*exi_reg(channel, EXI_CR) & EXI_CR_TSTART) == 0)
			return true;
	}
	return false;
}


static bool
exi_imm(uint32 channel, uint32 *data, uint32 length, uint32 direction)
{
	if (direction != EXI_CR_READ)
		*exi_reg(channel, EXI_DATA) = *data;

	eieio();
	*exi_reg(channel, EXI_CR) = EXI_CR_TSTART | direction | EXI_CR_LEN(length);
	eieio();

	if (!exi_wait(channel))
		return false;

	if (direction != EXI_CR_WRITE)
		*data = *exi_reg(channel, EXI_DATA);

	return true;
}


static bool
exi_command(uint32 command, uint32 *value, uint32 direction)
{
	*exi_reg(0, EXI_CSR) = EXI_CSR_CLK_8MHZ | EXI_CSR_CS(1);
	eieio();

	bool ok = exi_imm(0, &command, 4, EXI_CR_WRITE)
		&& exi_imm(0, value, 4, direction);

	*exi_reg(0, EXI_CSR) = 0;
	eieio();

	return ok;
}


status_t
wii_rtc_init(void)
{
	if (sInitialized)
		return B_OK;

	addr_t hollywood = wii_hollywood_registers();
	if (hollywood == 0)
		return B_NO_INIT;

	sEXIBase = hollywood + WII_HW_EXI;

	// The bias sits 12 bytes into SRAM; the chip auto-increments, so step the
	// read address word by word up to it.
	*exi_reg(0, EXI_CSR) = EXI_CSR_CLK_8MHZ | EXI_CSR_CS(1);
	eieio();

	uint32 command = EXI_SRAM_READ;
	bool ok = exi_imm(0, &command, 4, EXI_CR_WRITE);
	for (uint32 offset = 0; ok && offset <= SRAM_COUNTER_BIAS; offset += 4)
		ok = exi_imm(0, &sCounterBias, 4, EXI_CR_READ);

	*exi_reg(0, EXI_CSR) = 0;
	eieio();

	if (!ok) {
		dprintf("wii_rtc_init(): SRAM read failed, assuming zero bias\n");
		sCounterBias = 0;
	}

	sInitialized = true;
	return B_OK;
}


uint32
wii_rtc_get(void)
{
	if (!sInitialized)
		return 0;

	uint32 counter = 0;
	if (!exi_command(EXI_RTC_READ, &counter, EXI_CR_READ)) {
		dprintf("wii_rtc_get(): RTC read failed\n");
		return 0;
	}

	return counter + sCounterBias + WII_RTC_EPOCH_OFFSET;
}


void
wii_rtc_set(uint32 seconds)
{
	if (!sInitialized)
		return;

	if (seconds < WII_RTC_EPOCH_OFFSET + sCounterBias)
		return;

	uint32 counter = seconds - WII_RTC_EPOCH_OFFSET - sCounterBias;
	if (!exi_command(EXI_RTC_WRITE, &counter, EXI_CR_WRITE))
		dprintf("wii_rtc_set(): RTC write failed\n");
}


// #pragma mark - USB Gecko debug console


/*!	TX is the 16 bit command 0xB000 with the byte in bits 4-11; bit 26 of the
	reply is set once the adapter's FIFO has accepted the byte.
*/
static bool
usbgecko_send_byte(char c)
{
	uint32 data = (uint32)(0xB000 | ((uint8)c << 4)) << 16;

	*exi_reg(EXI_GECKO_CHANNEL, EXI_CSR)
		= EXI_CSR_CLK_32MHZ | EXI_CSR_CS(0);
	eieio();

	bool ok = exi_imm(EXI_GECKO_CHANNEL, &data, 2, EXI_CR_READWRITE);

	*exi_reg(EXI_GECKO_CHANNEL, EXI_CSR) = 0;
	eieio();

	return ok && (data & 0x04000000) != 0;
}


/*!	RX is the 16 bit command 0xA000; bit 27 of the reply is set when a byte
	is waiting, which then sits in reply bits 16-23.
*/
static bool
usbgecko_receive_byte(char* _c)
{
	uint32 data = 0xA0000000;

	*exi_reg(EXI_GECKO_CHANNEL, EXI_CSR)
		= EXI_CSR_CLK_32MHZ | EXI_CSR_CS(0);
	eieio();

	bool ok = exi_imm(EXI_GECKO_CHANNEL, &data, 2, EXI_CR_READWRITE);

	*exi_reg(EXI_GECKO_CHANNEL, EXI_CSR) = 0;
	eieio();

	if (!ok || (data & 0x08000000) == 0)
		return false;

	*_c = (char)(data >> 16);
	return true;
}


static void
gecko_console_push(char c)
{
	uint32 next = (sConsoleHead + 1) % GECKO_CONSOLE_RING_SIZE;
	if (next == sConsoleTail)
		return;

	sConsoleRing[sConsoleHead] = c;
	sConsoleHead = next;
}


/*!	Splits one received byte into console text and host input frames; returns
	true once \a _packet holds a complete, checksummed event.
*/
static bool
gecko_demux(char c, wii_gecko_input_packet* _packet)
{
	switch (sFrameState) {
		case 0:
			if ((uint8)c == WII_GECKO_FRAME_ESCAPE)
				sFrameState = 1;
			else
				gecko_console_push(c);
			return false;

		case 1:
			if (c == WII_GECKO_FRAME_TAG) {
				sFrameState = 2;
				sFrameLength = 0;
				return false;
			}

			// Not a frame after all, so the escape was console text.
			gecko_console_push((char)WII_GECKO_FRAME_ESCAPE);
			if ((uint8)c == WII_GECKO_FRAME_ESCAPE)
				return false;

			gecko_console_push(c);
			sFrameState = 0;
			return false;

		default:
		{
			sFrame[sFrameLength++] = (uint8)c;
			if (sFrameLength < sizeof(sFrame))
				return false;

			sFrameState = 0;

			uint8 checksum = WII_GECKO_CHECKSUM_SEED;
			for (uint32 i = 0; i < sizeof(sFrame) - 1; i++)
				checksum ^= sFrame[i];
			if (checksum != sFrame[sizeof(sFrame) - 1])
				return false;

			_packet->type = sFrame[0];
			memcpy(_packet->data, sFrame + 1, sizeof(_packet->data));
			return _packet->type != WII_GECKO_INPUT_NOP;
		}
	}
}


/*!	Non-blocking drain for the input driver. The debug console shares this EXI
	channel, hence the lock around each single byte transaction.
*/
bool
wii_gecko_input_poll(wii_gecko_input_packet* packet)
{
	if (sEXIBase == 0)
		return false;

	for (int i = 0; i < GECKO_CONSOLE_RING_SIZE; i++) {
		char c;
		cpu_status state = disable_interrupts();
		acquire_spinlock(&sGeckoLock);

		bool received = usbgecko_receive_byte(&c);
		bool complete = received && gecko_demux(c, packet);

		release_spinlock(&sGeckoLock);
		restore_interrupts(state);

		if (!received)
			return false;
		if (complete)
			return true;
	}

	return false;
}


status_t
wii_serial_debug_init(void)
{
	// Runs long before the VM can map the register area; until then the
	// device window the loader left mapped keeps the EXI block reachable.
	if (sEXIBase == 0)
		sEXIBase = 0xc0000000 + WII_HOLLYWOOD_PHYS_BASE + WII_HW_EXI;

	return B_OK;
}


void
wii_serial_debug_put_char(char c)
{
	if (sEXIBase == 0)
		return;

	// The input driver polls the same channel from an ordinary thread; inside
	// the debugger nothing else runs, and taking the lock could deadlock.
	cpu_status state = 0;
	bool locked = !debug_debugger_running();
	if (locked) {
		state = disable_interrupts();
		acquire_spinlock(&sGeckoLock);
	}

	// Bounded retry: the adapter's FIFO drains at USB pace mid-burst.
	for (int i = 0; i < 10000; i++) {
		if (usbgecko_send_byte(c))
			break;
	}

	if (locked) {
		release_spinlock(&sGeckoLock);
		restore_interrupts(state);
	}
}


char
wii_serial_debug_get_char(void)
{
	if (sEXIBase == 0)
		return 0;

	// Debugger context only, so no lock: input frames arriving here are
	// decoded and dropped rather than typed into the command line.
	for (;;) {
		if (sConsoleTail != sConsoleHead) {
			char c = sConsoleRing[sConsoleTail];
			sConsoleTail = (sConsoleTail + 1) % GECKO_CONSOLE_RING_SIZE;
			return c;
		}

		char c;
		wii_gecko_input_packet packet;
		if (usbgecko_receive_byte(&c))
			gecko_demux(c, &packet);
	}
}
