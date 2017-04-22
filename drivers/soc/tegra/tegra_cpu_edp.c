/*
 * Copyright (c) 2014, NVIDIA CORPORATION. All Rights Reserved.
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
#include <linux/init.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/suspend.h>
#include <linux/debugfs.h>
#include <linux/pm_qos.h>
#include <trace/events/sysedp.h>
#include <soc/tegra/fuse.h>
#include <soc/tegra/tegra-dvfs.h>
#include <soc/tegra/tegra-edp.h>
#include <soc/tegra/tegra-ppm.h>

struct cpu_edp_platform_data {
	char *clk_name;
	int n_caps;
	int freq_step;
	int reg_edp;
};

struct cpu_edp {
	struct platform_device *pdev;
	struct cpu_edp_platform_data pdata;
	struct tegra_ppm *ppm;
	int temperature;
	cpumask_t edp_cpumask;
	unsigned long freq_limit;
	unsigned long sysedp_freq_limit;
	unsigned long sysedp_cpupwr;
	struct thermal_zone_device *tz;
	unsigned long edp_thermal_index;
	struct mutex edp_lock;
	bool edp_init_done;
};

static struct cpu_edp s_cpu = {
	/* assume we're running hot */
	.temperature = 75,

	/* default cpu power (mW) */
#ifdef CONFIG_TEGRA_SYS_EDP
	.sysedp_cpupwr = 2000,
#else
	/* no cpu power limitation from sysedp */
	.sysedp_cpupwr = 100000,
#endif
};

static unsigned int tegra_get_sysedp_max_freq(struct cpu_edp *ctx,
					      int online_cpus)
{
	if (WARN_ONCE(!ctx->edp_init_done, "Tegra PPM is not ready\n"))
		return 0;

	return tegra_ppm_get_maxf(ctx->ppm, ctx->sysedp_cpupwr,
				  TEGRA_PPM_UNITS_MILLIWATTS,
				  ctx->temperature, online_cpus);
}

static unsigned int tegra_get_edp_max_freq(struct cpu_edp *ctx, int online_cpus)
{
	if (WARN_ONCE(!ctx->edp_init_done, "CPU-EDP is not ready\n"))
		return 0;

	return tegra_ppm_get_maxf(ctx->ppm, ctx->pdata.reg_edp,
				  TEGRA_PPM_UNITS_MILLIAMPS,
				  ctx->temperature, online_cpus);
}

/* Must be called while holding edp_lock */
static void tegra_edp_update_limit(struct device *dev)
{
	struct cpu_edp *ctx = dev_get_drvdata(dev);
	unsigned int limit, cpus;

	BUG_ON(!mutex_is_locked(&ctx->edp_lock));

	cpus = cpumask_weight(&ctx->edp_cpumask);

	limit = tegra_get_edp_max_freq(ctx, cpus);
	ctx->freq_limit = limit;

	limit = tegra_get_sysedp_max_freq(ctx, cpus);
	if (ctx->sysedp_freq_limit != limit) {
		if (IS_ENABLED(CONFIG_DEBUG_KERNEL))
			dev_dbg(dev, "sysedp: ncpus %u, cpu %lu mW %u kHz\n",
				cpus, ctx->sysedp_cpupwr, limit);
#ifdef CONFIG_TEGRA_SYS_EDP
		trace_sysedp_max_cpu_pwr(cpus, ctx->sysedp_cpupwr,
					 limit);
#endif
	}

	ctx->sysedp_freq_limit = limit;
}

/*
 * The CPU EDP driver worked with thermal framework, provided callbacks
 * for the cpu edp recation driver.
 */

int tegra_cpu_edp_get_thermal_index(struct platform_device *pdev)
{
	struct cpu_edp *ctx = dev_get_drvdata(&pdev->dev);

	if (!ctx)
		return 0;
	else
		return ctx->edp_thermal_index;
}
EXPORT_SYMBOL(tegra_cpu_edp_get_thermal_index);

int tegra_cpu_edp_count_therm_floors(struct platform_device *pdev)
{
	/*
	 * The thermal framework doesn't use this value to do anything,
	 * just show the max states in sysfs. This CPU EDP driver will
	 * return trip point temperature as the current cooling state,
	 * eg. the trip is 23C, the cooling state is 23. It will be
	 * more readable than meaningless number.
	 * So we set this max cooling state as a meaningless largish
	 * number.
	 */
	return 1024;
}
EXPORT_SYMBOL(tegra_cpu_edp_count_therm_floors);

