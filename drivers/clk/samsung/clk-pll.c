/*
 * Copyright (c) 2013 Samsung Electronics Co., Ltd.
 * Copyright (c) 2013 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This file contains the utility functions to register the pll clocks.
*/

#include <linux/errno.h>
#include <linux/hrtimer.h>
#include "clk.h"
#include "clk-pll.h"

#define PLL_TIMEOUT_MS		10

// ARM10C 20150117
// ARM10C 20150124
// sizeof(struct samsung_clk_pll): 28 bytes
struct samsung_clk_pll {
	struct clk_hw		hw;
	void __iomem		*lock_reg;
	void __iomem		*con_reg;
	enum samsung_pll_type	type;
	unsigned int		rate_count;
	const struct samsung_pll_rate_table *rate_table;
};

// ARM10C 20150124
// hw: &(kmem_cache#30-oX (apll))->hw
#define to_clk_pll(_hw) container_of(_hw, struct samsung_clk_pll, hw)

static const struct samsung_pll_rate_table *samsung_get_pll_settings(
				struct samsung_clk_pll *pll, unsigned long rate)
{
	const struct samsung_pll_rate_table  *rate_table = pll->rate_table;
	int i;

	for (i = 0; i < pll->rate_count; i++) {
		if (rate == rate_table[i].rate)
			return &rate_table[i];
	}

	return NULL;
}

static long samsung_pll_round_rate(struct clk_hw *hw,
			unsigned long drate, unsigned long *prate)
{
	struct samsung_clk_pll *pll = to_clk_pll(hw);
	const struct samsung_pll_rate_table *rate_table = pll->rate_table;
	int i;

	/* Assumming rate_table is in descending order */
	for (i = 0; i < pll->rate_count; i++) {
		if (drate >= rate_table[i].rate)
			return rate_table[i].rate;
	}

	/* return minimum supported value */
	return rate_table[i - 1].rate;
}

/*
 * PLL35xx Clock Type
 */
/* Maximum lock time can be 270 * PDIV cycles */
#define PLL35XX_LOCK_FACTOR	(270)

// ARM10C 20150124
// PLL35XX_MDIV_MASK: 0x3FF
#define PLL35XX_MDIV_MASK       (0x3FF)
// ARM10C 20150124
// PLL35XX_PDIV_MASK: 3F
#define PLL35XX_PDIV_MASK       (0x3F)
// ARM10C 20150124
// PLL35XX_SDIV_MASK: 0x7
#define PLL35XX_SDIV_MASK       (0x7)
#define PLL35XX_LOCK_STAT_MASK	(0x1)
// ARM10C 20150124
// PLL35XX_MDIV_SHIFT: 16
#define PLL35XX_MDIV_SHIFT      (16)
// ARM10C 20150124
// PLL35XX_PDIV_SHIFT: 8
#define PLL35XX_PDIV_SHIFT      (8)
// ARM10C 20150124
// PLL35XX_SDIV_SHIFT: 0
#define PLL35XX_SDIV_SHIFT      (0)
#define PLL35XX_LOCK_STAT_SHIFT	(29)

// ARM10C 20150117
// ARM10C 20150124
// &(kmem_cache#30-oX (apll))->hw, 24000000
static unsigned long samsung_pll35xx_recalc_rate(struct clk_hw *hw,
				unsigned long parent_rate)
{
	// hw: &(kmem_cache#30-oX (apll))->hw,
	// to_clk_pll(&(kmem_cache#30-oX (apll))->hw): kmem_cache#30-oX (apll)
	struct samsung_clk_pll *pll = to_clk_pll(hw);
	// pll: kmem_cache#30-oX (apll)

	u32 mdiv, pdiv, sdiv, pll_con;

	// parent_rate: 24000000
	u64 fvco = parent_rate;
	// fvco: 24000000

	// E.R.M: 5.9.1.2 APLL_CON0
	// Control PLL output frequency for APLL
	// exynos 5250 manual 에 나온fout을 1000Mhz 값으로 세팅하는 APLL_CON0 값 (0x007D0300) 으로
	// 읽히는 것으로 가정하고 분석 진행

	// pll->con_reg: (kmem_cache#30-oX (apll))->con_reg: 0xf0040100
	// __raw_readl(0xf0040100): 0x007D0300
	pll_con = __raw_readl(pll->con_reg);
	// pll_con: 0x007D0300

	// pll_con: 0x007D0300, PLL35XX_MDIV_SHIFT: 16, PLL35XX_MDIV_MASK: 0x3FF
	mdiv = (pll_con >> PLL35XX_MDIV_SHIFT) & PLL35XX_MDIV_MASK;
	// mdiv: 0x7D

	// pll_con: 0x007D0300, PLL35XX_PDIV_SHIFT: 8, PLL35XX_PDIV_MASK: 0x3F
	pdiv = (pll_con >> PLL35XX_PDIV_SHIFT) & PLL35XX_PDIV_MASK;
	// pdiv: 0x3

	// pll_con: 0x007D0300, PLL35XX_SDIV_SHIFT: 0, PLL35XX_SDIV_MASK: 0x7
	sdiv = (pll_con >> PLL35XX_SDIV_SHIFT) & PLL35XX_SDIV_MASK;
	// sdiv: 0x0

	// E.R.M: 5.4 Clock Generation
	// PLL 설정 관련 그림 참조

	// fvco: 24000000, mdiv: 0x7D
	fvco *= mdiv;
	// fvco: 3000000000

	// fvco: 3000000000, pdiv: 0x3, sdiv: 0x0
	// do_div(3000000000, 3): 1000000000
	do_div(fvco, (pdiv << sdiv));
	// fvco: 1000000000

	// fvco: 1000000000
	return (unsigned long)fvco;
	// return 1000000000
}

static inline bool samsung_pll35xx_mp_change(
		const struct samsung_pll_rate_table *rate, u32 pll_con)
{
	u32 old_mdiv, old_pdiv;

	old_mdiv = (pll_con >> PLL35XX_MDIV_SHIFT) & PLL35XX_MDIV_MASK;
	old_pdiv = (pll_con >> PLL35XX_PDIV_SHIFT) & PLL35XX_PDIV_MASK;

	return (rate->mdiv != old_mdiv || rate->pdiv != old_pdiv);
}

