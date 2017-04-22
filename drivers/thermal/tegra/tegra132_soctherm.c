/*
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/thermal.h>
#include <soc/tegra/fuse.h>

#include <dt-bindings/thermal/tegra124-soctherm.h>

#include "tegra_soctherm.h"

/* bits_per_temp_threshold */
#define TEGRA132_BPTT				0x8

#define TEGRA132_THERMCTL_LVL0_UP_THRESH_MASK	(0xff << 17)
#define TEGRA132_THERMCTL_LVL0_DN_THRESH_MASK	(0xff << 9)

#define TEGRA132_THERMTRIP_ANY_EN_MASK		(0x1 << 28)
#define TEGRA132_THERMTRIP_MEM_EN_MASK		(0x1 << 27)
#define TEGRA132_THERMTRIP_GPU_EN_MASK		(0x1 << 26)
#define TEGRA132_THERMTRIP_CPU_EN_MASK		(0x1 << 25)
#define TEGRA132_THERMTRIP_TSENSE_EN_MASK	(0x1 << 24)
#define TEGRA132_THERMTRIP_GPUMEM_THRESH_MASK	(0xff << 16)
#define TEGRA132_THERMTRIP_CPU_THRESH_MASK	(0xff << 8)
#define TEGRA132_THERMTRIP_TSENSE_THRESH_MASK	0xff

#define TEGRA132_FUSE_CP_REV			0x90

static const struct tegra_tsensor_configuration tegra132_tsensor_config = {
	.tall = 16300,
	.tsample = 120,
	.tiddq_en = 1,
	.ten_count = 1,
	.tsample_ate = 480,
};

static const struct tegra_tsensor_group tegra132_tsensor_group_cpu_pre_0_12 = {
	.id				= TEGRA124_SOCTHERM_SENSOR_CPU,
	.name				= "cpu",
	.thermctl_isr_shift		= 8,
	.thermctl_lvl0_offset		= THERMCTL_LEVEL0_GROUP_CPU,
	.thermctl_lvl0_up_thresh_mask	= TEGRA132_THERMCTL_LVL0_UP_THRESH_MASK,
	.thermctl_lvl0_dn_thresh_mask	= TEGRA132_THERMCTL_LVL0_DN_THRESH_MASK,
	.sensor_temp_offset		= SENSOR_TEMP1,
	.sensor_temp_mask		= SENSOR_TEMP1_CPU_TEMP_MASK,
	.pdiv				= 8,
	.pdiv_ate			= 8,
	.pdiv_mask			= SENSOR_PDIV_CPU_MASK,
	.pllx_hotspot_mask		= SENSOR_HOTSPOT_CPU_MASK,
	.pllx_hotspot_diff		= 10000,
	.thermtrip_any_en_mask		= TEGRA132_THERMTRIP_ANY_EN_MASK,
	.thermtrip_enable_mask		= TEGRA132_THERMTRIP_CPU_EN_MASK,
	.thermtrip_threshold_mask	= TEGRA132_THERMTRIP_CPU_THRESH_MASK,
	.bptt				= TEGRA132_BPTT,
	.flags				= SKIP_THERMAL_FW_REGISTRATION |
					  SKIP_THERMTRIP_REGISTRATION,
};

static const struct tegra_tsensor_group tegra132_tsensor_group_cpu_0_12_plus = {
	.id				= TEGRA124_SOCTHERM_SENSOR_CPU,
	.name				= "cpu",
	.thermctl_isr_shift		= 8,
	.thermctl_lvl0_offset		= THERMCTL_LEVEL0_GROUP_CPU,
	.thermctl_lvl0_up_thresh_mask	= TEGRA132_THERMCTL_LVL0_UP_THRESH_MASK,
	.thermctl_lvl0_dn_thresh_mask	= TEGRA132_THERMCTL_LVL0_DN_THRESH_MASK,
	.sensor_temp_offset		= SENSOR_TEMP1,
	.sensor_temp_mask		= SENSOR_TEMP1_CPU_TEMP_MASK,
	.pdiv				= 8,
	.pdiv_ate			= 8,
	.pdiv_mask			= SENSOR_PDIV_CPU_MASK,
	.pllx_hotspot_mask		= SENSOR_HOTSPOT_CPU_MASK,
	.pllx_hotspot_diff		= 10000,
	.thermtrip_any_en_mask		= TEGRA132_THERMTRIP_ANY_EN_MASK,
	.thermtrip_enable_mask		= TEGRA132_THERMTRIP_CPU_EN_MASK,
	.thermtrip_threshold_mask	= TEGRA132_THERMTRIP_CPU_THRESH_MASK,
	.bptt				= TEGRA132_BPTT,
};

