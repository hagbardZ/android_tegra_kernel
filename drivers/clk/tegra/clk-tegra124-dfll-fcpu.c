/*
 * Tegra124 DFLL FCPU clock source driver
 *
 * Copyright (C) 2012-2014 NVIDIA Corporation.  All rights reserved.
 *
 * Aleksandr Frid <afrid@nvidia.com>
 * Paul Walmsley <pwalmsley@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <soc/tegra/fuse.h>

#include <dt-bindings/thermal/tegra210-trips.h>

#include "clk.h"
#include "clk-dfll.h"
#include "cvb.h"

struct dfll_fcpu_data {
	const unsigned long *cpu_max_freq_table;
	unsigned int cpu_max_freq_table_size;
	const struct cvb_table *cpu_cvb_tables;
	unsigned int cpu_cvb_tables_size;
	const struct thermal_table *cpu_thermal_table;
};

/* Maximum CPU frequency, indexed by CPU speedo id */
static const unsigned long tegra124_cpu_max_freq_table[] = {
	[0] = 2014500000UL,
	[1] = 2320500000UL,
	[2] = 2116500000UL,
	[3] = 2524500000UL,
};

static const unsigned long tegra210_cpu_max_freq_table[] = {
	[0] = 1912500000UL,
	[1] = 1912500000UL,
	[2] = 2218500000UL,
	[3] = 1912500000UL,
	[4] = 1632000000UL,
	[5] = 1912500000UL,
};

static const struct cvb_table tegra124_cpu_cvb_tables[] = {
	{
		.speedo_id = -1,
		.process_id = -1,
		.min_millivolts = 900,
		.max_millivolts = 1260,
		.alignment = {
			.step_uv = 10000, /* 10mV */
		},
		.speedo_scale = 100,
		.voltage_scale = 1000,
		.cvb_table = {
			{204000000UL,   {1112619, -29295, 402} },
			{306000000UL,	{1150460, -30585, 402} },
			{408000000UL,	{1190122, -31865, 402} },
			{510000000UL,	{1231606, -33155, 402} },
			{612000000UL,	{1274912, -34435, 402} },
			{714000000UL,	{1320040, -35725, 402} },
			{816000000UL,	{1366990, -37005, 402} },
			{918000000UL,	{1415762, -38295, 402} },
			{1020000000UL,	{1466355, -39575, 402} },
			{1122000000UL,	{1518771, -40865, 402} },
			{1224000000UL,	{1573009, -42145, 402} },
			{1326000000UL,	{1629068, -43435, 402} },
			{1428000000UL,	{1686950, -44715, 402} },
			{1530000000UL,	{1746653, -46005, 402} },
			{1632000000UL,	{1808179, -47285, 402} },
			{1734000000UL,	{1871526, -48575, 402} },
			{1836000000UL,	{1936696, -49855, 402} },
			{1938000000UL,	{2003687, -51145, 402} },
			{2014500000UL,	{2054787, -52095, 402} },
			{2116500000UL,	{2124957, -53385, 402} },
			{2218500000UL,	{2196950, -54665, 402} },
			{2320500000UL,	{2270765, -55955, 402} },
			{2422500000UL,	{2346401, -57235, 402} },
			{2524500000UL,	{2437299, -58535, 402} },
			{0,		{      0,      0,   0} },
		},
		.cpu_dfll_data = {
			.tune0_low = 0x005020ff,
			.tune0_high = 0x005040ff,
			.tune1 = 0x00000060,
		}
	},
};

#define TEGRA210_CPU_CVB_TABLE	\
		.cvb_table = {	\
			{51000000UL,    {1007452, -23865, 370} }, \
			{102000000UL,   {1007452, -23865, 370} }, \
			{204000000UL,   {1007452, -23865, 370} }, \
			{306000000UL,   {1052709, -24875, 370} }, \
			{408000000UL,   {1099069, -25895, 370} }, \
			{510000000UL,   {1146534, -26905, 370} }, \
			{612000000UL,   {1195102, -27915, 370} }, \
			{714000000UL,   {1244773, -28925, 370} }, \
			{816000000UL,   {1295549, -29935, 370} }, \
			{918000000UL,   {1347428, -30955, 370} }, \
			{1020000000UL,  {1400411, -31965, 370} }, \
			{1122000000UL,  {1454497, -32975, 370} }, \
			{1224000000UL,  {1509687, -33985, 370} }, \
			{1326000000UL,  {1565981, -35005, 370} }, \
			{1428000000UL,  {1623379, -36015, 370} }, \
			{1530000000UL,  {1681880, -37025, 370} }, \
			{1632000000UL,  {1741485, -38035, 370} }, \
			{1734000000UL,  {1802194, -39055, 370} }, \
			{1836000000UL,  {1864006, -40065, 370} }, \
			{1912500000UL,  {1910780, -40815, 370} }, \
			{0,             {      0,      0,   0} }, \
		}