static int samsung_pll35xx_set_rate(struct clk_hw *hw, unsigned long drate,
					unsigned long prate)
{
	struct samsung_clk_pll *pll = to_clk_pll(hw);
	const struct samsung_pll_rate_table *rate;
	u32 tmp;

	/* Get required rate settings from table */
	rate = samsung_get_pll_settings(pll, drate);
	if (!rate) {
		pr_err("%s: Invalid rate : %lu for pll clk %s\n", __func__,
			drate, __clk_get_name(hw->clk));
		return -EINVAL;
	}

	tmp = __raw_readl(pll->con_reg);

	if (!(samsung_pll35xx_mp_change(rate, tmp))) {
		/* If only s change, change just s value only*/
		tmp &= ~(PLL35XX_SDIV_MASK << PLL35XX_SDIV_SHIFT);
		tmp |= rate->sdiv << PLL35XX_SDIV_SHIFT;
		__raw_writel(tmp, pll->con_reg);

		return 0;
	}

	/* Set PLL lock time. */
	__raw_writel(rate->pdiv * PLL35XX_LOCK_FACTOR,
			pll->lock_reg);

	/* Change PLL PMS values */
	tmp &= ~((PLL35XX_MDIV_MASK << PLL35XX_MDIV_SHIFT) |
			(PLL35XX_PDIV_MASK << PLL35XX_PDIV_SHIFT) |
			(PLL35XX_SDIV_MASK << PLL35XX_SDIV_SHIFT));
	tmp |= (rate->mdiv << PLL35XX_MDIV_SHIFT) |
			(rate->pdiv << PLL35XX_PDIV_SHIFT) |
			(rate->sdiv << PLL35XX_SDIV_SHIFT);
	__raw_writel(tmp, pll->con_reg);

	/* wait_lock_time */
	do {
		cpu_relax();
		tmp = __raw_readl(pll->con_reg);
	} while (!(tmp & (PLL35XX_LOCK_STAT_MASK
				<< PLL35XX_LOCK_STAT_SHIFT)));
	return 0;
}

static const struct clk_ops samsung_pll35xx_clk_ops = {
	.recalc_rate = samsung_pll35xx_recalc_rate,
	.round_rate = samsung_pll_round_rate,
	.set_rate = samsung_pll35xx_set_rate,
};

// ARM10C 20150117
static const struct clk_ops samsung_pll35xx_clk_min_ops = {
	.recalc_rate = samsung_pll35xx_recalc_rate,
};

/*
 * PLL36xx Clock Type
 */
/* Maximum lock time can be 3000 * PDIV cycles */
#define PLL36XX_LOCK_FACTOR    (3000)

// ARM10C 20150124
// PLL36XX_KDIV_MASK: 0xFFFF
#define PLL36XX_KDIV_MASK	(0xFFFF)
// ARM10C 20150124
// PLL36XX_MDIV_MASK: 0x1FF
#define PLL36XX_MDIV_MASK	(0x1FF)
// ARM10C 20150124
// PLL36XX_PDIV_MASK: 0x3F
#define PLL36XX_PDIV_MASK	(0x3F)
// ARM10C 20150124
// PLL36XX_SDIV_MASK: 0x7
#define PLL36XX_SDIV_MASK	(0x7)
// ARM10C 20150124
// PLL36XX_MDIV_SHIFT: 16
#define PLL36XX_MDIV_SHIFT	(16)
// ARM10C 20150124
// PLL36XX_PDIV_SHIFT: 8
#define PLL36XX_PDIV_SHIFT	(8)
// ARM10C 20150124
// PLL36XX_SDIV_SHIFT: 0
#define PLL36XX_SDIV_SHIFT	(0)
#define PLL36XX_KDIV_SHIFT	(0)
#define PLL36XX_LOCK_STAT_SHIFT	(29)

// ARM10C 20150124
// &(kmem_cache#30-oX (epll))->hw, 24000000
static unsigned long samsung_pll36xx_recalc_rate(struct clk_hw *hw,
				unsigned long parent_rate)
{
	// hw: &(kmem_cache#30-oX (epll))->hw,
	// to_clk_pll(&(kmem_cache#30-oX (epll))->hw): kmem_cache#30-oX (epll)
	struct samsung_clk_pll *pll = to_clk_pll(hw);
	// pll: kmem_cache#30-oX (epll)

	u32 mdiv, pdiv, sdiv, pll_con0, pll_con1;
	s16 kdiv;

	// parent_rate: 24000000
	u64 fvco = parent_rate;
	// fvco: 24000000

	// E.R.M: 5.9.1.81 EPLL_CON0
	// Control PLL output frequency for EPLL
	// EPLL_CON0, EPLL_CON1은 reset값 기준으로 분석 수행

	// pll->con_reg: (kmem_cache#30-oX (epll))->con_reg: 0xf0050130
	// __raw_readl(0xf0050130): 0x00300301
	pll_con0 = __raw_readl(pll->con_reg);
	// pll_con0: 0x00300301

	// pll->con_reg: (kmem_cache#30-oX (epll))->con_reg: 0xf0050130
	// __raw_readl(0xf0050134): 0x0
	pll_con1 = __raw_readl(pll->con_reg + 4);
	// pll_con1: 0x0

	// pll_con0: 0x00300301, PLL36XX_MDIV_SHIFT: 16, PLL36XX_MDIV_MASK: 0x1FF
	mdiv = (pll_con0 >> PLL36XX_MDIV_SHIFT) & PLL36XX_MDIV_MASK;
	// mdiv: 0x30

	// pll_con0: 0x00300301, PLL36XX_PDIV_SHIFT: 8, PLL36XX_PDIV_MASK: 0x3F
	pdiv = (pll_con0 >> PLL36XX_PDIV_SHIFT) & PLL36XX_PDIV_MASK;
	// pdiv: 0x3

	// pll_con0: 0x00300301, PLL36XX_SDIV_SHIFT: 0, PLL36XX_SDIV_MASK: 0x7
	sdiv = (pll_con0 >> PLL36XX_SDIV_SHIFT) & PLL36XX_SDIV_MASK;
	// sdiv: 0x1

	// pll_con1: 0x0, PLL36XX_KDIV_MASK: 0xFFFF
	kdiv = (s16)(pll_con1 & PLL36XX_KDIV_MASK);
	// kdiv: 0

	// fvco: 24000000, mdiv: 0x30, kdiv: 0
	fvco *= (mdiv << 16) + kdiv;
	// fvco: 75497232000000

	// fvco: 75497232000000, pdiv: 0x3, sdiv: 0x1
	do_div(fvco, (pdiv << sdiv));
	// fvco: 12582872000000

	// fvco: 12582872000000
	fvco >>= 16;
	// fvco: 191999389

	// fvco: 191999389
	return (unsigned long)fvco;
	// return 191999389
}

