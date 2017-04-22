/*
 * Copyright (c) 2012, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <soc/tegra/tegra-dvfs.h>

#include "clk.h"

#define SUPER_STATE_IDLE 0
#define SUPER_STATE_RUN 1
#define SUPER_STATE_IRQ 2
#define SUPER_STATE_FIQ 3

#define SUPER_STATE_SHIFT 28
#define SUPER_STATE_MASK ((BIT(SUPER_STATE_IDLE) | BIT(SUPER_STATE_RUN) | \
			   BIT(SUPER_STATE_IRQ) | BIT(SUPER_STATE_FIQ))	\
			  << SUPER_STATE_SHIFT)

#define SUPER_LP_DIV2_BYPASS (1 << 16)

#define super_state(s) (BIT(s) << SUPER_STATE_SHIFT)
#define super_state_to_src_shift(m, s) ((m->width * s))
#define super_state_to_src_mask(m) (((1 << m->width) - 1))

#define SUPER_SKIPPER_ENABLE	BIT(31)
#define SUPER_SKIPPER_MUL_SIZE	8
#define SUPER_SKIPPER_MUL_SHIFT	8
#define SUPER_SKIPPER_MUL_MASK	(((1 << SUPER_SKIPPER_MUL_SIZE) - 1) \
				<< SUPER_SKIPPER_MUL_SHIFT)
#define SUPER_SKIPPER_DIV_SIZE	8
#define SUPER_SKIPPER_DIV_SHIFT	0
#define SUPER_SKIPPER_DIV_MASK	(((1 << SUPER_SKIPPER_DIV_SIZE) - 1) \
				<< SUPER_SKIPPER_DIV_SHIFT)
#define SUPER_SKIPPER_MASK	(SUPER_SKIPPER_ENABLE |		\
				SUPER_SKIPPER_MUL_MASK |	\
				SUPER_SKIPPER_DIV_MASK)

static u8 clk_super_get_parent(struct clk_hw *hw)
{
	struct tegra_clk_super_mux *mux = to_clk_super_mux(hw);
	u32 val, state;
	u8 source, shift;

	val = readl_relaxed(mux->reg);

	state = val & SUPER_STATE_MASK;

	BUG_ON((state != super_state(SUPER_STATE_RUN)) &&
	       (state != super_state(SUPER_STATE_IDLE)));
	shift = (state == super_state(SUPER_STATE_IDLE)) ?
		super_state_to_src_shift(mux, SUPER_STATE_IDLE) :
		super_state_to_src_shift(mux, SUPER_STATE_RUN);

	source = (val >> shift) & super_state_to_src_mask(mux);

	/*
	 * If LP_DIV2_BYPASS is not set and PLLX is current parent then
	 * PLLX/2 is the input source to CCLKLP.
	 */
	if ((mux->flags & TEGRA_DIVIDER_2) && !(val & SUPER_LP_DIV2_BYPASS) &&
	    (source == mux->pllx_index))
		source = mux->div2_index;

	return source;
}

static int clk_super_set_parent(struct clk_hw *hw, u8 index)
{
	struct tegra_clk_super_mux *mux = to_clk_super_mux(hw);
	u32 val, state;
	int err = 0;
	u8 parent_index, shift;
	unsigned long flags = 0;

	if (mux->lock)
		spin_lock_irqsave(mux->lock, flags);

	val = readl_relaxed(mux->reg);
	state = val & SUPER_STATE_MASK;
	BUG_ON((state != super_state(SUPER_STATE_RUN)) &&
	       (state != super_state(SUPER_STATE_IDLE)));
	shift = (state == super_state(SUPER_STATE_IDLE)) ?
		super_state_to_src_shift(mux, SUPER_STATE_IDLE) :
		super_state_to_src_shift(mux, SUPER_STATE_RUN);

	/*
	 * For LP mode super-clock switch between PLLX direct
	 * and divided-by-2 outputs is allowed only when other
	 * than PLLX clock source is current parent.
	 */
	if ((mux->flags & TEGRA_DIVIDER_2) && ((index == mux->div2_index) ||
					       (index == mux->pllx_index))) {
		parent_index = clk_super_get_parent(hw);
		if ((parent_index == mux->div2_index) ||
		    (parent_index == mux->pllx_index)) {
			err = -EINVAL;
			goto out;
		}

		val ^= SUPER_LP_DIV2_BYPASS;
		writel_relaxed(val, mux->reg);
		udelay(2);

		if (index == mux->div2_index)
			index = mux->pllx_index;
	}
	val &= ~((super_state_to_src_mask(mux)) << shift);
	val |= (index & (super_state_to_src_mask(mux))) << shift;

	writel_relaxed(val, mux->reg);
	udelay(2);

out:
	if (mux->lock)
		spin_unlock_irqrestore(mux->lock, flags);

	return err;
}

static int clk_super_prepare(struct clk_hw *hw)
{
	struct tegra_clk_super_mux *mux = to_clk_super_mux(hw);

	mux->prepared = true;
	return tegra_dvfs_set_rate(hw->clk, clk_get_rate(hw->clk));
}

