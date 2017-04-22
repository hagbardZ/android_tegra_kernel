/*
 * drivers/thermal/pd_thermal_gov.c
 *
 * Copyright (c) 2013, NVIDIA CORPORATION.  All rights reserved.

 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.

 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/thermal.h>

#include "thermal_core.h"

#define DRV_NAME	"pd_thermal_gov"

#define MAX_ERR_TEMP_DEFAULT		9000	/* in mC */
#define MAX_ERR_GAIN_DEFAULT		1000
#define GAIN_P_DEFAULT			1000
#define GAIN_D_DEFAULT			0
#define UP_COMPENSATION_DEFAULT		20
#define DOWN_COMPENSATION_DEFAULT	20

struct pd_thermal_gov_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct attribute *attr,
			char *buf);
	ssize_t (*store)(struct kobject *kobj, struct attribute *attr,
			 const char *buf, size_t count);
};

struct pd_thermal_governor {
	struct kobject kobj;

	int max_err_temp; /* max error temperature in mC */
	int max_err_gain; /* max error gain */

	int gain_p; /* proportional gain */
	int gain_d; /* derivative gain */

	/* max derivative output, percentage of max error */
	unsigned long max_dout;

	unsigned long up_compensation;
	unsigned long down_compensation;
};

#define tz_to_gov(t)		\
	(t->governor_data)

#define kobj_to_gov(k)		\
	container_of(k, struct pd_thermal_governor, kobj)

#define attr_to_gov_attr(a)	\
	container_of(a, struct pd_thermal_gov_attribute, attr)

static ssize_t max_err_temp_show(struct kobject *kobj, struct attribute *attr,
				 char *buf)
{
	struct pd_thermal_governor *gov = kobj_to_gov(kobj);

	if (!gov)
		return -ENODEV;

	return sprintf(buf, "%d\n", gov->max_err_temp);
}

static ssize_t max_err_temp_store(struct kobject *kobj, struct attribute *attr,
				  const char *buf, size_t count)
{
	struct pd_thermal_governor *gov = kobj_to_gov(kobj);
	int val;

	if (!gov)
		return -ENODEV;

	if (!sscanf(buf, "%d\n", &val))
		return -EINVAL;

	gov->max_err_temp = val;
	return count;
}

static struct pd_thermal_gov_attribute max_err_temp_attr =
	__ATTR(max_err_temp, 0644, max_err_temp_show, max_err_temp_store);

static ssize_t max_err_gain_show(struct kobject *kobj, struct attribute *attr,
				 char *buf)
{
	struct pd_thermal_governor *gov = kobj_to_gov(kobj);

	if (!gov)
		return -ENODEV;

	return sprintf(buf, "%d\n", gov->max_err_gain);
}

static ssize_t max_err_gain_store(struct kobject *kobj, struct attribute *attr,
				  const char *buf, size_t count)
{
	struct pd_thermal_governor *gov = kobj_to_gov(kobj);
	int val;

	if (!gov)
		return -ENODEV;

	if (!sscanf(buf, "%d\n", &val))
		return -EINVAL;

	gov->max_err_gain = val;
	return count;
}

static struct pd_thermal_gov_attribute max_err_gain_attr =
	__ATTR(max_err_gain, 0644, max_err_gain_show, max_err_gain_store);

static ssize_t max_dout_show(struct kobject *kobj,
			     struct attribute *attr, char *buf)
{
	struct pd_thermal_governor *gov = kobj_to_gov(kobj);

	if (!gov)
		return -ENODEV;

	return sprintf(buf, "%lu\n", gov->max_dout);
}

static ssize_t max_dout_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	struct pd_thermal_governor *gov = kobj_to_gov(kobj);
	unsigned long val;

	if (!gov)
		return -ENODEV;

	if (!sscanf(buf, "%lu\n", &val))
		return -EINVAL;

	gov->max_dout = val;
	return count;
}

static struct pd_thermal_gov_attribute max_dout_attr =
	__ATTR(max_dout, 0644, max_dout_show, max_dout_store);

static ssize_t gain_p_show(struct kobject *kobj, struct attribute *attr,
			   char *buf)
{
	struct pd_thermal_governor *gov = kobj_to_gov(kobj);

	if (!gov)
		return -ENODEV;

	return sprintf(buf, "%d\n", gov->gain_p);
}

static ssize_t gain_p_store(struct kobject *kobj, struct attribute *attr,
			    const char *buf, size_t count)
{
	struct pd_thermal_governor *gov = kobj_to_gov(kobj);
	int val;

	if (!gov)
		return -ENODEV;

	if (!sscanf(buf, "%d\n", &val))
		return -EINVAL;

	gov->gain_p = val;
	return count;
}

static struct pd_thermal_gov_attribute gain_p_attr =
	__ATTR(gain_p, 0644, gain_p_show, gain_p_store);

static ssize_t gain_d_show(struct kobject *kobj, struct attribute *attr,
			   char *buf)
{
	struct pd_thermal_governor *gov = kobj_to_gov(kobj);

	if (!gov)
		return -ENODEV;

	return sprintf(buf, "%d\n", gov->gain_d);
}

