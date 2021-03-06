/*
 * Copyright (c) 2013 Samsung Electronics Co., Ltd.
 * Copyright (c) 2013 Linaro Ltd.
 * Author: Thomas Abraham <thomas.ab@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Common Clock Framework support for all Samsung platforms
*/

#ifndef __SAMSUNG_CLK_H
#define __SAMSUNG_CLK_H

#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/io.h>
#include <linux/clk-provider.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include "clk-pll.h"

/**
 * struct samsung_clock_alias: information about mux clock
 * @id: platform specific id of the clock.
 * @dev_name: name of the device to which this clock belongs.
 * @alias: optional clock alias name to be assigned to this clock.
 */
struct samsung_clock_alias {
	unsigned int		id;
	const char		*dev_name;
	const char		*alias;
};

#define ALIAS(_id, dname, a)	\
	{							\
		.id		= _id,				\
		.dev_name	= dname,			\
		.alias		= a,				\
	}

#define MHZ (1000 * 1000)

/**
 * struct samsung_fixed_rate_clock: information about fixed-rate clock
 * @id: platform specific id of the clock.
 * @name: name of this fixed-rate clock.
 * @parent_name: optional parent clock name.
 * @flags: optional fixed-rate clock flags.
 * @fixed-rate: fixed clock rate of this clock.
 */
// ARM10C 20150110
struct samsung_fixed_rate_clock {
	unsigned int		id;
	char			*name;
	const char		*parent_name;
	unsigned long		flags;
	unsigned long		fixed_rate;
};

// ARM10C 20150110
// ARM10C 20150124
#define FRATE(_id, cname, pname, f, frate)		\
	{						\
		.id		= _id,			\
		.name		= cname,		\
		.parent_name	= pname,		\
		.flags		= f,			\
		.fixed_rate	= frate,		\
	}

/*
 * struct samsung_fixed_factor_clock: information about fixed-factor clock
 * @id: platform specific id of the clock.
 * @name: name of this fixed-factor clock.
 * @parent_name: parent clock name.
 * @mult: fixed multiplication factor.
 * @div: fixed division factor.
 * @flags: optional fixed-factor clock flags.
 */
struct samsung_fixed_factor_clock {
	unsigned int		id;
	char			*name;
	const char		*parent_name;
	unsigned long		mult;
	unsigned long		div;
	unsigned long		flags;
};

// ARM10C 20150124
#define FFACTOR(_id, cname, pname, m, d, f)		\
	{						\
		.id		= _id,			\
		.name		= cname,		\
		.parent_name	= pname,		\
		.mult		= m,			\
		.div		= d,			\
		.flags		= f,			\
	}

/**
 * struct samsung_mux_clock: information about mux clock
 * @id: platform specific id of the clock.
 * @dev_name: name of the device to which this clock belongs.
 * @name: name of this mux clock.
 * @parent_names: array of pointer to parent clock names.
 * @num_parents: number of parents listed in @parent_names.
 * @flags: optional flags for basic clock.
 * @offset: offset of the register for configuring the mux.
 * @shift: starting bit location of the mux control bit-field in @reg.
 * @width: width of the mux control bit-field in @reg.
 * @mux_flags: flags for mux-type clock.
 * @alias: optional clock alias name to be assigned to this clock.
 */
// ARM10C 20150131
struct samsung_mux_clock {
	unsigned int		id;
	const char		*dev_name;
	const char		*name;
	const char		**parent_names;
	u8			num_parents;
	unsigned long		flags;
	unsigned long		offset;
	u8			shift;
	u8			width;
	u8			mux_flags;
	const char		*alias;
};