static inline bool samsung_pll36xx_mpk_change(
	const struct samsung_pll_rate_table *rate, u32 pll_con0, u32 pll_con1)
{
	u32 old_mdiv, old_pdiv, old_kdiv;

	old_mdiv = (pll_con0 >> PLL36XX_MDIV_SHIFT) & PLL36XX_MDIV_MASK;
	old_pdiv = (pll_con0 >> PLL36XX_PDIV_SHIFT) & PLL36XX_PDIV_MASK;
	old_kdiv = (pll_con1 >> PLL36XX_KDIV_SHIFT) & PLL36XX_KDIV_MASK;

	return (rate->mdiv != old_mdiv || rate->pdiv != old_pdiv ||
		rate->kdiv != old_kdiv);
}

static int samsung_pll36xx_set_rate(struct clk_hw *hw, unsigned long drate,
					unsigned long parent_rate)
{
	struct samsung_clk_pll *pll = to_clk_pll(hw);
	u32 tmp, pll_con0, pll_con1;
	const struct samsung_pll_rate_table *rate;

	rate = samsung_get_pll_settings(pll, drate);
	if (!rate) {
		pr_err("%s: Invalid rate : %lu for pll clk %s\n", __func__,
			drate, __clk_get_name(hw->clk));
		return -EINVAL;
	}

	pll_con0 = __raw_readl(pll->con_reg);
	pll_con1 = __raw_readl(pll->con_reg + 4);

	if (!(samsung_pll36xx_mpk_change(rate, pll_con0, pll_con1))) {
		/* If only s change, change just s value only*/
		pll_con0 &= ~(PLL36XX_SDIV_MASK << PLL36XX_SDIV_SHIFT);
		pll_con0 |= (rate->sdiv << PLL36XX_SDIV_SHIFT);
		__raw_writel(pll_con0, pll->con_reg);

		return 0;
	}

	/* Set PLL lock time. */
	__raw_writel(rate->pdiv * PLL36XX_LOCK_FACTOR, pll->lock_reg);

	 /* Change PLL PMS values */
	pll_con0 &= ~((PLL36XX_MDIV_MASK << PLL36XX_MDIV_SHIFT) |
			(PLL36XX_PDIV_MASK << PLL36XX_PDIV_SHIFT) |
			(PLL36XX_SDIV_MASK << PLL36XX_SDIV_SHIFT));
	pll_con0 |= (rate->mdiv << PLL36XX_MDIV_SHIFT) |
			(rate->pdiv << PLL36XX_PDIV_SHIFT) |
			(rate->sdiv << PLL36XX_SDIV_SHIFT);
	__raw_writel(pll_con0, pll->con_reg);

	pll_con1 &= ~(PLL36XX_KDIV_MASK << PLL36XX_KDIV_SHIFT);
	pll_con1 |= rate->kdiv << PLL36XX_KDIV_SHIFT;
	__raw_writel(pll_con1, pll->con_reg + 4);

	/* wait_lock_time */
	do {
		cpu_relax();
		tmp = __raw_readl(pll->con_reg);
	} while (!(tmp & (1 << PLL36XX_LOCK_STAT_SHIFT)));

	return 0;
}

static const struct clk_ops samsung_pll36xx_clk_ops = {
	.recalc_rate = samsung_pll36xx_recalc_rate,
	.set_rate = samsung_pll36xx_set_rate,
	.round_rate = samsung_pll_round_rate,
};

// ARM10C 20150124
static const struct clk_ops samsung_pll36xx_clk_min_ops = {
	.recalc_rate = samsung_pll36xx_recalc_rate,
};

/*
 * PLL45xx Clock Type
 */
#define PLL4502_LOCK_FACTOR	400
#define PLL4508_LOCK_FACTOR	240

#define PLL45XX_MDIV_MASK	(0x3FF)
#define PLL45XX_PDIV_MASK	(0x3F)
#define PLL45XX_SDIV_MASK	(0x7)
#define PLL45XX_AFC_MASK	(0x1F)
#define PLL45XX_MDIV_SHIFT	(16)
#define PLL45XX_PDIV_SHIFT	(8)
#define PLL45XX_SDIV_SHIFT	(0)
#define PLL45XX_AFC_SHIFT	(0)

#define PLL45XX_ENABLE		BIT(31)
#define PLL45XX_LOCKED		BIT(29)

static unsigned long samsung_pll45xx_recalc_rate(struct clk_hw *hw,
				unsigned long parent_rate)
{
	struct samsung_clk_pll *pll = to_clk_pll(hw);
	u32 mdiv, pdiv, sdiv, pll_con;
	u64 fvco = parent_rate;

	pll_con = __raw_readl(pll->con_reg);
	mdiv = (pll_con >> PLL45XX_MDIV_SHIFT) & PLL45XX_MDIV_MASK;
	pdiv = (pll_con >> PLL45XX_PDIV_SHIFT) & PLL45XX_PDIV_MASK;
	sdiv = (pll_con >> PLL45XX_SDIV_SHIFT) & PLL45XX_SDIV_MASK;

	if (pll->type == pll_4508)
		sdiv = sdiv - 1;

	fvco *= mdiv;
	do_div(fvco, (pdiv << sdiv));

	return (unsigned long)fvco;
}

static bool samsung_pll45xx_mp_change(u32 pll_con0, u32 pll_con1,
				const struct samsung_pll_rate_table *rate)
{
	u32 old_mdiv, old_pdiv, old_afc;

	old_mdiv = (pll_con0 >> PLL45XX_MDIV_SHIFT) & PLL45XX_MDIV_MASK;
	old_pdiv = (pll_con0 >> PLL45XX_PDIV_SHIFT) & PLL45XX_PDIV_MASK;
	old_afc = (pll_con1 >> PLL45XX_AFC_SHIFT) & PLL45XX_AFC_MASK;

	return (old_mdiv != rate->mdiv || old_pdiv != rate->pdiv
		|| old_afc != rate->afc);
}