static const struct cvb_table tegra210_cpu_cvb_tables[] = {
	{
		.speedo_id = 0,
		.process_id = 0,
		.min_millivolts = 850,
		.max_millivolts = 1170,
		.alignment = {
			.step_uv = 6250, /* 6.25mV */
		},
		.speedo_scale = 100,
		.voltage_scale = 1000,
		TEGRA210_CPU_CVB_TABLE,
		.cpu_dfll_data = {
			.tune0_low = 0xffead0ff,
			.tune0_high = 0xffead0ff,
			.tune1 = 0x020091d9,
			.tune_high_min_millivolts = 864,
		}
	},
	{
		.speedo_id = 0,
		.process_id = 1,
		.min_millivolts = 850,
		.max_millivolts = 1170,
		.alignment = {
			.step_uv = 6250, /* 6.25mV */
		},
		.speedo_scale = 100,
		.voltage_scale = 1000,
		TEGRA210_CPU_CVB_TABLE,
		.cpu_dfll_data = {
			.tune0_low = 0xffead0ff,
			.tune0_high = 0xffead0ff,
			.tune1 = 0x025501d0,
			.tune_high_min_millivolts = 864,
		}
	},
	{
		.speedo_id = 1,
		.process_id = 0,
		.min_millivolts = 837,
		.max_millivolts = 1227,
		.alignment = {
			.step_uv = 6250, /* 6.25mV */
		},
		.speedo_scale = 100,
		.voltage_scale = 1000,
		TEGRA210_CPU_CVB_TABLE,
		.cpu_dfll_data = {
			.tune0_low = 0xffead0ff,
			.tune0_high = 0xffead0ff,
			.tune1 = 0x020091d9,
			.tune_high_min_millivolts = 864,
		}
	},
	{
		.speedo_id = 1,
		.process_id = 1,
		.min_millivolts = 837,
		.max_millivolts = 1227,
		.alignment = {
			.step_uv = 6250, /* 6.25mV */
		},
		.speedo_scale = 100,
		.voltage_scale = 1000,
		TEGRA210_CPU_CVB_TABLE,
		.cpu_dfll_data = {
			.tune0_low = 0xffead0ff,
			.tune0_high = 0xffead0ff,
			.tune1 = 0x025501d0,
			.tune_high_min_millivolts = 864,
		}
	},
	{
		.speedo_id = 5,
		.process_id = 0,
		.min_millivolts = 818,
		.max_millivolts = 1227,
		.alignment = {
			.step_uv = 6250, /* 6.25mV */
		},
		.speedo_scale = 100,
		.voltage_scale = 1000,
		TEGRA210_CPU_CVB_TABLE,
		.cpu_dfll_data = {
			.tune0_low = 0xffead0ff,
			.tune0_high = 0xffead0ff,
			.tune1 = 0x020091d9,
			.tune_high_min_millivolts = 864,
		}
	},
	{
		.speedo_id = 5,
		.process_id = 1,
		.min_millivolts = 818,
		.max_millivolts = 1227,
		.alignment = {
			.step_uv = 6250, /* 6.25mV */
		},
		.speedo_scale = 100,
		.voltage_scale = 1000,
		TEGRA210_CPU_CVB_TABLE,
		.cpu_dfll_data = {
			.tune0_low = 0xffead0ff,
			.tune0_high = 0xffead0ff,
			.tune1 = 0x025501d0,
			.tune_high_min_millivolts = 864,
		}
	},
};

static struct thermal_tv tegra210_thermal_floor_table[] = {
	{TEGRA210_DFLL_THERMAL_FLOOR_0 / 1000, 950},
	{TEGRA210_DFLL_THERMAL_FLOOR_1 / 1000, 850},
	{TEGRA210_DFLL_THERMAL_FLOOR_2 / 1000, 850},
	{TEGRA210_DFLL_THERMAL_FLOOR_3 / 1000, 850},
	{TEGRA210_DFLL_THERMAL_FLOOR_4 / 1000, 850},
};

static const struct thermal_tv tegra210_thermal_cap_table[] = {
	{TEGRA210_DFLL_THERMAL_CAP_NOCAP / 1000, INT_MAX},
	{TEGRA210_DFLL_THERMAL_CAP_0 / 1000, 1170},
	{TEGRA210_DFLL_THERMAL_CAP_1 / 1000, 1132},
};

static const struct thermal_table tegra210_cpu_thermal_table = {
	.thermal_floor_table = tegra210_thermal_floor_table,
	.thermal_floor_table_size = ARRAY_SIZE(tegra210_thermal_floor_table),
	.coefficients = { {800000, 0, 0}, 0, 0, 0 },
	.speedo_scale = 100,
	.voltage_scale = 1000,
	.temp_scale = 10,
	.thermal_cap_table = tegra210_thermal_cap_table,
	.thermal_cap_table_size = ARRAY_SIZE(tegra210_thermal_cap_table),
};

static struct tegra_dfll_soc_data soc;