static const struct tegra_tsensor_group tegra132_tsensor_group_gpu = {
	.id				= TEGRA124_SOCTHERM_SENSOR_GPU,
	.name				= "gpu",
	.thermctl_isr_shift		= 16,
	.thermctl_lvl0_offset		= THERMCTL_LEVEL0_GROUP_GPU,
	.thermctl_lvl0_up_thresh_mask	= TEGRA132_THERMCTL_LVL0_UP_THRESH_MASK,
	.thermctl_lvl0_dn_thresh_mask	= TEGRA132_THERMCTL_LVL0_DN_THRESH_MASK,
	.sensor_temp_offset		= SENSOR_TEMP1,
	.sensor_temp_mask		= SENSOR_TEMP1_GPU_TEMP_MASK,
	.pdiv				= 8,
	.pdiv_ate			= 8,
	.pdiv_mask			= SENSOR_PDIV_GPU_MASK,
	.pllx_hotspot_mask		= SENSOR_HOTSPOT_GPU_MASK,
	.pllx_hotspot_diff		= 5000,
	.thermtrip_any_en_mask		= TEGRA132_THERMTRIP_ANY_EN_MASK,
	.thermtrip_enable_mask		= TEGRA132_THERMTRIP_GPU_EN_MASK,
	.thermtrip_threshold_mask	= TEGRA132_THERMTRIP_GPUMEM_THRESH_MASK,
	.bptt				= TEGRA132_BPTT,
};

static const struct tegra_tsensor_group tegra132_tsensor_group_pll_pre_0_12 = {
	.id				= TEGRA124_SOCTHERM_SENSOR_PLLX,
	.name				= "pll",
	.thermctl_isr_shift		= 0,
	.thermctl_lvl0_offset		= THERMCTL_LEVEL0_GROUP_TSENSE,
	.thermctl_lvl0_up_thresh_mask	= TEGRA132_THERMCTL_LVL0_UP_THRESH_MASK,
	.thermctl_lvl0_dn_thresh_mask	= TEGRA132_THERMCTL_LVL0_DN_THRESH_MASK,
	.sensor_temp_offset		= SENSOR_TEMP2,
	.sensor_temp_mask		= SENSOR_TEMP2_PLLX_TEMP_MASK,
	.pdiv				= 8,
	.pdiv_ate			= 8,
	.pdiv_mask			= SENSOR_PDIV_PLLX_MASK,
	.thermtrip_any_en_mask		= TEGRA132_THERMTRIP_ANY_EN_MASK,
	.thermtrip_enable_mask		= TEGRA132_THERMTRIP_TSENSE_EN_MASK,
	.thermtrip_threshold_mask	= TEGRA132_THERMTRIP_TSENSE_THRESH_MASK,
	.bptt				= TEGRA132_BPTT,
};