// ARM10C 20150131
// #define __MUX(none, NULL, "mout_mspll_kfc", mspll_cpu_p, SRC_TOP7, 8, 2, 0, 0, NULL):
// {
// 	.id		= none,
// 	.dev_name	= NULL,
// 	.name		= "mout_mspll_kfc",
// 	.parent_names	= mspll_cpu_p,
// 	.num_parents	= ARRAY_SIZE(mspll_cpu_p),
// 	.flags		= (0) | CLK_SET_RATE_NO_REPARENT,
// 	.offset		= SRC_TOP7,
// 	.shift		= 8,
// 	.width		= 2,
// 	.mux_flags	= 0,
// 	.alias		= NULL,
// }
// ARM10C 20150131
// __MUX(none, NULL, "mout_aclk400_mscl", group1_p, SRC_TOP0, 4, 2, 0, 0, "aclk400_mscl"):
// {
// 	.id		= none,
// 	.dev_name	= NULL,
// 	.name		= "mout_aclk400_mscl",
// 	.parent_names	= group1_p,
// 	.num_parents	= ARRAY_SIZE(group1_p),
// 	.flags		= (0) | CLK_SET_RATE_NO_REPARENT,
// 	.offset		= SRC_TOP0,
// 	.shift		= 4,
// 	.width		= 2,
// 	.mux_flags	= 0,
// 	.alias		= "aclk400_mscl",
// }
#define __MUX(_id, dname, cname, pnames, o, s, w, f, mf, a)	\
	{							\
		.id		= _id,				\
		.dev_name	= dname,			\
		.name		= cname,			\
		.parent_names	= pnames,			\
		.num_parents	= ARRAY_SIZE(pnames),		\
		.flags		= (f) | CLK_SET_RATE_NO_REPARENT, \
		.offset		= o,				\
		.shift		= s,				\
		.width		= w,				\
		.mux_flags	= mf,				\
		.alias		= a,				\
	}

// ARM10C 20150131
// #define __MUX(none, NULL, "mout_mspll_kfc", mspll_cpu_p, SRC_TOP7, 8, 2, 0, 0, NULL):
// {
// 	.id		= none,
// 	.dev_name	= NULL,
// 	.name		= "mout_mspll_kfc",
// 	.parent_names	= mspll_cpu_p,
// 	.num_parents	= ARRAY_SIZE(mspll_cpu_p),
// 	.flags		= (0) | CLK_SET_RATE_NO_REPARENT,
// 	.offset		= SRC_TOP7,
// 	.shift		= 8,
// 	.width		= 2,
// 	.mux_flags	= 0,
// 	.alias		= NULL,
// }
//
// #define MUX(none, "mout_mspll_kfc", mspll_cpu_p, SRC_TOP7, 8, 2):
// {
// 	.id		= none,
// 	.dev_name	= NULL,
// 	.name		= "mout_mspll_kfc",
// 	.parent_names	= mspll_cpu_p,
// 	.num_parents	= ARRAY_SIZE(mspll_cpu_p),
// 	.flags		= (0) | CLK_SET_RATE_NO_REPARENT,
// 	.offset		= SRC_TOP7,
// 	.shift		= 8,
// 	.width		= 2,
// 	.mux_flags	= 0,
// 	.alias		= NULL,
// }
#define MUX(_id, cname, pnames, o, s, w)			\
	__MUX(_id, NULL, cname, pnames, o, s, w, 0, 0, NULL)

// ARM10C 20150131
// #define __MUX(none, NULL, "mout_aclk400_mscl", group1_p, SRC_TOP0, 4, 2, 0, 0, "aclk400_mscl"):
// {
// 	.id		= none,
// 	.dev_name	= NULL,
// 	.name		= "mout_aclk400_mscl",
// 	.parent_names	= group1_p,
// 	.num_parents	= ARRAY_SIZE(group1_p),
// 	.flags		= (0) | CLK_SET_RATE_NO_REPARENT,
// 	.offset		= SRC_TOP0,
// 	.shift		= 4,
// 	.width		= 2,
// 	.mux_flags	= 0,
// 	.alias		= "aclk400_mscl",
// }
//
// #define MUX_A(none, "mout_aclk400_mscl", group1_p, SRC_TOP0, 4, 2, "aclk400_mscl"):
// {
// 	.id		= none,
// 	.dev_name	= NULL,
// 	.name		= "mout_aclk400_mscl",
// 	.parent_names	= group1_p,
// 	.num_parents	= ARRAY_SIZE(group1_p),
// 	.flags		= (0) | CLK_SET_RATE_NO_REPARENT,
// 	.offset		= SRC_TOP0,
// 	.shift		= 4,
// 	.width		= 2,
// 	.mux_flags	= 0,
// 	.alias		= "aclk400_mscl",
// }
#define MUX_A(_id, cname, pnames, o, s, w, a)			\
	__MUX(_id, NULL, cname, pnames, o, s, w, 0, 0, a)

#define MUX_F(_id, cname, pnames, o, s, w, f, mf)		\
	__MUX(_id, NULL, cname, pnames, o, s, w, f, mf, NULL)