static const struct dfll_fcpu_data tegra124_dfll_fcpu_data = {
	.cpu_max_freq_table = tegra124_cpu_max_freq_table,
	.cpu_max_freq_table_size = ARRAY_SIZE(tegra124_cpu_max_freq_table),
	.cpu_cvb_tables = tegra124_cpu_cvb_tables,
	.cpu_cvb_tables_size = ARRAY_SIZE(tegra124_cpu_cvb_tables)
};

static const struct dfll_fcpu_data tegra210_dfll_fcpu_data = {
	.cpu_max_freq_table = tegra210_cpu_max_freq_table,
	.cpu_max_freq_table_size = ARRAY_SIZE(tegra210_cpu_max_freq_table),
	.cpu_cvb_tables = tegra210_cpu_cvb_tables,
	.cpu_cvb_tables_size = ARRAY_SIZE(tegra210_cpu_cvb_tables),
	.cpu_thermal_table = &tegra210_cpu_thermal_table
};

static const struct of_device_id tegra124_dfll_fcpu_of_match[] = {
	{
		.compatible = "nvidia,tegra124-dfll",
		.data = &tegra124_dfll_fcpu_data
	},
	{
		.compatible = "nvidia,tegra210-dfll",
		.data = &tegra210_dfll_fcpu_data
	},
	{ },
};
MODULE_DEVICE_TABLE(of, tegra124_dfll_fcpu_of_match);

static int tegra124_dfll_fcpu_probe(struct platform_device *pdev)
{
	int process_id, speedo_id, speedo_value, ret;
	const struct cvb_table *cvb;
	const struct of_device_id *of_id;
	const struct dfll_fcpu_data *fcpu_data;
	const struct thermal_table *thermal;

	of_id = of_match_device(tegra124_dfll_fcpu_of_match, &pdev->dev);
	fcpu_data = of_id->data;

	process_id = tegra_sku_info.cpu_process_id;
	speedo_id = tegra_sku_info.cpu_speedo_id;
	speedo_value = tegra_sku_info.cpu_speedo_value;

	if (speedo_id >= fcpu_data->cpu_max_freq_table_size) {
		pr_err("unknown max CPU freq for speedo_id=%d\n", speedo_id);
		return -ENODEV;
	}

	soc.opp_dev = get_cpu_device(0);
	if (!soc.opp_dev) {
		pr_err("no CPU0 device\n");
		return -ENODEV;
	}

	cvb = tegra_cvb_build_opp_table(fcpu_data->cpu_cvb_tables,
					fcpu_data->cpu_cvb_tables_size,
					process_id, speedo_id, speedo_value,
					fcpu_data->cpu_max_freq_table[speedo_id],
					soc.opp_dev);
	if (IS_ERR(cvb)) {
		pr_err("couldn't build OPP table: %ld\n", PTR_ERR(cvb));
		return PTR_ERR(cvb);
	}

	soc.min_millivolts = cvb->min_millivolts;
	soc.alignment = cvb->alignment.step_uv;
	soc.tune0_low = cvb->cpu_dfll_data.tune0_low;
	soc.tune0_high = cvb->cpu_dfll_data.tune0_high;
	soc.tune1 = cvb->cpu_dfll_data.tune1;
	soc.tune_high_min_millivolts =
		cvb->cpu_dfll_data.tune_high_min_millivolts;

	thermal = fcpu_data->cpu_thermal_table;
	ret = tegra_cvb_build_thermal_table(thermal, speedo_value);
	if (ret < 0) {
		pr_warn("couldn't build thermal floor table\n");
	} else {
		soc.thermal_floor_table = thermal->thermal_floor_table;
		soc.thermal_floor_table_size = thermal->thermal_floor_table_size;
	}

	if (!thermal || !thermal->thermal_cap_table) {
		pr_warn("couldn't get thermal cap table\n");
	} else {
		soc.thermal_cap_table = thermal->thermal_cap_table;
		soc.thermal_cap_table_size = thermal->thermal_cap_table_size;
	}

	return tegra_dfll_register(pdev, &soc);
}

static const struct dev_pm_ops tegra124_dfll_pm_ops = {
	SET_RUNTIME_PM_OPS(tegra_dfll_runtime_suspend,
			   tegra_dfll_runtime_resume, NULL)
};

static struct platform_driver tegra124_dfll_fcpu_driver = {
	.probe		= tegra124_dfll_fcpu_probe,
	.remove		= tegra_dfll_unregister,
	.driver		= {
		.name		= "tegra124-dfll",
		.of_match_table = tegra124_dfll_fcpu_of_match,
		.pm		= &tegra124_dfll_pm_ops,
	},
};

module_platform_driver(tegra124_dfll_fcpu_driver);

MODULE_DESCRIPTION("Tegra124 DFLL clock source driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Aleksandr Frid <afrid@nvidia.com>");
MODULE_AUTHOR("Paul Walmsley <pwalmsley@nvidia.com>");
