/*
 * SKIP_ATF, OPMODE=aarch32 only - links the AArch64 stage2
 * (src/stage2_main.c). tools/mk_bl1_image.py appends it to BL1's image
 * at this address. Keep in sync with STAGE2_AARCH64_ADDR in
 * src/cfgBootDefine.h.
 */

OUTPUT_FORMAT("elf64-littleaarch64", "elf64-littleaarch64", "elf64-littleaarch64")
OUTPUT_ARCH(aarch64)

ENTRY(Startup)

SECTIONS
{
	. = 0x00000000FFFF9000;
	. = ALIGN(4);
	.text :
	{
		obj-stage2/startup_aarch64.o	(.text)
		*(.text)
	}

	.vectors ALIGN(2048) : { *(.vectors) }

	. = ALIGN(4);
	.rodata : { *(.rodata*) }

	. = ALIGN(4);
	.data : { *(.data*) }

	. = ALIGN(4);
	.got : { *(.got) }

	_end = .;

	. = ALIGN(8);
	__bss_start__ = .;
	.bss_start (OVERLAY) : {
		KEEP(*(.__bss_start));
		__bss_base = .;
	}
	.bss __bss_base (OVERLAY) : {
		*(.bss*)
		. = ALIGN(4);
		__bss_limit = .;
	}
	.bss_end __bss_limit (OVERLAY) : {
		KEEP(*(.__bss_end));
	}
	__bss_end__ = .;
}

ASSERT(Startup == 0x00000000FFFF9000, "Startup must be at 0x00000000FFFF9000, STAGE2_AARCH64_ADDR, where BL1 resets into")