static int samsung_pll45xx_set_rate(struct clk_hw *hw, unsigned long drate,
					unsigned long prate)
{
	struct samsung_clk_pll *pll = to_clk_pll(hw);
	const struct samsung_pll_rate_table *rate;
	u32 con0, con1;
	ktime_t start;

	/* Get required rate settings from table */
	rate = samsung_get_pll_settings(pll, drate);
	if (!rate) {
		pr_err("%s: Invalid rate : %lu for pll clk %s\n", __func__,
			drate, __clk_get_name(hw->clk));
		return -EINVAL;
	}

	con0 = __raw_readl(pll->con_reg);
	con1 = __raw_readl(pll->con_reg + 0x4);

	if (!(samsung_pll45xx_mp_change(con0, con1, rate))) {
		/* If only s change, change just s value only*/
		con0 &= ~(PLL45XX_SDIV_MASK << PLL45XX_SDIV_SHIFT);
		con0 |= rate->sdiv << PLL45XX_SDIV_SHIFT;
		__raw_writel(con0, pll->con_reg);

		return 0;
	}

	/* Set PLL PMS values. */
	con0 &= ~((PLL45XX_MDIV_MASK << PLL45XX_MDIV_SHIFT) |
			(PLL45XX_PDIV_MASK << PLL45XX_PDIV_SHIFT) |
			(PLL45XX_SDIV_MASK << PLL45XX_SDIV_SHIFT));
	con0 |= (rate->mdiv << PLL45XX_MDIV_SHIFT) |
			(rate->pdiv << PLL45XX_PDIV_SHIFT) |
			(rate->sdiv << PLL45XX_SDIV_SHIFT);

	/* Set PLL AFC value. */
	con1 = __raw_readl(pll->con_reg + 0x4);
	con1 &= ~(PLL45XX_AFC_MASK << PLL45XX_AFC_SHIFT);
	con1 |= (rate->afc << PLL45XX_AFC_SHIFT);

	/* Set PLL lock time. */
	switch (pll->type) {
	case pll_4502:
		__raw_writel(rate->pdiv * PLL4502_LOCK_FACTOR, pll->lock_reg);
		break;
	case pll_4508:
		__raw_writel(rate->pdiv * PLL4508_LOCK_FACTOR, pll->lock_reg);
		break;
	default:
		break;
	};

	/* Set new configuration. */
	__raw_writel(con1, pll->con_reg + 0x4);
	__raw_writel(con0, pll->con_reg);

	/* Wait for locking. */
	start = ktime_get();
	while (!(__raw_readl(pll->con_reg) & PLL45XX_LOCKED)) {
		ktime_t delta = ktime_sub(ktime_get(), start);

		if (ktime_to_ms(delta) > PLL_TIMEOUT_MS) {
			pr_err("%s: could not lock PLL %s\n",
					__func__, __clk_get_name(hw->clk));
			return -EFAULT;
		}

		cpu_relax();
	}

	return 0;
}

static const struct clk_ops samsung_pll45xx_clk_ops = {
	.recalc_rate = samsung_pll45xx_recalc_rate,
	.round_rate = samsung_pll_round_rate,
	.set_rate = samsung_pll45xx_set_rate,
};

static const struct clk_ops samsung_pll45xx_clk_min_ops = {
	.recalc_rate = samsung_pll45xx_recalc_rate,
};

/*
 * PLL46xx Clock Type
 */
#define PLL46XX_LOCK_FACTOR	3000

#define PLL46XX_VSEL_MASK	(1)
#define PLL46XX_MDIV_MASK	(0x1FF)
#define PLL46XX_PDIV_MASK	(0x3F)
#define PLL46XX_SDIV_MASK	(0x7)
#define PLL46XX_VSEL_SHIFT	(27)
#define PLL46XX_MDIV_SHIFT	(16)
#define PLL46XX_PDIV_SHIFT	(8)
#define PLL46XX_SDIV_SHIFT	(0)

#define PLL46XX_KDIV_MASK	(0xFFFF)
#define PLL4650C_KDIV_MASK	(0xFFF)
#define PLL46XX_KDIV_SHIFT	(0)
#define PLL46XX_MFR_MASK	(0x3F)
#define PLL46XX_MRR_MASK	(0x1F)
#define PLL46XX_KDIV_SHIFT	(0)
#define PLL46XX_MFR_SHIFT	(16)
#define PLL46XX_MRR_SHIFT	(24)

#define PLL46XX_ENABLE		BIT(31)
#define PLL46XX_LOCKED		BIT(29)
#define PLL46XX_VSEL		BIT(27)

static unsigned long samsung_pll46xx_recalc_rate(struct clk_hw *hw,
				unsigned long parent_rate)
{
	struct samsung_clk_pll *pll = to_clk_pll(hw);
	u32 mdiv, pdiv, sdiv, kdiv, pll_con0, pll_con1, shift;
	u64 fvco = parent_rate;

	pll_con0 = __raw_readl(pll->con_reg);
	pll_con1 = __raw_readl(pll->con_reg + 4);
	mdiv = (pll_con0 >> PLL46XX_MDIV_SHIFT) & PLL46XX_MDIV_MASK;
	pdiv = (pll_con0 >> PLL46XX_PDIV_SHIFT) & PLL46XX_PDIV_MASK;
	sdiv = (pll_con0 >> PLL46XX_SDIV_SHIFT) & PLL46XX_SDIV_MASK;
	kdiv = pll->type == pll_4650c ? pll_con1 & PLL4650C_KDIV_MASK :
					pll_con1 & PLL46XX_KDIV_MASK;

	shift = pll->type == pll_4600 ? 16 : 10;
	fvco *= (mdiv << shift) + kdiv;
	do_div(fvco, (pdiv << sdiv));
	fvco >>= shift;

	return (unsigned long)fvco;
}

static bool samsung_pll46xx_mpk_change(u32 pll_con0, u32 pll_con1,
				const struct samsung_pll_rate_table *rate)
{
	u32 old_mdiv, old_pdiv, old_kdiv;

	old_mdiv = (pll_con0 >> PLL46XX_MDIV_SHIFT) & PLL46XX_MDIV_MASK;
	old_pdiv = (pll_con0 >> PLL46XX_PDIV_SHIFT) & PLL46XX_PDIV_MASK;
	old_kdiv = (pll_con1 >> PLL46XX_KDIV_SHIFT) & PLL46XX_KDIV_MASK;

	return (old_mdiv != rate->mdiv || old_pdiv != rate->pdiv
		|| old_kdiv != rate->kdiv);
}