int tegra_cpu_edp_update_thermal_index(struct platform_device *pdev,
				       unsigned long new_idx)
{
	struct cpu_edp *ctx = dev_get_drvdata(&pdev->dev);

	if (!cpufreq_get(0))
		return 0;

	mutex_lock(&ctx->edp_lock);

	ctx->edp_thermal_index = new_idx;

	/*
	 * Get temperature, convert from mC to C, and quantize to 4 degrees
	 * it can keep a balance between functionality and efficiency
	 */
	ctx->temperature = (ctx->tz->temperature + 3999) / 4000 * 4;

	ctx->edp_cpumask = *cpu_online_mask;
	tegra_edp_update_limit(&pdev->dev);

	mutex_unlock(&ctx->edp_lock);

	cpufreq_update_policy(0);

	return 0;
}
EXPORT_SYMBOL(tegra_cpu_edp_update_thermal_index);

bool tegra_cpu_edp_ready(void)
{
	return s_cpu.edp_init_done;
}
EXPORT_SYMBOL(tegra_cpu_edp_ready);

#ifdef CONFIG_DEBUG_FS
static int cpu_edp_limit_get(void *data, u64 *val)
{
	struct cpu_edp *ctx = data;
	*val = (u64)tegra_get_edp_max_freq(ctx,
					cpumask_weight(&ctx->edp_cpumask));
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(cpu_edp_limit_fops, cpu_edp_limit_get, NULL, "%lld\n");

static struct dentry *tegra_edp_debugfs_init(struct cpu_edp *ctx,
					     const char *name)
{
	struct dentry *edp_dir;

	edp_dir = debugfs_create_dir(name, NULL);
	if (IS_ERR_OR_NULL(edp_dir))
		return NULL;

	debugfs_create_u32("reg_edp_ma", S_IRUGO | S_IWUSR,
			   edp_dir, &ctx->pdata.reg_edp);
	debugfs_create_u32("temperature", S_IRUGO | S_IWUSR,
			   edp_dir, &ctx->temperature);
	debugfs_create_file("cpu_edp_limit", S_IRUGO,
			    edp_dir, ctx, &cpu_edp_limit_fops);

	return edp_dir;
}
#else
static inline struct dentry *tegra_edp_debugfs_init(struct cpu_edp *ctx,
						    const char *name)
{ return NULL; }
#endif /* CONFIG_DEBUG_FS */

static int tegra_cpu_edp_parse_dt(struct platform_device *pdev,
				  struct cpu_edp *ctx)
{
	struct cpu_edp_platform_data *pdata = &ctx->pdata;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *tz_np;

	tz_np = of_parse_phandle(np, "nvidia,tz", 0);
	if (IS_ERR(tz_np))
		return -ENODATA;
	ctx->tz = thermal_zone_get_zone_by_node(tz_np);
	if (IS_ERR(ctx->tz))
		return -EPROBE_DEFER;

	if (WARN(of_property_read_u32(np, "nvidia,freq-step",
				      &pdata->freq_step),
		 "missing required parameter: nvidia,freq-step\n"))
		return -ENODATA;

	if (WARN(of_property_read_u32(np, "nvidia,edp-limit",
				      &pdata->reg_edp),
		 "missing required parameter: nvidia,edp-limit\n"))
		return -ENODATA;

	return 0;
}

/* Get the supported maximum cpu frequency in Hz */
static unsigned int tegra_edp_get_max_cpu_freq(void)
{
	struct cpufreq_frequency_table *table = cpufreq_frequency_get_table(0);
	unsigned int freq = 0;
	int i;

	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++) {
		if (table[i].frequency == CPUFREQ_ENTRY_INVALID)
			continue;

		if (table[i].frequency > freq)
			freq = table[i].frequency;
	}

	return freq * 1000;
}

/* Governor requested frequency, not higher than edp limits */
static unsigned int tegra_edp_governor_speed(struct cpu_edp *ctx,
					unsigned int requested_speed)
{
	unsigned int speed;

	if ((!ctx->freq_limit) || (!ctx->sysedp_freq_limit))
		speed = max(ctx->freq_limit, ctx->sysedp_freq_limit);
	else
		speed = min(ctx->freq_limit, ctx->sysedp_freq_limit);

	if ((!speed) ||
	    ((requested_speed) && (requested_speed <= speed)))
		return requested_speed;
	else
		return speed;
}