static ssize_t gain_d_store(struct kobject *kobj, struct attribute *attr,
			    const char *buf, size_t count)
{
	struct pd_thermal_governor *gov = kobj_to_gov(kobj);
	int val;

	if (!gov)
		return -ENODEV;

	if (!sscanf(buf, "%d\n", &val))
		return -EINVAL;

	gov->gain_d = val;
	return count;
}

static struct pd_thermal_gov_attribute gain_d_attr =
	__ATTR(gain_d, 0644, gain_d_show, gain_d_store);

static ssize_t up_compensation_show(struct kobject *kobj,
				    struct attribute *attr, char *buf)
{
	struct pd_thermal_governor *gov = kobj_to_gov(kobj);

	if (!gov)
		return -ENODEV;

	return sprintf(buf, "%lu\n", gov->up_compensation);
}

static ssize_t up_compensation_store(struct kobject *kobj,
				     struct attribute *attr, const char *buf,
				     size_t count)
{
	struct pd_thermal_governor *gov = kobj_to_gov(kobj);
	unsigned long val;

	if (!gov)
		return -ENODEV;

	if (!sscanf(buf, "%lu\n", &val))
		return -EINVAL;

	gov->up_compensation = val;
	return count;
}

static struct pd_thermal_gov_attribute up_compensation_attr =
	__ATTR(up_compensation, 0644,
	       up_compensation_show, up_compensation_store);

static ssize_t down_compensation_show(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	struct pd_thermal_governor *gov = kobj_to_gov(kobj);

	if (!gov)
		return -ENODEV;

	return sprintf(buf, "%lu\n", gov->down_compensation);
}

static ssize_t down_compensation_store(struct kobject *kobj,
				       struct attribute *attr, const char *buf,
				       size_t count)
{
	struct pd_thermal_governor *gov = kobj_to_gov(kobj);
	unsigned long val;

	if (!gov)
		return -ENODEV;

	if (!sscanf(buf, "%lu\n", &val))
		return -EINVAL;

	gov->down_compensation = val;
	return count;
}

static struct pd_thermal_gov_attribute down_compensation_attr =
	__ATTR(down_compensation, 0644,
	       down_compensation_show, down_compensation_store);

static struct attribute *pd_thermal_gov_default_attrs[] = {
	&max_err_temp_attr.attr,
	&max_err_gain_attr.attr,
	&gain_p_attr.attr,
	&gain_d_attr.attr,
	&max_dout_attr.attr,
	&up_compensation_attr.attr,
	&down_compensation_attr.attr,
	NULL,
};

static ssize_t pd_thermal_gov_show(struct kobject *kobj,
				    struct attribute *attr, char *buf)
{
	struct pd_thermal_gov_attribute *gov_attr = attr_to_gov_attr(attr);

	if (!gov_attr->show)
		return -EIO;

	return gov_attr->show(kobj, attr, buf);
}

static ssize_t pd_thermal_gov_store(struct kobject *kobj,
				     struct attribute *attr, const char *buf,
				     size_t len)
{
	struct pd_thermal_gov_attribute *gov_attr = attr_to_gov_attr(attr);

	if (!gov_attr->store)
		return -EIO;

	return gov_attr->store(kobj, attr, buf, len);
}

static const struct sysfs_ops pd_thermal_gov_sysfs_ops = {
	.show	= pd_thermal_gov_show,
	.store	= pd_thermal_gov_store,
};

static struct kobj_type pd_thermal_gov_ktype = {
	.default_attrs	= pd_thermal_gov_default_attrs,
	.sysfs_ops	= &pd_thermal_gov_sysfs_ops,
};

static int pd_thermal_gov_bind(struct thermal_zone_device *tz)
{
	struct pd_thermal_governor *gov;
	int ret;

	gov = kzalloc(sizeof(struct pd_thermal_governor), GFP_KERNEL);
	if (!gov) {
		dev_err(&tz->device, "%s: Can't alloc governor data\n",
			DRV_NAME);
		return -ENOMEM;
	}

	ret = kobject_init_and_add(&gov->kobj, &pd_thermal_gov_ktype,
				   &tz->device.kobj, DRV_NAME);
	if (ret) {
		dev_err(&tz->device, "%s: Can't init kobject\n", DRV_NAME);
		kobject_put(&gov->kobj);
		kfree(gov);
		return ret;
	}

	gov->max_err_temp = MAX_ERR_TEMP_DEFAULT;
	gov->max_err_gain = MAX_ERR_GAIN_DEFAULT;
	gov->gain_p = GAIN_P_DEFAULT;
	gov->gain_d = GAIN_D_DEFAULT;
	gov->up_compensation = UP_COMPENSATION_DEFAULT;
	gov->down_compensation = DOWN_COMPENSATION_DEFAULT;

	tz->governor_data = gov;

	return 0;
}

static void pd_thermal_gov_unbind(struct thermal_zone_device *tz)
{
	struct pd_thermal_governor *gov = tz_to_gov(tz);

	if (!gov)
		return;

	kobject_put(&gov->kobj);
	kfree(gov);
}