static const struct tegra_tsensor_group tegra132_tsensor_group_pll_0_12_plus = {
	.id				= TEGRA124_SOCTHERM_SENSOR_PLLX,
	.name				= "pll",
	.thermctl_isr_shift		= 0,
	.thermctl_lvl0_offset		= THERMCTL_LEVEL0_GROUP_TSENSE,
	.thermctl_lvl0_up_thresh_mask	= TEGRA132_THERMCTL_LVL0_UP_THRESH_MASK,
	.thermctl_lvl0_dn_thresh_mask	= TEGRA132_THERMCTL_LVL0_DN_THRESH_MASK,
	.sensor_temp_offset		= SENSOR_TEMP2,
	.sensor_temp_mask		= SENSOR_TEMP2_PLLX_TEMP_MASK,
	.pdiv				= 8,
	.pdiv_ate			= 8,
	.pdiv_mask			= SENSOR_PDIV_PLLX_MASK,
	.thermtrip_any_en_mask		= TEGRA132_THERMTRIP_ANY_EN_MASK,
	.thermtrip_enable_mask		= TEGRA132_THERMTRIP_TSENSE_EN_MASK,
	.thermtrip_threshold_mask	= TEGRA132_THERMTRIP_TSENSE_THRESH_MASK,
	.bptt				= TEGRA132_BPTT,
	.flags				= SKIP_THERMAL_FW_REGISTRATION |
					  SKIP_THERMTRIP_REGISTRATION,
};

static const struct tegra_tsensor_group tegra132_tsensor_group_mem = {
	.id				= TEGRA124_SOCTHERM_SENSOR_MEM,
	.name				= "mem",
	.thermctl_isr_shift		= 24,
	.thermctl_lvl0_offset		= THERMCTL_LEVEL0_GROUP_MEM,
	.thermctl_lvl0_up_thresh_mask	= TEGRA132_THERMCTL_LVL0_UP_THRESH_MASK,
	.thermctl_lvl0_dn_thresh_mask	= TEGRA132_THERMCTL_LVL0_DN_THRESH_MASK,
	.sensor_temp_offset		= SENSOR_TEMP2,
	.sensor_temp_mask		= SENSOR_TEMP2_MEM_TEMP_MASK,
	.pdiv				= 8,
	.pdiv_ate			= 8,
	.pdiv_mask			= SENSOR_PDIV_MEM_MASK,
	.pllx_hotspot_mask		= SENSOR_HOTSPOT_MEM_MASK,
	.pllx_hotspot_diff		= 0,
	.thermtrip_any_en_mask		= TEGRA132_THERMTRIP_ANY_EN_MASK,
	.thermtrip_enable_mask		= TEGRA132_THERMTRIP_MEM_EN_MASK,
	.thermtrip_threshold_mask	= TEGRA132_THERMTRIP_GPUMEM_THRESH_MASK,
	.bptt				= TEGRA132_BPTT,
};

static const struct tegra_tsensor_group *tegra132_tsensor_groups_pre_0_12[] = {
	&tegra132_tsensor_group_cpu_pre_0_12,
	&tegra132_tsensor_group_gpu,
	&tegra132_tsensor_group_pll_pre_0_12,
	&tegra132_tsensor_group_mem,
	NULL,
};

static const struct tegra_tsensor_group *tegra132_tsensor_groups_0_12_plus[] = {
	&tegra132_tsensor_group_cpu_0_12_plus,
	&tegra132_tsensor_group_gpu,
	&tegra132_tsensor_group_pll_0_12_plus,
	&tegra132_tsensor_group_mem,
	NULL,
};