#define MUX_FA(_id, cname, pnames, o, s, w, f, mf, a)		\
	__MUX(_id, NULL, cname, pnames, o, s, w, f, mf, a)

/**
 * @id: platform specific id of the clock.
 * struct samsung_div_clock: information about div clock
 * @dev_name: name of the device to which this clock belongs.
 * @name: name of this div clock.
 * @parent_name: name of the parent clock.
 * @flags: optional flags for basic clock.
 * @offset: offset of the register for configuring the div.
 * @shift: starting bit location of the div control bit-field in @reg.
 * @div_flags: flags for div-type clock.
 * @alias: optional clock alias name to be assigned to this clock.
 */
struct samsung_div_clock {
	unsigned int		id;
	const char		*dev_name;
	const char		*name;
	const char		*parent_name;
	unsigned long		flags;
	unsigned long		offset;
	u8			shift;
	u8			width;
	u8			div_flags;
	const char		*alias;
	struct clk_div_table	*table;
};

// ARM10C 20150228
// #define __DIV(none, NULL, "sclk_apll", "mout_apll", DIV_CPU0, 24, 3, 0, 0, NULL, NULL):
// {
//     .id		= none,
//     .dev_name	= NULL,
//     .name		= "sclk_apll",
//     .parent_name	= "mout_apll",
//     .flags		= 0,
//     .offset		= DIV_CPU0,
//     .shift		= 24,
//     .width		= 3,
//     .div_flags	= 0,
//     .alias		= NULL,
//     .table		= NULL,
// }
#define __DIV(_id, dname, cname, pname, o, s, w, f, df, a, t)	\
	{							\
		.id		= _id,				\
		.dev_name	= dname,			\
		.name		= cname,			\
		.parent_name	= pname,			\
		.flags		= f,				\
		.offset		= o,				\
		.shift		= s,				\
		.width		= w,				\
		.div_flags	= df,				\
		.alias		= a,				\
		.table		= t,				\
	}

// ARM10C 20150228
// __DIV(none, NULL, "sclk_apll", "mout_apll", DIV_CPU0, 24, 3, 0, 0, NULL, NULL):
// {
//     .id		= none,
//     .dev_name	= NULL,
//     .name		= "sclk_apll",
//     .parent_name	= "mout_apll",
//     .flags		= 0,
//     .offset		= DIV_CPU0,
//     .shift		= 24,
//     .width		= 3,
//     .div_flags	= 0,
//     .alias		= NULL,
//     .table		= NULL,
// }
//
// #define DIV(none, "sclk_apll", "mout_apll", DIV_CPU0, 24, 3):
// {
//     .id		= none,
//     .dev_name	= NULL,
//     .name		= "sclk_apll",
//     .parent_name	= "mout_apll",
//     .flags		= 0,
//     .offset		= DIV_CPU0,
//     .shift		= 24,
//     .width		= 3,
//     .div_flags	= 0,
//     .alias		= NULL,
//     .table		= NULL,
// }
#define DIV(_id, cname, pname, o, s, w)				\
	__DIV(_id, NULL, cname, pname, o, s, w, 0, 0, NULL, NULL)

#define DIV_A(_id, cname, pname, o, s, w, a)			\
	__DIV(_id, NULL, cname, pname, o, s, w, 0, 0, a, NULL)

#define DIV_F(_id, cname, pname, o, s, w, f, df)		\
	__DIV(_id, NULL, cname, pname, o, s, w, f, df, NULL, NULL)

#define DIV_T(_id, cname, pname, o, s, w, t)			\
	__DIV(_id, NULL, cname, pname, o, s, w, 0, 0, NULL, t)

/**
 * struct samsung_gate_clock: information about gate clock
 * @id: platform specific id of the clock.
 * @dev_name: name of the device to which this clock belongs.
 * @name: name of this gate clock.
 * @parent_name: name of the parent clock.
 * @flags: optional flags for basic clock.
 * @offset: offset of the register for configuring the gate.
 * @bit_idx: bit index of the gate control bit-field in @reg.
 * @gate_flags: flags for gate-type clock.
 * @alias: optional clock alias name to be assigned to this clock.
 */
struct samsung_gate_clock {
	unsigned int		id;
	const char		*dev_name;
	const char		*name;
	const char		*parent_name;
	unsigned long		flags;
	unsigned long		offset;
	u8			bit_idx;
	u8			gate_flags;
	const char		*alias;
};