static void pd_thermal_gov_update_passive(struct thermal_zone_device *tz,
					   enum thermal_trip_type trip_type,
					   unsigned long old,
					   unsigned long new)
{
	if ((trip_type != THERMAL_TRIP_PASSIVE) &&
	    (trip_type != THERMAL_TRIPS_NONE))
		return;

	if ((old == THERMAL_NO_TARGET) && (new != THERMAL_NO_TARGET))
		tz->passive++;
	else if ((old != THERMAL_NO_TARGET) && (new == THERMAL_NO_TARGET))
		tz->passive--;
}

static unsigned long
pd_thermal_gov_get_target(struct thermal_zone_device *tz,
			   struct thermal_cooling_device *cdev,
			   enum thermal_trip_type trip_type,
			   int trip_temp)
{
	struct pd_thermal_governor *gov = tz_to_gov(tz);
	int last_temperature = tz->passive ? tz->last_temperature : trip_temp;
	int passive_delay = tz->passive ? tz->passive_delay : MSEC_PER_SEC;
	s64 proportional, derivative, sum_err, max_err;
	unsigned long max_state, cur_state, target, compensation;

	if (cdev->ops->get_max_state(cdev, &max_state) < 0)
		return 0;

	if (cdev->ops->get_cur_state(cdev, &cur_state) < 0)
		return 0;

	max_err = (s64)gov->max_err_temp * (s64)gov->max_err_gain;

	/* Calculate proportional term */
	proportional = tz->temperature - trip_temp;
	proportional *= gov->gain_p;

	/* Calculate derivative term */
	derivative = tz->temperature - last_temperature;
	derivative *= gov->gain_d;
	derivative *= MSEC_PER_SEC;
	derivative = div64_s64(derivative, passive_delay);
	if (gov->max_dout) {
		s64 max_dout = div64_s64(max_err * gov->max_dout, 100);
		if (derivative < 0)
			derivative = -min_t(s64, abs64(derivative), max_dout);
		else
			derivative = min_t(s64, derivative, max_dout);
	}

	sum_err = max_t(s64, proportional + derivative, 0);
	sum_err = min_t(s64, sum_err, max_err);
	sum_err = sum_err * max_state + max_err - 1;
	target = (unsigned long)div64_s64(sum_err, max_err);

	/* Apply compensation */
	if (target == cur_state)
		return target;

	if (target > cur_state) {
		compensation = DIV_ROUND_UP(gov->up_compensation *
					    (target - cur_state), 100);
		target = min(cur_state + compensation, max_state);
	} else if (target < cur_state) {
		compensation = DIV_ROUND_UP(gov->down_compensation *
					    (cur_state - target), 100);
		target = (cur_state > compensation) ?
			 (cur_state - compensation) : 0;
	}

	return target;
}

static int pd_thermal_gov_throttle(struct thermal_zone_device *tz, int trip)
{
	struct thermal_instance *instance;
	enum thermal_trip_type trip_type;
	int trip_temp, hyst;
	unsigned long target;

	tz->ops->get_trip_type(tz, trip, &trip_type);
	tz->ops->get_trip_temp(tz, trip, &trip_temp);
	tz->ops->get_trip_hyst(tz, trip, &hyst);

	mutex_lock(&tz->lock);

	list_for_each_entry(instance, &tz->thermal_instances, tz_node) {
		struct thermal_cooling_device *cdev;
		unsigned long cur_state;

		if ((instance->trip != trip) ||
		    ((tz->temperature < trip_temp) &&
		    (instance->target == THERMAL_NO_TARGET)))
			continue;

		cdev = instance->cdev;
		target = pd_thermal_gov_get_target(tz, cdev,
						   trip_type, trip_temp);
		if (target >= instance->upper)
			target = instance->upper;
		else if (target < instance->lower)
			target = instance->lower;

		if ((tz->temperature < (trip_temp - hyst)) &&
		    (instance->target == instance->lower) &&
		    (target == instance->lower))
			target = THERMAL_NO_TARGET;

		if (!((cdev->ops->get_cur_state(cdev, &cur_state) == 0) &&
		      (cur_state != instance->target)))
			if (instance->target == target)
				continue;

		pd_thermal_gov_update_passive(tz, trip_type, instance->target,
					       target);
		instance->target = target;
		instance->cdev->updated = false;
	}

	list_for_each_entry(instance, &tz->thermal_instances, tz_node)
		thermal_cdev_update(instance->cdev);

	mutex_unlock(&tz->lock);

	return 0;
}

static struct thermal_governor pd_thermal_gov = {
	.name		= DRV_NAME,
	.bind_to_tz	= pd_thermal_gov_bind,
	.unbind_from_tz	= pd_thermal_gov_unbind,
	.throttle	= pd_thermal_gov_throttle,
};

int thermal_gov_pd_register(void)
{
	return thermal_register_governor(&pd_thermal_gov);
}

void thermal_gov_pd_unregister(void)
{
	thermal_unregister_governor(&pd_thermal_gov);
}