static struct tegra_tsensor tegra132_tsensors_pre_0_09[] = {
	{
		.base = 0xc0,
		.name = "cpu0",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x098,
		.fuse_corr_alpha = 1119800,
		.fuse_corr_beta = -6330400,
		.group = &tegra132_tsensor_group_cpu_pre_0_12,
	},
	{
		.base = 0xe0,
		.name = "cpu1",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x084,
		.fuse_corr_alpha = 1094100,
		.fuse_corr_beta = -3751800,
		.group = &tegra132_tsensor_group_cpu_pre_0_12,
	},
	{
		.base = 0x100,
		.name = "cpu2",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x088,
		.fuse_corr_alpha = 1108800,
		.fuse_corr_beta = -3835200,
		.group = &tegra132_tsensor_group_cpu_pre_0_12,
	},
	{
		.base = 0x120,
		.name = "cpu3",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x12c,
		.fuse_corr_alpha = 1103200,
		.fuse_corr_beta = -5132100,
		.group = &tegra132_tsensor_group_cpu_pre_0_12,
	},
	{
		.base = 0x140,
		.name = "mem0",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x158,
		.fuse_corr_alpha = 1168400,
		.fuse_corr_beta = -11266000,
		.group = &tegra132_tsensor_group_mem,
	},
	{
		.base = 0x160,
		.name = "mem1",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x15c,
		.fuse_corr_alpha = 1185600,
		.fuse_corr_beta = -10861000,
		.group = &tegra132_tsensor_group_mem,
	},
	{
		.base = 0x180,
		.name = "gpu",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x154,
		.fuse_corr_alpha = 1158500,
		.fuse_corr_beta = -10714000,
		.group = &tegra132_tsensor_group_gpu,
	},
	{
		.base = 0x1a0,
		.name = "pllx",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x160,
		.fuse_corr_alpha = 1150000,
		.fuse_corr_beta = -11899000,
		.group = &tegra132_tsensor_group_pll_pre_0_12,
	},
	{ .name = NULL },
};

static struct tegra_tsensor tegra132_tsensors_0_09_to_0_11[] = {
	{
		.base = 0xc0,
		.name = "cpu0",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x098,
		.fuse_corr_alpha = 1126600,
		.fuse_corr_beta = -9433500,
		.group = &tegra132_tsensor_group_cpu_pre_0_12,
	},
	{
		.base = 0xe0,
		.name = "cpu1",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x084,
		.fuse_corr_alpha = 1110800,
		.fuse_corr_beta = -7383000,
		.group = &tegra132_tsensor_group_cpu_pre_0_12,
	},
	{
		.base = 0x100,
		.name = "cpu2",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x088,
		.fuse_corr_alpha = 1113800,
		.fuse_corr_beta = -6215200,
		.group = &tegra132_tsensor_group_cpu_pre_0_12,
	},
	{
		.base = 0x120,
		.name = "cpu3",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x12c,
		.fuse_corr_alpha = 1129600,
		.fuse_corr_beta = -8196100,
		.group = &tegra132_tsensor_group_cpu_pre_0_12,
	},
	{
		.base = 0x140,
		.name = "mem0",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x158,
		.fuse_corr_alpha = 1132900,
		.fuse_corr_beta = -6755300,
		.group = &tegra132_tsensor_group_mem,
	},
	{
		.base = 0x160,
		.name = "mem1",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x15c,
		.fuse_corr_alpha = 1142300,
		.fuse_corr_beta = -7374200,
		.group = &tegra132_tsensor_group_mem,
	},
	{
		.base = 0x180,
		.name = "gpu",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x154,
		.fuse_corr_alpha = 1125100,
		.fuse_corr_beta = -6350400,
		.group = &tegra132_tsensor_group_gpu,
	},
	{
		.base = 0x1a0,
		.name = "pllx",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x160,
		.fuse_corr_alpha = 1118100,
		.fuse_corr_beta = -8208800,
		.group = &tegra132_tsensor_group_pll_pre_0_12,
	},
	{ .name = NULL },
};