// ARM10C 20150307
// #define __GATE(sclk_uart0, NULL, "sclk_uart0", "dout_uart0", GATE_TOP_SCLK_PERIC, 0, CLK_SET_RATE_PARENT, 0, NULL):
// {
//     .id              = sclk_uart0,
//     .dev_name        = NULL,
//     .name            = "sclk_uart0",
//     .parent_name     = "dout_uart0",
//     .flags           = CLK_SET_RATE_PARENT,
//     .offset          = GATE_TOP_SCLK_PERIC,
//     .bit_idx         = 0,
//     .gate_flags      = 0,
//     .alias           = NULL,
// }
#define __GATE(_id, dname, cname, pname, o, b, f, gf, a)	\
	{							\
		.id		= _id,				\
		.dev_name	= dname,			\
		.name		= cname,			\
		.parent_name	= pname,			\
		.flags		= f,				\
		.offset		= o,				\
		.bit_idx	= b,				\
		.gate_flags	= gf,				\
		.alias		= a,				\
	}

// ARM10C 20150307
// __GATE(sclk_uart0, NULL, "sclk_uart0", "dout_uart0", GATE_TOP_SCLK_PERIC, 0, CLK_SET_RATE_PARENT, 0, NULL):
// {
//     .id              = sclk_uart0,
//     .dev_name        = NULL,
//     .name            = "sclk_uart0",
//     .parent_name     = "dout_uart0",
//     .flags           = CLK_SET_RATE_PARENT,
//     .offset          = GATE_TOP_SCLK_PERIC,
//     .bit_idx         = 0,
//     .gate_flags      = 0,
//     .alias           = NULL,
// }
//
// #define GATE(sclk_uart0, "sclk_uart0", "dout_uart0", GATE_TOP_SCLK_PERIC, 0, CLK_SET_RATE_PARENT, 0):
// {
//     .id              = sclk_uart0,
//     .dev_name        = NULL,
//     .name            = "sclk_uart0",
//     .parent_name     = "dout_uart0",
//     .flags           = CLK_SET_RATE_PARENT,
//     .offset          = GATE_TOP_SCLK_PERIC,
//     .bit_idx         = 0,
//     .gate_flags      = 0,
//     .alias           = NULL,
// }
#define GATE(_id, cname, pname, o, b, f, gf)			\
	__GATE(_id, NULL, cname, pname, o, b, f, gf, NULL)

#define GATE_A(_id, cname, pname, o, b, f, gf, a)		\
	__GATE(_id, NULL, cname, pname, o, b, f, gf, a)

#define GATE_D(_id, dname, cname, pname, o, b, f, gf)		\
	__GATE(_id, dname, cname, pname, o, b, f, gf, NULL)

#define GATE_DA(_id, dname, cname, pname, o, b, f, gf, a)	\
	__GATE(_id, dname, cname, pname, o, b, f, gf, a)

// ARM10C 20150131
// #define PNAME(mspll_cpu_p):
// static const char *mspll_cpu_p[] __initdata
#define PNAME(x) static const char *x[] __initdata

/**
 * struct samsung_clk_reg_dump: register dump of clock controller registers.
 * @offset: clock register offset from the controller base address.
 * @value: the value to be register at offset.
 */
// ARM10C 20150110
// sizeof(struct samsung_clk_reg_dump): 8 bytes
struct samsung_clk_reg_dump {
	u32	offset;
	u32	value;
};

/**
 * struct samsung_pll_clock: information about pll clock
 * @id: platform specific id of the clock.
 * @dev_name: name of the device to which this clock belongs.
 * @name: name of this pll clock.
 * @parent_name: name of the parent clock.
 * @flags: optional flags for basic clock.
 * @con_offset: offset of the register for configuring the PLL.
 * @lock_offset: offset of the register for locking the PLL.
 * @type: Type of PLL to be registered.
 * @alias: optional clock alias name to be assigned to this clock.
 */
struct samsung_pll_clock {
	unsigned int		id;
	const char		*dev_name;
	const char		*name;
	const char		*parent_name;
	unsigned long		flags;
	int			con_offset;
	int			lock_offset;
	enum samsung_pll_type	type;
	const struct samsung_pll_rate_table *rate_table;
	const char              *alias;
};