static int max_cpu_power_notifier(struct notifier_block *b,
				unsigned long cpupwr, void *v)
{
	struct platform_device *pdev = s_cpu.pdev;
	struct device *dev = &pdev->dev;

	dev_dbg(dev, "PM QoS Max CPU Power Notify: %lu\n", cpupwr);

	mutex_lock(&s_cpu.edp_lock);
	/* store the new cpu power */
	s_cpu.sysedp_cpupwr = cpupwr;

	tegra_edp_update_limit(dev);
	cpufreq_update_policy(0);
	mutex_unlock(&s_cpu.edp_lock);

	return NOTIFY_OK;
}

static struct notifier_block max_cpu_pwr_notifier_block = {
	.notifier_call = max_cpu_power_notifier,
};

/**
 * tegra_cpu_edp_notifier - Notifier callback for cpu hotplug.
 * @nb: struct notifier_block * with callback info.
 * @event: value showing cpufreq event for which this function invoked.
 * @data: callback-specific data
 *
 * Callback to hijack the notification on cpu hotplug.
 * Every time there is a event of cpu hotplug, we will intercept and
 * update the cpu edp limit and update cpu policy if needed.
 *
 * Return: 0 (success)
 */
static int tegra_cpu_edp_notifier(
	struct notifier_block *nb, unsigned long event, void *data)
{
	struct platform_device *pdev = s_cpu.pdev;
	struct device *dev = &pdev->dev;
	int ret = 0;
	unsigned int cpu_speed, new_speed;
	int cpu = (long)data;

	switch (event) {
	case CPU_UP_PREPARE:
		mutex_lock(&s_cpu.edp_lock);
		cpu_set(cpu, s_cpu.edp_cpumask);
		tegra_edp_update_limit(dev);

		cpu_speed = cpufreq_get(0);
		new_speed = tegra_edp_governor_speed(&s_cpu, cpu_speed);
		if (new_speed < cpu_speed) {
			ret = cpufreq_update_policy(0);
			dev_dbg(dev, "cpu-tegra:%sforce EDP limit %u kHz\n",
				ret ? " failed to " : " ", new_speed);
			if (ret) {
				cpu_clear(cpu, s_cpu.edp_cpumask);
				tegra_edp_update_limit(dev);
			}
		}
		mutex_unlock(&s_cpu.edp_lock);
		break;
	case CPU_DEAD:
		mutex_lock(&s_cpu.edp_lock);
		cpu_clear(cpu, s_cpu.edp_cpumask);
		tegra_edp_update_limit(dev);
		mutex_unlock(&s_cpu.edp_lock);
		cpufreq_update_policy(0);
		break;
	}

	return notifier_from_errno(ret);
}

static struct notifier_block tegra_cpu_edp_notifier_block = {
	.notifier_call = tegra_cpu_edp_notifier,
};

/**
 * edp_cpufreq_policy_notifier - Notifier callback for cpufreq policy change.
 * @nb:	struct notifier_block * with callback info.
 * @event: value showing cpufreq event for which this function invoked.
 * @data: callback-specific data
 *
 * Callback to hijack the notification on cpufreq policy transition.
 * Every time there is a change in policy, we will intercept and
 * update the cpufreq policy with edp constraints.
 *
 * Return: 0 (success)
 */
static int edp_cpufreq_policy_notifier(struct notifier_block *nb,
				       unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned int limit;

	if (event != CPUFREQ_ADJUST)
		return 0;

	/* Limit max freq to be within edp limit. */
	limit = tegra_edp_governor_speed(&s_cpu, 0);
	if (limit && (policy->max != limit))
		cpufreq_verify_within_limits(policy, 0, limit);

	return 0;
}

/* Notifier for cpufreq policy change */
static struct notifier_block edp_cpufreq_notifier_block = {
	.notifier_call = edp_cpufreq_policy_notifier,
};

static int
tegra_cpu_edp_predict_millivolts(void *p, unsigned long rate)
{
	struct clk *clk = (struct clk *)p;

	return tegra_dvfs_predict_millivolts(clk, rate);
}

