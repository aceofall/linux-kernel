/*
 * Tag parsing.
 *
 * Copyright (C) 1995-2001 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * This is the traditional way of passing data to the kernel at boot time.  Rather
 * than passing a fixed inflexible structure to the kernel, we pass a list
 * of variable-sized tags to the kernel.  The first tag must be a ATAG_CORE
 * tag for the list to be recognised (to distinguish the tagged list from
 * a param_struct).  The list is terminated with a zero-length tag (this tag
 * is not parsed in any way).
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/root_dev.h>
#include <linux/screen_info.h>

#include <asm/setup.h>
#include <asm/system_info.h>
#include <asm/page.h>
#include <asm/mach/arch.h>

#include "atags.h"

// ARM10C 20131012
// CONFIG_CMDLINE="earlyprintk=ocd,keep ignore_loglevel"
// KID 20140302
// CONFIG_CMDLINE: "root=/dev/ram0 rw ramdisk=8192 initrd=0x41000000,8M console=ttySAC1,115200 init=/linuxrc mem=256M"
static char default_command_line[COMMAND_LINE_SIZE] __initdata = CONFIG_CMDLINE;

#ifndef MEM_SIZE
// ARM10C 20131012
// KID 20140302
// MEM_SIZE: 0x1000000 (16MB)
#define MEM_SIZE	(16*1024*1024)
#endif

// ARM10C 20131012
// KID 20140227
// KID 20140302
// ATAG_CORE: 0x54410001, ATAG_MEM: 0x54410002, ATAG_NONE: 0x00000000
// PAGE_SIZE: 4096 (4K), MEM_SIZE: 0x1000000 (16MB)
// tag_size(tag_core): 20, tag_size(tag_mem32): 16
//
// static struct {
// 	struct tag_header hdr1;
// 	struct tag_core   core;
// 	struct tag_header hdr2;
// 	struct tag_mem32  mem;
// 	struct tag_header hdr3;
// } default_tags __initdata = {
// 	{ 20, 0x54410001 },
// 	{ 1, 4096, 0xff },
// 	{ 16, 0x54410002 },
// 	{ 0x1000000 },
// 	{ 0, 0x00000000 }
// };
static struct {
	struct tag_header hdr1;
	struct tag_core   core;
	struct tag_header hdr2;
	struct tag_mem32  mem;
	struct tag_header hdr3;
} default_tags __initdata = {
	{ tag_size(tag_core), ATAG_CORE },
	{ 1, PAGE_SIZE, 0xff },
	{ tag_size(tag_mem32), ATAG_MEM },
	{ MEM_SIZE },
	{ 0, ATAG_NONE }
};

// KID 20140302
// tag->hdr.tag: ATAG_CORE
static int __init parse_tag_core(const struct tag *tag)
{
	// tag->hdr.size: 20
	if (tag->hdr.size > 2) {
		// tag->u.core.flags: 1
		if ((tag->u.core.flags & 1) == 0)
			root_mountflags &= ~MS_RDONLY;

		// tag->u.core.rootdev: 0xFF
		ROOT_DEV = old_decode_dev(tag->u.core.rootdev);
		// ROOT_DEV: 0
	}
	return 0;
}

__tagtable(ATAG_CORE, parse_tag_core);

// KID 20140302
// tag->hdr.tag: ATAG_MEM
static int __init parse_tag_mem32(const struct tag *tag)
{
	// tag->u.mem.start: 0, tag->u.mem.size: 0x1000000 
	return arm_add_memory(tag->u.mem.start, tag->u.mem.size);
}

__tagtable(ATAG_MEM, parse_tag_mem32);

#if defined(CONFIG_VGA_CONSOLE) || defined(CONFIG_DUMMY_CONSOLE)
static int __init parse_tag_videotext(const struct tag *tag)
{
	screen_info.orig_x            = tag->u.videotext.x;
	screen_info.orig_y            = tag->u.videotext.y;
	screen_info.orig_video_page   = tag->u.videotext.video_page;
	screen_info.orig_video_mode   = tag->u.videotext.video_mode;
	screen_info.orig_video_cols   = tag->u.videotext.video_cols;
	screen_info.orig_video_ega_bx = tag->u.videotext.video_ega_bx;
	screen_info.orig_video_lines  = tag->u.videotext.video_lines;
	screen_info.orig_video_isVGA  = tag->u.videotext.video_isvga;
	screen_info.orig_video_points = tag->u.videotext.video_points;
	return 0;
}

__tagtable(ATAG_VIDEOTEXT, parse_tag_videotext);
#endif

#ifdef CONFIG_BLK_DEV_RAM
static int __init parse_tag_ramdisk(const struct tag *tag)
{
	extern int rd_size, rd_image_start, rd_prompt, rd_doload;

	rd_image_start = tag->u.ramdisk.start;
	rd_doload = (tag->u.ramdisk.flags & 1) == 0;
	rd_prompt = (tag->u.ramdisk.flags & 2) == 0;

	if (tag->u.ramdisk.size)
		rd_size = tag->u.ramdisk.size;

	return 0;
}

__tagtable(ATAG_RAMDISK, parse_tag_ramdisk);
#endif

static int __init parse_tag_serialnr(const struct tag *tag)
{
	system_serial_low = tag->u.serialnr.low;
	system_serial_high = tag->u.serialnr.high;
	return 0;
}

__tagtable(ATAG_SERIAL, parse_tag_serialnr);

static int __init parse_tag_revision(const struct tag *tag)
{
	system_rev = tag->u.revision.rev;
	return 0;
}

__tagtable(ATAG_REVISION, parse_tag_revision);

static int __init parse_tag_cmdline(const struct tag *tag)
{
#if defined(CONFIG_CMDLINE_EXTEND)
	strlcat(default_command_line, " ", COMMAND_LINE_SIZE);
	strlcat(default_command_line, tag->u.cmdline.cmdline,
		COMMAND_LINE_SIZE);
#elif defined(CONFIG_CMDLINE_FORCE)
	pr_warning("Ignoring tag cmdline (using the default kernel command line)\n");
#else
	strlcpy(default_command_line, tag->u.cmdline.cmdline,
		COMMAND_LINE_SIZE);
#endif
	return 0;
}

__tagtable(ATAG_CMDLINE, parse_tag_cmdline);

/*
 * Scan the tag table for this tag, and call its parse function.
 * The tag table is built by the linker from all the __tagtable
 * declarations.
 */
