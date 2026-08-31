/*
 * Copyright 2006, Ingo Weinhold <bonefish@cs.tu-berlin.de>.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <arch_platform.h>

#include <new>

#include <KernelExport.h>

#include <arch/generic/debug_uart.h>
#include <boot/kernel_args.h>
#include <platform/wii/wii.h>
#include <real_time_clock.h>
#include <util/kernel_cpp.h>
#include <vm/vm.h>


void *gFDT;
static PPCPlatform *sPPCPlatform;


PPCPlatform::PPCPlatform(ppc_platform_type platformType)
	: fPlatformType(platformType)
{
}


PPCPlatform::~PPCPlatform()
{
}


PPCPlatform *
PPCPlatform::Default()
{
	return sPPCPlatform;
}


status_t
PPCPlatform::InitPostThread(struct kernel_args *kernelArgs)
{
	return B_OK;
}


// #pragma mark - U-Boot + FDT


namespace BPrivate {

class PPCUBoot : public PPCPlatform {
public:
	PPCUBoot();
	virtual ~PPCUBoot();

	virtual status_t Init(struct kernel_args *kernelArgs);
	virtual status_t InitSerialDebug(struct kernel_args *kernelArgs);
	virtual status_t InitPostVM(struct kernel_args *kernelArgs);
	virtual status_t InitRTC(struct kernel_args *kernelArgs,
		struct real_time_data *data);

	virtual char SerialDebugGetChar();
	virtual void SerialDebugPutChar(char c);

	virtual	void SetHardwareRTC(uint64 seconds);
	virtual	uint32 GetHardwareRTC();

	virtual	void ShutDown(bool reboot);

private:
	int	fInput;
	int	fOutput;
	int	fRTC;
	DebugUART *fDebugUART;
};

}	// namespace BPrivate

using BPrivate::PPCUBoot;


PPCUBoot::PPCUBoot()
	: PPCPlatform(PPC_PLATFORM_U_BOOT),
	  fInput(-1),
	  fOutput(-1),
	  fRTC(-1),
	  fDebugUART(NULL)
{
}


PPCUBoot::~PPCUBoot()
{
}


status_t
PPCUBoot::Init(struct kernel_args *kernelArgs)
{
	gFDT = kernelArgs->platform_args.fdt;
	// XXX: do we error out if no FDT?
	return B_OK;
}


status_t
PPCUBoot::InitSerialDebug(struct kernel_args *kernelArgs)
{
	// TODO: get relevant debug uart from fdt
	//fDebugUART = debug_uart_from_fdt(gFDT);
	if (fDebugUART == NULL)
		return B_ERROR;
	return B_OK;
}


status_t
PPCUBoot::InitPostVM(struct kernel_args *kernelArgs)
{
	return B_ERROR;
}


status_t
PPCUBoot::InitRTC(struct kernel_args *kernelArgs,
	struct real_time_data *data)
{
	return B_ERROR;
}


char
PPCUBoot::SerialDebugGetChar()
{
	if (fDebugUART)
		return fDebugUART->GetChar(false);
	return 0;
}


void
PPCUBoot::SerialDebugPutChar(char c)
{
	if (fDebugUART)
		fDebugUART->PutChar(c);
}


void
PPCUBoot::SetHardwareRTC(uint64 seconds)
{
}


uint32
PPCUBoot::GetHardwareRTC()
{
	return 0;
}


void
PPCUBoot::ShutDown(bool reboot)
{
}


// #pragma mark - Wii

namespace BPrivate {

class PPCWii : public PPCPlatform {
public:
	PPCWii();
	virtual ~PPCWii();

	virtual status_t Init(struct kernel_args *kernelArgs);
	virtual status_t InitSerialDebug(struct kernel_args *kernelArgs);
	virtual status_t InitPostVM(struct kernel_args *kernelArgs);
	virtual status_t InitPostThread(struct kernel_args *kernelArgs);
	virtual status_t InitRTC(struct kernel_args *kernelArgs,
		struct real_time_data *data);

	virtual char SerialDebugGetChar();
	virtual void SerialDebugPutChar(char c);

	virtual void SetHardwareRTC(uint64 seconds);
	virtual uint32 GetHardwareRTC();

	virtual void ShutDown(bool reboot);

private:
	static int32 VideoThread(void* arg);
	
	area_id fFakeFrameBufferArea;
	area_id fRealFrameBufferArea;
	void* fFakeFrameBuffer;
	void* fRealFrameBuffer;
	int fFrameBufferWidth;
	int fFrameBufferHeight;
};

}	// namespace BPrivate
using BPrivate::PPCWii;

PPCWii::PPCWii()
	:
	PPCPlatform(PPC_PLATFORM_WII),
	fFakeFrameBufferArea(-1),
	fRealFrameBufferArea(-1),
	fFakeFrameBuffer(NULL),
	fRealFrameBuffer(NULL),
	fFrameBufferWidth(0),
	fFrameBufferHeight(0)
{
}


PPCWii::~PPCWii()
{
}


status_t
PPCWii::Init(struct kernel_args *kernelArgs)
{
	return wii_platform_init(kernelArgs);
}


status_t
PPCWii::InitSerialDebug(struct kernel_args *kernelArgs)
{
	return wii_serial_debug_init();
}


status_t
PPCWii::InitPostVM(struct kernel_args *kernelArgs)
{
	status_t error = wii_platform_init_post_vm(kernelArgs);
	if (error != B_OK)
		return error;

	// Map the fake RGB32 framebuffer (which app_server draws to)
	fFrameBufferWidth = kernelArgs->frame_buffer.width;
	fFrameBufferHeight = kernelArgs->frame_buffer.height;
	size_t fakeSize = fFrameBufferWidth * fFrameBufferHeight * 4;
	
	// Write-combining = cache-inhibited but non-guarded: loads and stores can
	// gather instead of one strictly ordered bus transaction each (the same
	// reasoning as the framebuffer driver's remap_frame_buffer()).
	fFakeFrameBufferArea = map_physical_memory("wii fake rgb framebuffer",
		kernelArgs->frame_buffer.physical_buffer.start, fakeSize,
		B_ANY_KERNEL_ADDRESS | B_WRITE_COMBINING_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &fFakeFrameBuffer);
		
	if (fFakeFrameBufferArea < 0) {
		dprintf("PPCWii: Failed to map fake framebuffer\n");
		return B_ERROR;
	}

	// Map the real hardware YUYV framebuffer
	size_t realSize = kernelArgs->arch_args.wii_hardware_framebuffer.size;
	fRealFrameBufferArea = map_physical_memory("wii real yuyv framebuffer",
		kernelArgs->arch_args.wii_hardware_framebuffer.start, realSize,
		B_ANY_KERNEL_ADDRESS | B_WRITE_COMBINING_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &fRealFrameBuffer);
		
	if (fRealFrameBufferArea < 0) {
		dprintf("PPCWii: Failed to map real framebuffer\n");
		return B_ERROR;
	}

	return B_OK;
}


//! The conversion thread can only be spawned once the thread system is up.
status_t
PPCWii::InitPostThread(struct kernel_args *kernelArgs)
{
	// Low priority: a missed frame is invisible, a starved launch_daemon isn't.
	thread_id thread = spawn_kernel_thread(VideoThread, "wii video conversion",
		B_LOW_PRIORITY, this);
	if (thread < 0)
		return thread;

	return resume_thread(thread);
}


int32
PPCWii::VideoThread(void* arg)
{
	PPCWii* self = (PPCWii*)arg;

	bigtime_t windowTime = 0;
	uint32 frames = 0;
	uint32 lastReport = 0;

	while (true) {
		// Wait ~33ms (30 FPS)
		snooze(33333);

		uint32* src = (uint32*)self->fFakeFrameBuffer;
		uint32* dst = (uint32*)self->fRealFrameBuffer;

		int pairs = (self->fFrameBufferWidth & ~1)
			* self->fFrameBufferHeight / 2;

		bigtime_t frameStart = system_time();

		// Convert RGB32 to YUYV (YUV422). Both buffers are cache-inhibited,
		// so touch them word-at-a-time: 3 bus accesses per pixel pair
		// instead of the 10 a bytewise loop costs.
		// app_server's B_RGB32 is B,G,R,X byte order in memory on every arch
		// (Painter uses agg::order_bgra), so a big-endian load is B G R X
		// from the top byte down.
		for (int i = 0; i < pairs; i++) {
			uint32 p0 = *src++;
			uint32 p1 = *src++;

			int b0 = p0 >> 24;
			int g0 = (p0 >> 16) & 0xff;
			int r0 = (p0 >> 8) & 0xff;
			int b1 = p1 >> 24;
			int g1 = (p1 >> 16) & 0xff;
			int r1 = (p1 >> 8) & 0xff;

			// Y = (77*R + 150*G + 29*B) >> 8, U/V per the usual
			// approximation; each coefficient row sums to 256 or +-128, so
			// no clamping is needed.
			int y0 = (77 * r0 + 150 * g0 + 29 * b0) >> 8;
			int y1 = (77 * r1 + 150 * g1 + 29 * b1) >> 8;

			int r = (r0 + r1) >> 1;
			int g = (g0 + g1) >> 1;
			int b = (b0 + b1) >> 1;

			uint32 u = ((-43 * r - 85 * g + 128 * b) >> 8) + 128;
			uint32 v = ((128 * r - 107 * g - 21 * b) >> 8) + 128;

			// YUYV: Y0 U0 Y1 V0
			*dst++ = ((uint32)y0 << 24) | (u << 16) | ((uint32)y1 << 8) | v;
		}

		// This copy is suspected of eating most of the core; keep its cost
		// visible in the syslog (one line per ~1024 frames).
		windowTime += system_time() - frameStart;
		frames++;
		if (frames == 32 || frames - lastReport >= 1024) {
			dprintf("wii video: convert avg %" B_PRId64 " us over frames %"
				B_PRIu32 "..%" B_PRIu32 "\n",
				windowTime / (frames - lastReport), lastReport + 1, frames);
			windowTime = 0;
			lastReport = frames;
		}
	}

	return 0;
}


status_t
PPCWii::InitRTC(struct kernel_args *kernelArgs,
	struct real_time_data *data)
{
	return wii_rtc_init();
}


char
PPCWii::SerialDebugGetChar()
{
	return wii_serial_debug_get_char();
}


void
PPCWii::SerialDebugPutChar(char c)
{
	wii_serial_debug_put_char(c);
}


void
PPCWii::SetHardwareRTC(uint64 seconds)
{
	wii_rtc_set((uint32)seconds);
}


uint32
PPCWii::GetHardwareRTC()
{
	return wii_rtc_get();
}


void
PPCWii::ShutDown(bool reboot)
{
	wii_platform_shutdown(reboot);
}


// # pragma mark -


#define PLATFORM_BUFFER_SIZE MAX(sizeof(PPCUBoot), sizeof(PPCWii))
// static buffer for constructing the actual PPCPlatform
static char *sPPCPlatformBuffer[PLATFORM_BUFFER_SIZE];

// PCI host bridge info, captured by the boot loader (see arch_kernel_args).
// Legacy single-bridge fields mirror bridge 0 (the boot bridge).
static uint32 sPCIHostBridgeType = 0;
static phys_addr_t sPCIConfigAddress = 0xfec00000;
static phys_addr_t sPCIConfigData = 0xfee00000;

// All PCI host bridges (a Power Mac G4 has several UniNorth buses).
static uint32 sPCIHostBridgeCount = 0;
static struct {
	uint32		type;
	phys_addr_t	configAddress;
	phys_addr_t	configData;
} sPCIHostBridges[MAX_PCI_HOST_BRIDGES];

static uint32 sGmacIRQ = 0;
static uint8 sGmacMAC[6] = { 0, 0, 0, 0, 0, 0 };
static bool sGmacMACValid = false;


extern "C" void
ppc_get_pci_host_bridge(uint32* type, phys_addr_t* configAddress,
	phys_addr_t* configData)
{
	*type = sPCIHostBridgeType;
	*configAddress = sPCIConfigAddress;
	*configData = sPCIConfigData;
}

extern "C" uint32
ppc_get_pci_host_bridge_count()
{
	return sPCIHostBridgeCount;
}

extern "C" status_t
ppc_get_pci_host_bridge_at(uint32 index, uint32* type,
	phys_addr_t* configAddress, phys_addr_t* configData)
{
	if (index >= sPCIHostBridgeCount)
		return B_BAD_INDEX;
	*type = sPCIHostBridges[index].type;
	*configAddress = sPCIHostBridges[index].configAddress;
	*configData = sPCIHostBridges[index].configData;
	return B_OK;
}

extern "C" uint32
ppc_get_gmac_irq()
{
	return sGmacIRQ;
}

extern "C" bool
ppc_get_gmac_mac(uint8* address)
{
	if (!sGmacMACValid)
		return false;
	for (int i = 0; i < 6; i++)
		address[i] = sGmacMAC[i];
	return true;
}


status_t
arch_platform_init(struct kernel_args *kernelArgs)
{
	switch (kernelArgs->arch_args.platform) {
		case PPC_PLATFORM_U_BOOT:
			sPPCPlatform = new(sPPCPlatformBuffer) PPCUBoot;
			break;
		case PPC_PLATFORM_WII:
			sPPCPlatform = new(sPPCPlatformBuffer) PPCWii;
			break;
		default:
			return B_ERROR;
	}

	sPCIHostBridgeType = kernelArgs->arch_args.pci_host_bridge_type;
	sPCIConfigAddress = kernelArgs->arch_args.pci_config_address;
	sPCIConfigData = kernelArgs->arch_args.pci_config_data;
	sGmacIRQ = kernelArgs->arch_args.gmac_irq;
	sGmacMACValid = kernelArgs->arch_args.gmac_mac_valid != 0;
	for (int i = 0; i < 6; i++)
		sGmacMAC[i] = kernelArgs->arch_args.gmac_mac[i];

	uint32 bridgeCount = kernelArgs->arch_args.pci_host_bridge_count;
	if (bridgeCount == 0 || bridgeCount > MAX_PCI_HOST_BRIDGES) {
		// Old loader (or none captured): synthesize bridge 0 from the
		// legacy fields so single-bridge machines keep working.
		sPCIHostBridgeCount = 1;
		sPCIHostBridges[0].type = sPCIHostBridgeType;
		sPCIHostBridges[0].configAddress = sPCIConfigAddress;
		sPCIHostBridges[0].configData = sPCIConfigData;
	} else {
		sPCIHostBridgeCount = bridgeCount;
		for (uint32 i = 0; i < bridgeCount; i++) {
			sPCIHostBridges[i].type
				= kernelArgs->arch_args.pci_host_bridges[i].type;
			sPCIHostBridges[i].configAddress
				= kernelArgs->arch_args.pci_host_bridges[i].config_address;
			sPCIHostBridges[i].configData
				= kernelArgs->arch_args.pci_host_bridges[i].config_data;
		}
	}

	return sPPCPlatform->Init(kernelArgs);
}


status_t
arch_platform_init_post_vm(struct kernel_args *kernelArgs)
{
	return sPPCPlatform->InitPostVM(kernelArgs);
}


status_t
arch_platform_init_post_thread(struct kernel_args *kernelArgs)
{
	return sPPCPlatform->InitPostThread(kernelArgs);
}