static struct tegra_tsensor tegra132_tsensors_0_12_plus[] = {
	{
		.base = 0xc0,
		.name = "cpu0",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x098,
		.fuse_corr_alpha = 1126600,
		.fuse_corr_beta = -9433500,
		.group = &tegra132_tsensor_group_cpu_0_12_plus,
	},
	{
		.base = 0xe0,
		.name = "cpu1",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x084,
		.fuse_corr_alpha = 1110800,
		.fuse_corr_beta = -7383000,
		.group = &tegra132_tsensor_group_cpu_0_12_plus,
	},
	{
		.base = 0x100,
		.name = "cpu2",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x088,
		.fuse_corr_alpha = 1113800,
		.fuse_corr_beta = -6215200,
		.group = &tegra132_tsensor_group_cpu_0_12_plus,
	},
	{
		.base = 0x120,
		.name = "cpu3",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x12c,
		.fuse_corr_alpha = 1129600,
		.fuse_corr_beta = -8196100,
		.group = &tegra132_tsensor_group_cpu_0_12_plus,
	},
	{
		.base = 0x140,
		.name = "mem0",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x158,
		.fuse_corr_alpha = 1132900,
		.fuse_corr_beta = -6755300,
		.group = &tegra132_tsensor_group_mem,
	},
	{
		.base = 0x160,
		.name = "mem1",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x15c,
		.fuse_corr_alpha = 1142300,
		.fuse_corr_beta = -7374200,
		.group = &tegra132_tsensor_group_mem,
	},
	{
		.base = 0x180,
		.name = "gpu",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x154,
		.fuse_corr_alpha = 1125100,
		.fuse_corr_beta = -6350400,
		.group = &tegra132_tsensor_group_gpu,
	},
	{
		.base = 0x1a0,
		.name = "pllx",
		.config = &tegra132_tsensor_config,
		.calib_fuse_offset = 0x160,
		.fuse_corr_alpha = 1118100,
		.fuse_corr_beta = -8208800,
		.group = &tegra132_tsensor_group_pll_0_12_plus,
	},
	{ .name = NULL },
};


static struct of_device_id tegra132_soctherm_of_match[] = {
	{ .compatible = "nvidia,tegra132-soctherm" },
	{ },
};
MODULE_DEVICE_TABLE(of, tegra_soctherm_of_match);

static int tegra132_soctherm_probe(struct platform_device *pdev)
{
	const struct tegra_tsensor_group **tegra132_tsensor_groups = NULL;
	struct tegra_tsensor *tegra132_tsensors = NULL;
	u32 rev, rev_major, rev_minor;
	int err;

	/* Apply temperature sensor correction values from fuses */
	err = tegra_fuse_readl(TEGRA132_FUSE_CP_REV, &rev);
	if (err) {
		dev_err(&pdev->dev, "could not read CP_REV fuse: %d\n", err);
		return err;
	}
	rev_minor = rev & 0x1f;
	rev_major = (rev >> 5) & 0x3f;
	pr_debug("%s: CP rev %d.%d\n", __func__, rev_major, rev_minor);

	if (rev_major == 0 && rev_minor <= 8) {
		tegra132_tsensors = tegra132_tsensors_pre_0_09;
		tegra132_tsensor_groups =
			tegra132_tsensor_groups_pre_0_12;
	} else if (rev_major == 0 && rev_minor <= 11) {
		tegra132_tsensors = tegra132_tsensors_0_09_to_0_11;
		tegra132_tsensor_groups =
			tegra132_tsensor_groups_pre_0_12;
	} else {
		tegra132_tsensors = tegra132_tsensors_0_12_plus;
		tegra132_tsensor_groups =
			tegra132_tsensor_groups_0_12_plus;
	}

	return tegra_soctherm_probe(pdev,
				    tegra132_tsensors, tegra132_tsensor_groups,
				    CHIPID_TEGRA13X);
}

#ifdef CONFIG_PM_SLEEP
static SIMPLE_DEV_PM_OPS(tegra_soctherm_pm,
			 soctherm_suspend, soctherm_resume);
#define TEGRA_SOC_THERM_PM      (&tegra_soctherm_pm)
#else
#define TEGRA_SOC_THERM_PM      NULL
#endif

static struct platform_driver tegra132_soctherm_driver = {
	.probe = tegra132_soctherm_probe,
	.remove = tegra_soctherm_remove,
	.driver = {
		.name = "tegra132_soctherm",
		.pm = TEGRA_SOC_THERM_PM,
		.of_match_table = tegra132_soctherm_of_match,
	},
};
module_platform_driver(tegra132_soctherm_driver);

MODULE_AUTHOR("NVIDIA");
MODULE_DESCRIPTION("Tegra132 SOCTHERM thermal management driver");
MODULE_LICENSE("GPL v2");