static void clk_super_unprepare(struct clk_hw *hw)
{
	struct tegra_clk_super_mux *mux = to_clk_super_mux(hw);

	tegra_dvfs_set_rate(hw->clk, 0);
	mux->prepared = false;
}

static int clk_super_is_prepared(struct clk_hw *hw)
{
	struct tegra_clk_super_mux *mux = to_clk_super_mux(hw);

	if (mux->prepared)
		return true;

	/* In case the clock is used to determine the required voltage */
	return tegra_dvfs_get_rate(hw->clk) != 0;
}

const struct clk_ops tegra_clk_super_ops = {
	.get_parent = clk_super_get_parent,
	.set_parent = clk_super_set_parent,
	.prepare = clk_super_prepare,
	.unprepare = clk_super_unprepare,
	.is_prepared = clk_super_is_prepared,
};

struct clk *tegra_clk_register_super_mux(const char *name,
		const char **parent_names, u8 num_parents,
		unsigned long flags, void __iomem *reg, u8 clk_super_flags,
		u8 width, u8 pllx_index, u8 div2_index, spinlock_t *lock)
{
	struct tegra_clk_super_mux *super;
	struct clk *clk;
	struct clk_init_data init;

	super = kzalloc(sizeof(*super), GFP_KERNEL);
	if (!super) {
		pr_err("%s: could not allocate super clk\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	init.name = name;
	init.ops = &tegra_clk_super_ops;
	init.flags = flags;
	init.parent_names = parent_names;
	init.num_parents = num_parents;

	super->reg = reg;
	super->pllx_index = pllx_index;
	super->div2_index = div2_index;
	super->lock = lock;
	super->width = width;
	super->flags = clk_super_flags;

	/* Data in .init is copied by clk_register(), so stack variable OK */
	super->hw.init = &init;

	clk = clk_register(NULL, &super->hw);
	if (IS_ERR(clk))
		kfree(super);

	return clk;
}

static unsigned long clk_super_skipper_recalc_rate(struct clk_hw *hw,
						   unsigned long parent_rate)
{
	struct tegra_clk_super_skipper *skipper = to_clk_super_skipper(hw);
	unsigned int val;
	int div, mul;

	val = readl_relaxed(skipper->reg);
	div = (val & SUPER_SKIPPER_DIV_MASK) >> SUPER_SKIPPER_DIV_SHIFT;
	div++;

	mul = (val & SUPER_SKIPPER_MUL_MASK) >> SUPER_SKIPPER_MUL_SHIFT;
	mul++;

	return DIV_ROUND_UP(parent_rate * mul, div);
}

static long clk_super_skipper_round_rate(struct clk_hw *hw, unsigned long rate,
					 unsigned long *prate)
{
	unsigned long output;
	u32 mul, div;

	if (rate > *prate)
		return *prate;

	div = 1 << SUPER_SKIPPER_DIV_SIZE;
	output = DIV_ROUND_UP(rate << SUPER_SKIPPER_DIV_SIZE, *prate);
	mul = output ? : 1;

	return DIV_ROUND_UP(*prate * mul, div);
}

static int clk_super_skipper_set_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long parent_rate)
{
	struct tegra_clk_super_skipper *skipper = to_clk_super_skipper(hw);
	u32 val, mul, div;
	unsigned long flags = 0;
	unsigned long output;

	div = 1 << SUPER_SKIPPER_DIV_SIZE;
	output = DIV_ROUND_UP(rate << SUPER_SKIPPER_DIV_SIZE, parent_rate);
	mul = output ? : 1;

	if (skipper->lock)
		spin_lock_irqsave(skipper->lock, flags);

	val = readl_relaxed(skipper->reg);
	val &= ~SUPER_SKIPPER_MASK;

	if (mul < div) {
		val = SUPER_SKIPPER_ENABLE |
			(mul - 1) << SUPER_SKIPPER_MUL_SHIFT |
			(div - 1) << SUPER_SKIPPER_DIV_SHIFT;
	}

	writel_relaxed(val, skipper->reg);

	if (skipper->lock)
		spin_unlock_irqrestore(skipper->lock, flags);

	return 0;
}

const struct clk_ops tegra_clk_super_skipper_ops = {
	.set_rate = clk_super_skipper_set_rate,
	.round_rate = clk_super_skipper_round_rate,
	.recalc_rate = clk_super_skipper_recalc_rate,
};

struct clk *
tegra_clk_register_super_skipper(const char *name, const char *parent_name,
				 unsigned long flags, void __iomem *reg,
				 spinlock_t *lock)
{
	struct tegra_clk_super_skipper *skipper;
	struct clk *clk;
	struct clk_init_data init;

	skipper = kzalloc(sizeof(*skipper), GFP_KERNEL);
	if (!skipper) {
		pr_err("%s: could not allocate skipper clk\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	init.name = name;
	init.ops = &tegra_clk_super_skipper_ops;
	init.flags = flags;
	init.parent_names = (parent_name ? &parent_name : NULL);
	init.num_parents = (parent_name ? 1 : 0);

	skipper->reg = reg;
	skipper->lock = lock;
	skipper->hw.init = &init;

	clk = clk_register(NULL, &skipper->hw);
	if (IS_ERR(clk))
		kfree(skipper);

	return clk;
}