static int samsung_pll46xx_set_rate(struct clk_hw *hw, unsigned long drate,
					unsigned long prate)
{
	struct samsung_clk_pll *pll = to_clk_pll(hw);
	const struct samsung_pll_rate_table *rate;
	u32 con0, con1, lock;
	ktime_t start;

	/* Get required rate settings from table */
	rate = samsung_get_pll_settings(pll, drate);
	if (!rate) {
		pr_err("%s: Invalid rate : %lu for pll clk %s\n", __func__,
			drate, __clk_get_name(hw->clk));
		return -EINVAL;
	}

	con0 = __raw_readl(pll->con_reg);
	con1 = __raw_readl(pll->con_reg + 0x4);

	if (!(samsung_pll46xx_mpk_change(con0, con1, rate))) {
		/* If only s change, change just s value only*/
		con0 &= ~(PLL46XX_SDIV_MASK << PLL46XX_SDIV_SHIFT);
		con0 |= rate->sdiv << PLL46XX_SDIV_SHIFT;
		__raw_writel(con0, pll->con_reg);

		return 0;
	}

	/* Set PLL lock time. */
	lock = rate->pdiv * PLL46XX_LOCK_FACTOR;
	if (lock > 0xffff)
		/* Maximum lock time bitfield is 16-bit. */
		lock = 0xffff;

	/* Set PLL PMS and VSEL values. */
	con0 &= ~((PLL46XX_MDIV_MASK << PLL46XX_MDIV_SHIFT) |
			(PLL46XX_PDIV_MASK << PLL46XX_PDIV_SHIFT) |
			(PLL46XX_SDIV_MASK << PLL46XX_SDIV_SHIFT) |
			(PLL46XX_VSEL_MASK << PLL46XX_VSEL_SHIFT));
	con0 |= (rate->mdiv << PLL46XX_MDIV_SHIFT) |
			(rate->pdiv << PLL46XX_PDIV_SHIFT) |
			(rate->sdiv << PLL46XX_SDIV_SHIFT) |
			(rate->vsel << PLL46XX_VSEL_SHIFT);

	/* Set PLL K, MFR and MRR values. */
	con1 = __raw_readl(pll->con_reg + 0x4);
	con1 &= ~((PLL46XX_KDIV_MASK << PLL46XX_KDIV_SHIFT) |
			(PLL46XX_MFR_MASK << PLL46XX_MFR_SHIFT) |
			(PLL46XX_MRR_MASK << PLL46XX_MRR_SHIFT));
	con1 |= (rate->kdiv << PLL46XX_KDIV_SHIFT) |
			(rate->mfr << PLL46XX_MFR_SHIFT) |
			(rate->mrr << PLL46XX_MRR_SHIFT);

	/* Write configuration to PLL */
	__raw_writel(lock, pll->lock_reg);
	__raw_writel(con0, pll->con_reg);
	__raw_writel(con1, pll->con_reg + 0x4);

	/* Wait for locking. */
	start = ktime_get();
	while (!(__raw_readl(pll->con_reg) & PLL46XX_LOCKED)) {
		ktime_t delta = ktime_sub(ktime_get(), start);

		if (ktime_to_ms(delta) > PLL_TIMEOUT_MS) {
			pr_err("%s: could not lock PLL %s\n",
					__func__, __clk_get_name(hw->clk));
			return -EFAULT;
		}

		cpu_relax();
	}

	return 0;
}

static const struct clk_ops samsung_pll46xx_clk_ops = {
	.recalc_rate = samsung_pll46xx_recalc_rate,
	.round_rate = samsung_pll_round_rate,
	.set_rate = samsung_pll46xx_set_rate,
};

static const struct clk_ops samsung_pll46xx_clk_min_ops = {
	.recalc_rate = samsung_pll46xx_recalc_rate,
};

/*
 * PLL6552 Clock Type
 */

#define PLL6552_MDIV_MASK	0x3ff
#define PLL6552_PDIV_MASK	0x3f
#define PLL6552_SDIV_MASK	0x7
#define PLL6552_MDIV_SHIFT	16
#define PLL6552_PDIV_SHIFT	8
#define PLL6552_SDIV_SHIFT	0

static unsigned long samsung_pll6552_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct samsung_clk_pll *pll = to_clk_pll(hw);
	u32 mdiv, pdiv, sdiv, pll_con;
	u64 fvco = parent_rate;

	pll_con = __raw_readl(pll->con_reg);
	mdiv = (pll_con >> PLL6552_MDIV_SHIFT) & PLL6552_MDIV_MASK;
	pdiv = (pll_con >> PLL6552_PDIV_SHIFT) & PLL6552_PDIV_MASK;
	sdiv = (pll_con >> PLL6552_SDIV_SHIFT) & PLL6552_SDIV_MASK;

	fvco *= mdiv;
	do_div(fvco, (pdiv << sdiv));

	return (unsigned long)fvco;
}

static const struct clk_ops samsung_pll6552_clk_ops = {
	.recalc_rate = samsung_pll6552_recalc_rate,
};

/*
 * PLL6553 Clock Type
 */

#define PLL6553_MDIV_MASK	0xff
#define PLL6553_PDIV_MASK	0x3f
#define PLL6553_SDIV_MASK	0x7
#define PLL6553_KDIV_MASK	0xffff
#define PLL6553_MDIV_SHIFT	16
#define PLL6553_PDIV_SHIFT	8
#define PLL6553_SDIV_SHIFT	0
#define PLL6553_KDIV_SHIFT	0

static unsigned long samsung_pll6553_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct samsung_clk_pll *pll = to_clk_pll(hw);
	u32 mdiv, pdiv, sdiv, kdiv, pll_con0, pll_con1;
	u64 fvco = parent_rate;

	pll_con0 = __raw_readl(pll->con_reg);
	pll_con1 = __raw_readl(pll->con_reg + 0x4);
	mdiv = (pll_con0 >> PLL6553_MDIV_SHIFT) & PLL6553_MDIV_MASK;
	pdiv = (pll_con0 >> PLL6553_PDIV_SHIFT) & PLL6553_PDIV_MASK;
	sdiv = (pll_con0 >> PLL6553_SDIV_SHIFT) & PLL6553_SDIV_MASK;
	kdiv = (pll_con1 >> PLL6553_KDIV_SHIFT) & PLL6553_KDIV_MASK;

	fvco *= (mdiv << 16) + kdiv;
	do_div(fvco, (pdiv << sdiv));
	fvco >>= 16;

	return (unsigned long)fvco;
}

static const struct clk_ops samsung_pll6553_clk_ops = {
	.recalc_rate = samsung_pll6553_recalc_rate,
};

/*
 * PLL2550x Clock Type
 */

#define PLL2550X_R_MASK       (0x1)
#define PLL2550X_P_MASK       (0x3F)
#define PLL2550X_M_MASK       (0x3FF)
#define PLL2550X_S_MASK       (0x7)
#define PLL2550X_R_SHIFT      (20)
#define PLL2550X_P_SHIFT      (14)
#define PLL2550X_M_SHIFT      (4)
#define PLL2550X_S_SHIFT      (0)

struct samsung_clk_pll2550x {
	struct clk_hw		hw;
	const void __iomem	*reg_base;
	unsigned long		offset;
};

#define to_clk_pll2550x(_hw) container_of(_hw, struct samsung_clk_pll2550x, hw)

