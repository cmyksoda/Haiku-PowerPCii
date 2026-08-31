/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <KernelExport.h>
#include <boot/platform.h>
#include <boot/heap.h>
#include <boot/stage2.h>
#include <arch/cpu.h>
#include <string.h>

#include <arch_platform.h>

#include <gccore.h>
#include <ogcsys.h>
#include <ogc/machine/processor.h>

#include "console.h"
#include "debug.h"
#include "mmu.h"


// libogc's crt0 sequence, none of it declared in a public header
extern "C" void PPCExcptInit(void);
extern "C" void KThreadInit(void);
extern "C" void KIrqInit(void);
extern "C" void SYS_PreMain(void);

extern "C" void wii_start(void);
extern "C" int main(stage2_args *args);
extern "C" status_t arch_start_kernel(struct kernel_args *kernelArgs,
		addr_t kernelEntry, addr_t kernelStackTop, uint32 sdr1);
extern "C" status_t boot_arch_mmu_init(void);

extern void (*__ctor_list)(void);
extern void (*__ctor_end)(void);


static void
call_ctors(void)
{
	void (**f)(void);

	for (f = &__ctor_list; f < &__ctor_end; f++)
		(**f)();
}


static addr_t
get_kernel_entry(void)
{
	if (gKernelArgs.kernel_image->elf_class == ELFCLASS64) {
		preloaded_elf64_image *image = static_cast<preloaded_elf64_image *>(
			gKernelArgs.kernel_image.Pointer());
		return image->elf_header.e_entry;
	} else if (gKernelArgs.kernel_image->elf_class == ELFCLASS32) {
		preloaded_elf32_image *image = static_cast<preloaded_elf32_image *>(
			gKernelArgs.kernel_image.Pointer());
		return image->elf_header.e_entry;
	}
	panic("Unknown kernel format! Not 32-bit or 64-bit!");
	return 0;
}


extern "C" void
platform_start_kernel(void)
{
	// only now are the boot volume's driver settings loaded
	video_apply_settings();

	addr_t kernelEntry = get_kernel_entry();
	addr_t stackTop = gKernelArgs.cpu_kstack[0].start
		+ gKernelArgs.cpu_kstack[0].size;

	dprintf("kernel entry at %p\n", (void*)kernelEntry);
	dprintf("kernel stack top: %p\n", (void*)stackTop);

	// the kernel's first stack frame, written while the BATs still reach it
	memset((void*)(stackTop - 16), 0, 16);

	kernel_args *kernelArgs = &gKernelArgs;
	uint32 sdr1 = 0;
	if (wii_mmu_prepare_handoff(&kernelArgs, &sdr1) != B_OK)
		panic("could not build the kernel's page table\n");

	// the kernel takes over the exception vectors, so silence libogc's first
	uint32_t cookie;
	_CPU_ISR_Disable(cookie);
	(void)cookie;

	dprintf("entering the kernel: args %p, sdr1 %p\n", kernelArgs,
		(void*)sdr1);

	status_t error = arch_start_kernel(kernelArgs, kernelEntry, stackTop,
		sdr1);

	panic("Kernel returned! Return value: %" B_PRId32 "\n", error);
}


extern "C" void
platform_exit(void)
{
	SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);

	while (true)
		;
}


extern "C" uint32
platform_boot_options(void)
{
	return 0;
}


//! arch_rtc_init() divides by the time base frequency, so it must not be zero.
extern "C" status_t
boot_arch_cpu_init(void)
{
	gKernelArgs.num_cpus = 1;
	gKernelArgs.arch_args.cpu_frequency = TB_CORE_CLOCK;
	gKernelArgs.arch_args.bus_frequency = TB_BUS_CLOCK;
	gKernelArgs.arch_args.time_base_frequency = TB_TIMER_CLOCK * 1000;
	gKernelArgs.arch_args.platform = PPC_PLATFORM_WII;

	return B_OK;
}


extern "C" void
wii_start(void)
{
	ctype_init();

	PPCExcptInit();
	KThreadInit();
	KIrqInit();
	SYS_Init();
	SYS_PreMain();

	call_ctors();

	video_init();
	debug_init();

	dprintf("\nHaiku boot loader for the Nintendo Wii\n");
	dprintf("MEM1 arena: %p - %p, MEM2 arena: %p - %p\n", SYS_GetArena1Lo(),
		SYS_GetArena1Hi(), SYS_GetArena2Lo(), SYS_GetArena2Hi());

	if (boot_arch_cpu_init() != B_OK)
		panic("could not describe the cpu to the kernel\n");

	if (boot_arch_mmu_init() != B_OK)
		panic("could not set up the loader's memory map\n");

	stage2_args args;
	memset(&args, 0, sizeof(stage2_args));

	main(&args);
		// only returns if the user asked to leave the loader

	platform_exit();
}