static int tegra_cpu_edp_probe(struct platform_device *pdev)
{
	char *name = "cpu_edp";
	struct clk *cpu_clk;
	struct fv_relation *fv;
	struct tegra_ppm_params *params;
	unsigned iddq_ma;

	struct cpu_edp *ctx = &s_cpu;
	int ret;
	struct dentry *edp_dir;

	if (!cpufreq_get(0)) {
		dev_warn(&pdev->dev, "CPU clocks are not ready.\n");
		return -EPROBE_DEFER;
	}

	params = of_read_tegra_ppm_params(pdev->dev.of_node);
	if (IS_ERR_OR_NULL(params)) {
		dev_err(&pdev->dev, "Parse ppm parameters failed\n");
		return PTR_ERR(params);
	}

	ret = tegra_cpu_edp_parse_dt(pdev, ctx);
	if (ret) {
		dev_err(&pdev->dev, "Parse CPU EDP parameters failed\n");
		return ret;
	}

	mutex_init(&ctx->edp_lock);

	cpu_clk = clk_get_sys(NULL, "cclk_g");
	fv = fv_relation_create(cpu_clk, ctx->pdata.freq_step, 220,
				tegra_edp_get_max_cpu_freq(), 0,
				tegra_cpu_edp_predict_millivolts);
	if (IS_ERR_OR_NULL(fv)) {
		dev_err(&pdev->dev, "Initialize freq/volt data failed\n");
		return PTR_ERR(fv);
	}

	iddq_ma = tegra_sku_info.cpu_iddq_value;
	dev_dbg(&pdev->dev, "CPU IDDQ value %d\n", iddq_ma);

	edp_dir = tegra_edp_debugfs_init(ctx, name);

	ctx->ppm = tegra_ppm_create(name, fv, params,
				    iddq_ma, edp_dir);
	if (IS_ERR_OR_NULL(ctx->ppm)) {
		dev_err(&pdev->dev, "Create power model failed\n");
		return PTR_ERR(ctx->ppm);
	}

	pm_qos_add_notifier(PM_QOS_MAX_CPU_POWER, &max_cpu_pwr_notifier_block);

	cpufreq_register_notifier(&edp_cpufreq_notifier_block,
				  CPUFREQ_POLICY_NOTIFIER);

	register_hotcpu_notifier(&tegra_cpu_edp_notifier_block);

	platform_set_drvdata(pdev, ctx);

	ctx->pdev = pdev;
	ctx->edp_init_done = true;

	return 0;
}

static int tegra_cpu_edp_remove(struct platform_device *pdev)
{
	struct cpu_edp *ctx = dev_get_drvdata(&pdev->dev);

	unregister_hotcpu_notifier(&tegra_cpu_edp_notifier_block);
	cpufreq_unregister_notifier(&edp_cpufreq_notifier_block,
				    CPUFREQ_POLICY_NOTIFIER);
	pm_qos_remove_notifier(PM_QOS_MAX_CPU_POWER,
				&max_cpu_pwr_notifier_block);
	tegra_ppm_destroy(ctx->ppm, NULL, NULL);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int tegra_cpu_edp_resume(struct device *dev)
{
	struct cpu_edp *ctx = dev_get_drvdata(dev);

	mutex_lock(&ctx->edp_lock);
	ctx->edp_cpumask = *cpu_online_mask;
	tegra_edp_update_limit(dev);
	mutex_unlock(&ctx->edp_lock);

	cpufreq_update_policy(0);

	return 0;
}

static const struct dev_pm_ops tegra_cpu_edp_pm_ops = {
	.resume = tegra_cpu_edp_resume,
};
#endif

static struct of_device_id tegra_cpu_edp_of_match[] = {
	{ .compatible = "nvidia,tegra124-cpu-edp-capping" },
	{ },
};
MODULE_DEVICE_TABLE(of, tegra_cpu_edp_of_match);

static struct platform_driver tegra_cpu_edp_driver = {
	.probe = tegra_cpu_edp_probe,
	.remove = tegra_cpu_edp_remove,
	.driver = {
		.name = "tegra-ppm-cpu-edp",
		.of_match_table = tegra_cpu_edp_of_match,
#ifdef CONFIG_PM_SLEEP
		.pm     = &tegra_cpu_edp_pm_ops,
#endif
	},
};

static int __init tegra_cpu_edp_init(void)
{
	return platform_driver_register(&tegra_cpu_edp_driver);
}

static void __exit tegra_cpu_edp_exit(void)
{
	platform_driver_unregister(&tegra_cpu_edp_driver);
}

module_init(tegra_cpu_edp_init);
module_exit(tegra_cpu_edp_exit);

MODULE_AUTHOR("NVIDIA Corp.");
MODULE_DESCRIPTION("Tegra CPU EDP management");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:tegra-cpu-edp");