static unsigned long samsung_pll2550x_recalc_rate(struct clk_hw *hw,
				unsigned long parent_rate)
{
	struct samsung_clk_pll2550x *pll = to_clk_pll2550x(hw);
	u32 r, p, m, s, pll_stat;
	u64 fvco = parent_rate;

	pll_stat = __raw_readl(pll->reg_base + pll->offset * 3);
	r = (pll_stat >> PLL2550X_R_SHIFT) & PLL2550X_R_MASK;
	if (!r)
		return 0;
	p = (pll_stat >> PLL2550X_P_SHIFT) & PLL2550X_P_MASK;
	m = (pll_stat >> PLL2550X_M_SHIFT) & PLL2550X_M_MASK;
	s = (pll_stat >> PLL2550X_S_SHIFT) & PLL2550X_S_MASK;

	fvco *= m;
	do_div(fvco, (p << s));

	return (unsigned long)fvco;
}

static const struct clk_ops samsung_pll2550x_clk_ops = {
	.recalc_rate = samsung_pll2550x_recalc_rate,
};

struct clk * __init samsung_clk_register_pll2550x(const char *name,
			const char *pname, const void __iomem *reg_base,
			const unsigned long offset)
{
	struct samsung_clk_pll2550x *pll;
	struct clk *clk;
	struct clk_init_data init;

	pll = kzalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll) {
		pr_err("%s: could not allocate pll clk %s\n", __func__, name);
		return NULL;
	}

	init.name = name;
	init.ops = &samsung_pll2550x_clk_ops;
	init.flags = CLK_GET_RATE_NOCACHE;
	init.parent_names = &pname;
	init.num_parents = 1;

	pll->hw.init = &init;
	pll->reg_base = reg_base;
	pll->offset = offset;

	clk = clk_register(NULL, &pll->hw);
	if (IS_ERR(clk)) {
		pr_err("%s: failed to register pll clock %s\n", __func__,
				name);
		kfree(pll);
	}

	if (clk_register_clkdev(clk, name, NULL))
		pr_err("%s: failed to register lookup for %s", __func__, name);

	return clk;
}