// KID 20140302
// tags: default_tags으로 가정
static int __init parse_tag(const struct tag *tag)
{
	extern struct tagtable __tagtable_begin, __tagtable_end;
	struct tagtable *t;

	// __tagtable(ATAG_CORE, parse_tag_core) 와 같이 등록된 function들을 순회
	for (t = &__tagtable_begin; t < &__tagtable_end; t++)
		// tag->hdr.tag: ATAG_CORE
		// tag->hdr.tag: ATAG_MEM
		if (tag->hdr.tag == t->tag) {
			// t->parse: parse_tag_core
			// t->parse: parse_tag_mem32
			t->parse(tag);
			break;
		}

	return t < &__tagtable_end;
}

/*
 * Parse all tags in the list, checking both the global and architecture
 * specific tag tables.
 */
static void __init parse_tags(const struct tag *t)
{
	for (; t->hdr.size; t = tag_next(t))
		if (!parse_tag(t))
			printk(KERN_WARNING
				"Ignoring unrecognised tag 0x%08x\n",
				t->hdr.tag);
}

// KID 20140302
// tags: default_tags으로 가정
static void __init squash_mem_tags(struct tag *tag)
{
	for (; tag->hdr.size; tag = tag_next(tag))
		// tag->hdr.size: 16, tag->hdr.tag: 0x54410002, ATAG_MEM: 0x54410002
		if (tag->hdr.tag == ATAG_MEM)
			// ATAG_NONE: 0x00000000 
			tag->hdr.tag = ATAG_NONE;
			// tag->hdr.tag: 0x00000000 
			// ATAG에서 MEM TAG 정보를 찾고 TAG를 NONE으로 변경
}

