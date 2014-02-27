/*
 * Copyright (c) 2010-2012 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * EXYNOS - uncompress code
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#ifndef __ASM_ARCH_UNCOMPRESS_H
#define __ASM_ARCH_UNCOMPRESS_H __FILE__

#include <asm/mach-types.h>

#include <mach/map.h>
#include <plat/uncompress.h>

static unsigned int __raw_readl(unsigned int ptr)
{
	return *((volatile unsigned int *)ptr);
}

// KID 20140227
static void arch_detect_cpu(void)
{
	// EXYNOS_PA_CHIPID: 0x10000000
	u32 chip_id = __raw_readl(EXYNOS_PA_CHIPID);

	/*
	 * product_id is bits 31:12
	 * bits 23:20 describe the exynosX family
	 * bits 27:24 describe the exynosX family in exynos5420
	 */
	chip_id >>= 20;

	if ((chip_id & 0x0f) == 0x5 || (chip_id & 0xf0) == 0x50)
		// EXYNOS5_PA_UART: 0x12C00000, S3C_UART_OFFSET: 0x10000, CONFIG_S3C_LOWLEVEL_UART_PORT: 3
		uart_base = (volatile u8 *)EXYNOS5_PA_UART + (S3C_UART_OFFSET * CONFIG_S3C_LOWLEVEL_UART_PORT);
		// uart_base: 0x12C30000
	else
		uart_base = (volatile u8 *)EXYNOS4_PA_UART + (S3C_UART_OFFSET * CONFIG_S3C_LOWLEVEL_UART_PORT);

	/*
	 * For preventing FIFO overrun or infinite loop of UART console,
	 * fifo_max should be the minimum fifo size of all of the UART channels
	 */
	// S5PV210_UFSTAT_TXMASK: 0xFF0000
	fifo_mask = S5PV210_UFSTAT_TXMASK;
	// fifo_mask: 0xFF0000

	// S5PV210_UFSTAT_TXSHIFT: 0x10
	fifo_max = 15 << S5PV210_UFSTAT_TXSHIFT;
	// fifo_max: 0xF0000 
}
#endif /* __ASM_ARCH_UNCOMPRESS_H */