// ARM10C 20150117
// &pll_list[0]: &exynos5420_plls[0], base: 0xf0040000
// ARM10C 20150124
// &pll_list[3]: &exynos5420_plls[3], base: 0xf0040000
static void __init _samsung_clk_register_pll(struct samsung_pll_clock *pll_clk,
						void __iomem *base)
{
	struct samsung_clk_pll *pll;
	struct clk *clk;
	struct clk_init_data init;
	int ret, len;

	// sizeof(struct samsung_clk_pll): 28 bytes, GFP_KERNEL: 0xD0
	// kzalloc(28, GFP_KERNEL: 0xD0): kmem_cache#30-oX (apll)
	// sizeof(struct samsung_clk_pll): 28 bytes, GFP_KERNEL: 0xD0
	// kzalloc(28, GFP_KERNEL: 0xD0): kmem_cache#30-oX (epll)
	pll = kzalloc(sizeof(*pll), GFP_KERNEL);
	// pll: kmem_cache#30-oX (apll)
	// pll: kmem_cache#30-oX (epll)

	// pll: kmem_cache#30-oX (apll)
	// pll: kmem_cache#30-oX (epll)
	if (!pll) {
		pr_err("%s: could not allocate pll clk %s\n",
			__func__, pll_clk->name);
		return;
	}

	// pll_clk->name: (&exynos5420_plls[0])->name: "fout_apll"
	// pll_clk->name: (&exynos5420_plls[3])->name: "fout_epll"
	init.name = pll_clk->name;
	// init.name: "fout_apll"
	// init.name: "fout_epll"

	// pll_clk->flags: (&exynos5420_plls[0])->flags: CLK_GET_RATE_NOCACHE: 0x40
	// pll_clk->flags: (&exynos5420_plls[3])->flags: CLK_GET_RATE_NOCACHE: 0x40
	init.flags = pll_clk->flags;
	// init.flags: CLK_GET_RATE_NOCACHE: 0x40
	// init.flags: CLK_GET_RATE_NOCACHE: 0x40

	// pll_clk->parent_name: (&exynos5420_plls[0])->parent_name: "fin_pll"
	// pll_clk->parent_name: (&exynos5420_plls[3])->parent_name: "fin_pll"
	init.parent_names = &pll_clk->parent_name;
	// init.parent_names: "fin_pll"
	// init.parent_names: "fin_pll"

	init.num_parents = 1;
	// init.num_parents: 1
	// init.num_parents: 1

	// pll_clk->rate_table: (&exynos5420_plls[0])->rate_table: NULL
	// pll_clk->rate_table: (&exynos5420_plls[3])->rate_table: NULL
	if (pll_clk->rate_table) {
		/* find count of rates in rate_table */
		for (len = 0; pll_clk->rate_table[len].rate != 0; )
			len++;

		pll->rate_count = len;
		pll->rate_table = kmemdup(pll_clk->rate_table,
					pll->rate_count *
					sizeof(struct samsung_pll_rate_table),
					GFP_KERNEL);
		WARN(!pll->rate_table,
			"%s: could not allocate rate table for %s\n",
			__func__, pll_clk->name);
	}

	// pll_clk->type: (&exynos5420_plls[0])->type: pll_2550: 2
	// pll_clk->type: (&exynos5420_plls[3])->type: pll_2650: 3
	switch (pll_clk->type) {
	/* clk_ops for 35xx and 2550 are similar */
	case pll_35xx:
	case pll_2550:
		// pll->rate_table: (kmem_cache#30-oX (apll))->rate_table: NULL
		if (!pll->rate_table)
			init.ops = &samsung_pll35xx_clk_min_ops;
			// init.ops: &samsung_pll35xx_clk_min_ops
		else
			init.ops = &samsung_pll35xx_clk_ops;
		break;
		// break

	case pll_4500:
		init.ops = &samsung_pll45xx_clk_min_ops;
		break;
	case pll_4502:
	case pll_4508:
		if (!pll->rate_table)
			init.ops = &samsung_pll45xx_clk_min_ops;
		else
			init.ops = &samsung_pll45xx_clk_ops;
		break;
	/* clk_ops for 36xx and 2650 are similar */
	case pll_36xx:
	case pll_2650:
		// pll->rate_table: (kmem_cache#30-oX (epll))->rate_table: NULL
		if (!pll->rate_table)
			init.ops = &samsung_pll36xx_clk_min_ops;
			// init.ops: &samsung_pll36xx_clk_min_ops
		else
			init.ops = &samsung_pll36xx_clk_ops;
		break;
		// break

	case pll_6552:
		init.ops = &samsung_pll6552_clk_ops;
		break;
	case pll_6553:
		init.ops = &samsung_pll6553_clk_ops;
		break;
	case pll_4600:
	case pll_4650:
	case pll_4650c:
		if (!pll->rate_table)
			init.ops = &samsung_pll46xx_clk_min_ops;
		else
			init.ops = &samsung_pll46xx_clk_ops;
		break;
	default:
		pr_warn("%s: Unknown pll type for pll clk %s\n",
			__func__, pll_clk->name);
	}

	// pll->hw.init: (kmem_cache#30-oX (apll))->hw.init
	// pll->hw.init: (kmem_cache#30-oX (epll))->hw.init
	pll->hw.init = &init;
	// pll->hw.init: (kmem_cache#30-oX (apll))->hw.init: &init
	// pll->hw.init: (kmem_cache#30-oX (epll))->hw.init: &init

	// pll->type: (kmem_cache#30-oX (apll))->type, pll_clk->type: (&exynos5420_plls[0])->type: pll_2550: 2
	// pll->type: (kmem_cache#30-oX (apll))->type, pll_clk->type: (&exynos5420_plls[3])->type: pll_2650: 3
	pll->type = pll_clk->type;
	// pll->type: (kmem_cache#30-oX (apll))->type: pll_2550: 2
	// pll->type: (kmem_cache#30-oX (epll))->type: pll_2650: 3

	// pll->lock_reg: (kmem_cache#30-oX (apll))->lock_reg, base: 0xf0040000,
	// pll_clk->lock_offset: (&exynos5420_plls[0])->lock_offset: APLL_LOCK: 0
	// pll->lock_reg: (kmem_cache#30-oX (epll))->lock_reg, base: 0xf0040000,
	// pll_clk->lock_offset: (&exynos5420_plls[0])->lock_offset: EPLL_LOCK: 0x10040
	pll->lock_reg = base + pll_clk->lock_offset;
	// pll->lock_reg: (kmem_cache#30-oX (apll))->lock_reg: 0xf0040000
	// pll->lock_reg: (kmem_cache#30-oX (epll))->lock_reg: 0xf0050040

	// pll->con_reg: (kmem_cache#30-oX (apll))->con_reg, base: 0xf0040000,
	// pll_clk->con_offset: (&exynos5420_plls[0])->con_offset: APLL_CON0: 0x100
	// pll->con_reg: (kmem_cache#30-oX (epll))->con_reg, base: 0xf0040000,
	// pll_clk->con_offset: (&exynos5420_plls[0])->con_offset: EPLL_CON0: 0x10130
	pll->con_reg = base + pll_clk->con_offset;
	// pll->con_reg: (kmem_cache#30-oX (apll))->con_reg: 0xf0040100
	// pll->con_reg: (kmem_cache#30-oX (epll))->con_reg: 0xf0050130

	// &pll->hw: &(kmem_cache#30-oX (apll))->hw
	// clk_register(&(kmem_cache#30-oX (apll))->hw): kmem_cache#29-oX (apll)
	// &pll->hw: &(kmem_cache#30-oX (epll))->hw
	// clk_register(&(kmem_cache#30-oX (epll))->hw): kmem_cache#29-oX (epll)
	clk = clk_register(NULL, &pll->hw);
	// clk: kmem_cache#29-oX (apll)
	// clk: kmem_cache#29-oX (epll)

	// clk_register(apll)에서 한일:
	// struct clk 만큼 메모리를 kmem_cache#29-oX (apll) 할당 받고 struct clk 의 멤버 값을 아래와 같이 초기화 수행
	//
	// (kmem_cache#29-oX (apll))->name: kmem_cache#30-oX ("fout_apll")
	// (kmem_cache#29-oX (apll))->ops: &samsung_pll35xx_clk_min_ops
	// (kmem_cache#29-oX (apll))->hw: &(kmem_cache#30-oX (apll))->hw
	// (kmem_cache#29-oX (apll))->flags: 0x40
	// (kmem_cache#29-oX (apll))->num_parents: 1
	// (kmem_cache#29-oX (apll))->parent_names: kmem_cache#30-oX
	// (kmem_cache#29-oX (apll))->parent_names[0]: (kmem_cache#30-oX)[0]: kmem_cache#30-oX: "fin_pll"
	// (kmem_cache#29-oX (apll))->parent: kmem_cache#29-oX (fin_pll)
	// (kmem_cache#29-oX (apll))->rate: 1000000000 (1 Ghz)
	//
	// (&(kmem_cache#29-oX (apll))->child_node)->next: NULL
	// (&(kmem_cache#29-oX (apll))->child_node)->pprev: &(&(kmem_cache#29-oX (apll))->child_node)
	//
	// (&(kmem_cache#29-oX (fin_pll))->children)->first: &(kmem_cache#29-oX (apll))->child_node
	//
	// (&(kmem_cache#30-oX (apll))->hw)->clk: kmem_cache#29-oX (apll)

	// clk_register(epll)에서 한일:
	// struct clk 만큼 메모리를 kmem_cache#29-oX (epll) 할당 받고 struct clk 의 멤버 값을 아래와 같이 초기화 수행
	//
	// (kmem_cache#29-oX (epll))->name: kmem_cache#30-oX ("fout_epll")
	// (kmem_cache#29-oX (epll))->ops: &samsung_pll36xx_clk_min_ops
	// (kmem_cache#29-oX (epll))->hw: &(kmem_cache#30-oX (epll))->hw
	// (kmem_cache#29-oX (epll))->flags: 0x40
	// (kmem_cache#29-oX (epll))->num_parents: 1
	// (kmem_cache#29-oX (epll))->parent_names: kmem_cache#30-oX
	// (kmem_cache#29-oX (epll))->parent_names[0]: (kmem_cache#30-oX)[0]: kmem_cache#30-oX: "fin_pll"
	// (kmem_cache#29-oX (epll))->parent: kmem_cache#29-oX (fin_pll)
	// (kmem_cache#29-oX (epll))->rate: 191999389
	//
	// (&(kmem_cache#29-oX (epll))->child_node)->next: NULL
	// (&(kmem_cache#29-oX (epll))->child_node)->pprev: &(&(kmem_cache#29-oX (epll))->child_node)
	//
	// (&(kmem_cache#29-oX (fin_pll))->children)->first: &(kmem_cache#29-oX (epll))->child_node
	//
	// (&(kmem_cache#30-oX (epll))->hw)->clk: kmem_cache#29-oX (epll)

	// clk: kmem_cache#29-oX (apll), IS_ERR(kmem_cache#29-oX (apll)): 0
	// clk: kmem_cache#29-oX (epll), IS_ERR(kmem_cache#29-oX (epll)): 0
	if (IS_ERR(clk)) {
		pr_err("%s: failed to register pll clock %s : %ld\n",
			__func__, pll_clk->name, PTR_ERR(clk));
		kfree(pll);
		return;
	}

	// clk: kmem_cache#29-oX (apll), pll_clk->id: (&exynos5420_plls[0])->id: fout_apll: 2
	// clk: kmem_cache#29-oX (epll), pll_clk->id: (&exynos5420_plls[3])->id: fout_epll: 5
	samsung_clk_add_lookup(clk, pll_clk->id);

	// samsung_clk_add_lookup에서 한일:
	// clk_table[2]: (kmem_cache#23-o0)[2]: kmem_cache#29-oX (apll)

	// samsung_clk_add_lookup에서 한일:
	// clk_table[5]: (kmem_cache#23-o0)[5]: kmem_cache#29-oX (epll)

	// pll_clk->alias: (&exynos5420_plls[0])->alias: "fout_apll"
	// pll_clk->alias: (&exynos5420_plls[3])->alias: "fout_epll"
	if (!pll_clk->alias)
		return;

	// clk: kmem_cache#29-oX (apll),
	// pll_clk->alias: (&exynos5420_plls[0])->alias: "fout_apll",
	// pll_clk->dev_name: (&exynos5420_plls[0])->dev_name: NULL
	// clk_register_clkdev(kmem_cache#29-oX (apll), "fout_apll", NULL): 0
	// clk: kmem_cache#29-oX (epll),
	// pll_clk->alias: (&exynos5420_plls[3])->alias: "fout_epll",
	// pll_clk->dev_name: (&exynos5420_plls[3])->dev_name: NULL
	// clk_register_clkdev(kmem_cache#29-oX (epll), "fout_epll", NULL): 0
	ret = clk_register_clkdev(clk, pll_clk->alias, pll_clk->dev_name);
	// ret: 0
	// ret: 0

	// clk_register_clkdev에서 한일:
	// struct clk_lookup_alloc 의 메모리를 kmem_cache#30-oX (apll) 할당 받고
	// struct clk_lookup_alloc 맴버값 초기화 수행
	//
	// (kmem_cache#30-oX)->cl.clk: kmem_cache#29-oX (apll)
	// (kmem_cache#30-oX)->con_id: "fout_apll"
	// (kmem_cache#30-oX)->cl.con_id: (kmem_cache#30-oX)->con_id: "fout_apll"
	//
	// list clocks에 &(&(kmem_cache#30-oX (apll))->cl)->nade를 tail로 추가

	// clk_register_clkdev에서 한일:
	// struct clk_lookup_alloc 의 메모리를 kmem_cache#30-oX (epll) 할당 받고
	// struct clk_lookup_alloc 맴버값 초기화 수행
	//
	// (kmem_cache#30-oX)->cl.clk: kmem_cache#29-oX (epll)
	// (kmem_cache#30-oX)->con_id: "fout_apll"
	// (kmem_cache#30-oX)->cl.con_id: (kmem_cache#30-oX)->con_id: "fout_apll"
	//
	// list clocks에 &(&(kmem_cache#30-oX (epll))->cl)->nade를 tail로 추가

	// ret: 0
	// ret: 0
	if (ret)
		pr_err("%s: failed to register lookup for %s : %d",
			__func__, pll_clk->name, ret);
}