// ARM10C 20131012
// KID 20140227
// KID 20140302
// __atags_pointer: bootloader에서 넘겨준 atag를 저장해놓은  주소,
// machine_nr: bootloader에서 넘겨준 machine 정보
// bootloader에서 machine_nr 을 MACH_TYPE_S3C2440: 362 로 넘겨줬다고 가정
const struct machine_desc * __init
setup_machine_tags(phys_addr_t __atags_pointer, unsigned int machine_nr)
{
	struct tag *tags = (struct tag *)&default_tags;
	const struct machine_desc *mdesc = NULL, *p;
	char *from = default_command_line;

	// PHYS_OFFSET: 0x20000000
	default_tags.mem.start = PHYS_OFFSET;
	// default_tags.mem.start: 0x20000000

	/*
	 * locate machine in the list of supported machines.
	 */
	for_each_machine_desc(p)
	// for (p = __arch_info_begin; p < __arch_info_end; p++)
		// machine_nr: 362
		// p->nr: arch/arm/mach-s3c24xx/mach-smdk2440.c 의 
		// MACHINE_START(S3C2440, "SMDK2440") 구조체 값 참조
		if (machine_nr == p->nr) {
			// p->name: "SMDK2440"
			printk("Machine: %s\n", p->name);
			mdesc = p;
			// mdesc: __mach_desc_S3C2440
			break;
		}

	if (!mdesc) {
		early_print("\nError: unrecognized/unsupported machine ID"
			    " (r1 = 0x%08x).\n\n", machine_nr);
		dump_machine_table(); /* does not return */
	}

	if (__atags_pointer)
		tags = phys_to_virt(__atags_pointer);
	// __mach_desc_S3C2440->atag_offset: 0x100
	else if (mdesc->atag_offset)
		// PAGE_OFFSET: 0xC0000000, mdesc->atag_offset: 0x100
		tags = (void *)(PAGE_OFFSET + mdesc->atag_offset);
		// tags: 0xC0000100

#if defined(CONFIG_DEPRECATED_PARAM_STRUCT) // CONFIG_DEPRECATED_PARAM_STRUCT=n
	/*
	 * If we have the old style parameters, convert them to
	 * a tag list.
	 */
	if (tags->hdr.tag != ATAG_CORE)
		convert_to_tag_list(tags);
#endif
	// ATAG_CORE: 0x54410001, tags: default_tags으로 가정
	if (tags->hdr.tag != ATAG_CORE) {
		early_print("Warning: Neither atags nor dtb found\n");
		tags = (struct tag *)&default_tags;
	}

	// __mach_desc_S3C2440->fixup: NULL
	if (mdesc->fixup)
		mdesc->fixup(tags, &from, &meminfo);

	// ATAG_CORE: 0x54410001
	if (tags->hdr.tag == ATAG_CORE) {
		if (meminfo.nr_banks != 0)
			squash_mem_tags(tags);
		save_atags(tags);
		// atags_copy (1.5KB) 에 tags 를 카피

		parse_tags(tags);
		// 각 TAG에 맞게 등록된 parse function을 수행
	}

	/* parse_early_param needs a boot_command_line */
	// from: default_command_line: "root=/dev/ram0 rw ramdisk=8192 initrd=0x41000000,8M console=ttySAC1,115200 init=/linuxrc mem=256M"
	// COMMAND_LINE_SIZE: 1024
	strlcpy(boot_command_line, from, COMMAND_LINE_SIZE);
	// boot_command_line에 default_command_line 문자열 복사

	return mdesc;
}