// ARM10C 20150117
// #define __PLL(pll_2550, fout_apll, NULL, "fout_apll", "fin_pll", CLK_GET_RATE_NOCACHE, APLL_LOCK, APLL_CON0, NULL, "fout_apll"):
// {
// 	.id		= fout_apll,
// 	.type		= pll_2550,
// 	.dev_name	= NULL,
// 	.name		= "fout_apll",
// 	.parent_name	= "fin_pll",
// 	.flags		= CLK_GET_RATE_NOCACHE,
// 	.con_offset	= APLL_CON0,
// 	.lock_offset	= APLL_LOCK,
// 	.rate_table	= NULL,
// 	.alias		= "fout_apll",
// }
#define __PLL(_typ, _id, _dname, _name, _pname, _flags, _lock, _con,	\
		_rtable, _alias)					\
	{								\
		.id		= _id,					\
		.type		= _typ,					\
		.dev_name	= _dname,				\
		.name		= _name,				\
		.parent_name	= _pname,				\
		.flags		= CLK_GET_RATE_NOCACHE,			\
		.con_offset	= _con,					\
		.lock_offset	= _lock,				\
		.rate_table	= _rtable,				\
		.alias		= _alias,				\
	}

// ARM10C 20150117
// #define __PLL(pll_2550, fout_apll, NULL, "fout_apll", "fin_pll", CLK_GET_RATE_NOCACHE, APLL_LOCK, APLL_CON0, NULL, "fout_apll"):
// {
// 	.id		= fout_apll,
// 	.type		= pll_2550,
// 	.dev_name	= NULL,
// 	.name		= "fout_apll",
// 	.parent_name	= "fin_pll",
// 	.flags		= CLK_GET_RATE_NOCACHE,
// 	.con_offset	= APLL_CON0,
// 	.lock_offset	= APLL_LOCK,
// 	.rate_table	= NULL,
// 	.alias		= "fout_apll",
// }
//
// #define PLL(pll_2550, fout_apll, "fout_apll", "fin_pll", APLL_LOCK, APLL_CON0, NULL):
// {
// 	.id		= fout_apll,
// 	.type		= pll_2550,
// 	.dev_name	= NULL,
// 	.name		= "fout_apll",
// 	.parent_name	= "fin_pll",
// 	.flags		= CLK_GET_RATE_NOCACHE,
// 	.con_offset	= APLL_CON0,
// 	.lock_offset	= APLL_LOCK,
// 	.rate_table	= NULL,
// 	.alias		= "fout_apll",
// }
#define PLL(_typ, _id, _name, _pname, _lock, _con, _rtable)	\
	__PLL(_typ, _id, NULL, _name, _pname, CLK_GET_RATE_NOCACHE,	\
		_lock, _con, _rtable, _name)

#define PLL_A(_typ, _id, _name, _pname, _lock, _con, _alias, _rtable) \
	__PLL(_typ, _id, NULL, _name, _pname, CLK_GET_RATE_NOCACHE,	\
		_lock, _con, _rtable, _alias)

extern void __init samsung_clk_init(struct device_node *np, void __iomem *base,
		unsigned long nr_clks, unsigned long *rdump,
		unsigned long nr_rdump, unsigned long *soc_rdump,
		unsigned long nr_soc_rdump);
extern void __init samsung_clk_of_register_fixed_ext(
		struct samsung_fixed_rate_clock *fixed_rate_clk,
		unsigned int nr_fixed_rate_clk,
		struct of_device_id *clk_matches);

extern void samsung_clk_add_lookup(struct clk *clk, unsigned int id);

extern void samsung_clk_register_alias(struct samsung_clock_alias *list,
		unsigned int nr_clk);
extern void __init samsung_clk_register_fixed_rate(
		struct samsung_fixed_rate_clock *clk_list, unsigned int nr_clk);
extern void __init samsung_clk_register_fixed_factor(
		struct samsung_fixed_factor_clock *list, unsigned int nr_clk);
extern void __init samsung_clk_register_mux(struct samsung_mux_clock *clk_list,
		unsigned int nr_clk);
extern void __init samsung_clk_register_div(struct samsung_div_clock *clk_list,
		unsigned int nr_clk);
extern void __init samsung_clk_register_gate(
		struct samsung_gate_clock *clk_list, unsigned int nr_clk);
extern void __init samsung_clk_register_pll(struct samsung_pll_clock *pll_list,
		unsigned int nr_clk, void __iomem *base);

extern unsigned long _get_rate(const char *clk_name);

#endif /* __SAMSUNG_CLK_H */