// ARM10C 20150117
// exynos5420_plls, ARRAY_SIZE(exynos5420_plls): 11, reg_base: 0xf0040000
void __init samsung_clk_register_pll(struct samsung_pll_clock *pll_list,
				unsigned int nr_pll, void __iomem *base)
{
	int cnt;

	// nr_pll: 11
	for (cnt = 0; cnt < nr_pll; cnt++)
		// cnt: 0, &pll_list[0]: &exynos5420_plls[0], base: 0xf0040000
		// cnt: 3, &pll_list[3]: &exynos5420_plls[3], base: 0xf0040000
		_samsung_clk_register_pll(&pll_list[cnt], base);

		// _samsung_clk_register_pll (&exynos5420_plls[0]) 에서 한일:
		//
		// struct clk_fixed_rate 만큼 메모리를 kmem_cache#30-oX (apll) 할당 받고 struct clk_fixed_rate 의 멤버 값을 아래와 같이 초기화 수행
		// pll: kmem_cache#30-oX (apll)
		//
		// (kmem_cache#30-oX (apll))->hw.init: &init
		// (kmem_cache#30-oX (apll))->type: pll_2550: 2
		// (kmem_cache#30-oX (apll))->lock_reg: 0xf0040000
		// (kmem_cache#30-oX (apll))->con_reg: 0xf0040100
		//
		// struct clk 만큼 메모리를 kmem_cache#29-oX (apll) 할당 받고 struct clk 의 멤버 값을 아래와 같이 초기화 수행
		//
		// (kmem_cache#29-oX (apll))->name: kmem_cache#30-oX ("fout_apll")
		// (kmem_cache#29-oX (apll))->ops: &samsung_pll35xx_clk_min_ops
		// (kmem_cache#29-oX (apll))->hw: &(kmem_cache#30-oX (apll))->hw
		// (kmem_cache#29-oX (apll))->flags: 0x40
		// (kmem_cache#29-oX (apll))->num_parents: 1
		// (kmem_cache#29-oX (apll))->parent_names: kmem_cache#30-oX
		// (kmem_cache#29-oX (apll))->parent_names[0]: (kmem_cache#30-oX)[0]: kmem_cache#30-oX: "fin_pll"
		// (kmem_cache#29-oX (apll))->parent: kmem_cache#29-oX (fin_pll)
		// (kmem_cache#29-oX (apll))->rate: 1000000000 (1 Ghz)
		//
		// (&(kmem_cache#29-oX (apll))->child_node)->next: NULL
		// (&(kmem_cache#29-oX (apll))->child_node)->pprev: &(&(kmem_cache#29-oX (apll))->child_node)
		//
		// (&(kmem_cache#29-oX (fin_pll))->children)->first: &(kmem_cache#29-oX (apll))->child_node
		//
		// (&(kmem_cache#30-oX (apll))->hw)->clk: kmem_cache#29-oX (apll)
		//
		// clk_table[2]: (kmem_cache#23-o0)[2]: kmem_cache#29-oX (apll)
		//
		// struct clk_lookup_alloc 의 메모리를 kmem_cache#30-oX (apll) 할당 받고
		// struct clk_lookup_alloc 맴버값 초기화 수행
		//
		// (kmem_cache#30-oX)->cl.clk: kmem_cache#29-oX (apll)
		// (kmem_cache#30-oX)->con_id: "fout_apll"
		// (kmem_cache#30-oX)->cl.con_id: (kmem_cache#30-oX)->con_id: "fout_apll"
		//
		// list clocks에 &(&(kmem_cache#30-oX (apll))->cl)->nade를 tail로 추가

		// cnt: 1...10 loop 까지 loop 수행
}
