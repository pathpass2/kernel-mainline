/*
 * Goodix Touchscreen Driver
 * Core layer of touchdriver architecture.
 *
 * Copyright (C) 2019 - 2020 Goodix, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be a reference
 * to you, when you are integrating the GOODiX's CTP IC into your system,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/input/mt.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/of_platform.h>
#include <linux/completion.h>
#include <linux/of_irq.h>
#include <linux/pinctrl/consumer.h>

#include "goodix_ts_core.h"

#define INPUT_TYPE_B_PROTOCOL

#define GOOIDX_INPUT_PHYS "goodix_ts/input0"
#define PINCTRL_STATE_ACTIVE "default"
#define PINCTRL_STATE_SUSPEND "sleep"

static void goodix_ts_remove(struct platform_device *pdev);
void goodix_ts_dev_release(void);

#define CORE_MODULE_UNPROBED 0
#define CORE_MODULE_PROB_SUCCESS 1
#define CORE_MODULE_PROB_FAILED -1
#define CORE_MODULE_REMOVED -2
int core_module_prob_sate = CORE_MODULE_UNPROBED;

/* event notifier */
static BLOCKING_NOTIFIER_HEAD(ts_notifier_list);
/**
 * goodix_ts_register_client - register a client notifier
 * @nb: notifier block to callback on events
 *  see enum ts_notify_event in goodix_ts_core.h
 */
static int goodix_ts_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&ts_notifier_list, nb);
}

/**
 * fb_notifier_call_chain - notify clients of fb_events
 *	see enum ts_notify_event in goodix_ts_core.h
 */
static int goodix_ts_blocking_notify(enum ts_notify_event evt, void *v)
{
	int ret;

	ret = blocking_notifier_call_chain(&ts_notifier_list,
					   (unsigned long)evt, v);
	return ret;
}

static void goodix_ts_report_finger(struct input_dev *dev,
				    struct goodix_touch_data *touch_data)
{
	unsigned int touch_num = touch_data->touch_num;
	static u32 pre_fin;
	int i;

	/*first touch down and last touch up condition*/
	if (touch_num && !pre_fin)
		input_report_key(dev, BTN_TOUCH, 1);
	else if (!touch_num && pre_fin)
		input_report_key(dev, BTN_TOUCH, 0);

	pre_fin = touch_num;

	for (i = 0; i < GOODIX_MAX_TOUCH; i++) {
		if (!touch_data->coords[i].status)
			continue;
		if (touch_data->coords[i].status == TS_RELEASE) {
			input_mt_slot(dev, i);
			input_mt_report_slot_state(dev, MT_TOOL_FINGER, false);
			continue;
		}

		input_mt_slot(dev, i);
		input_mt_report_slot_state(dev, MT_TOOL_FINGER, true);
		input_report_abs(dev, ABS_MT_POSITION_X,
				 touch_data->coords[i].x);
		input_report_abs(dev, ABS_MT_POSITION_Y,
				 touch_data->coords[i].y);
		input_report_abs(dev, ABS_MT_TOUCH_MAJOR,
				 touch_data->coords[i].w);
	}

	/* report panel key */
	for (i = 0; i < GOODIX_MAX_TP_KEY; i++) {
		if (!touch_data->keys[i].status)
			continue;
		if (touch_data->keys[i].status == TS_TOUCH)
			input_report_key(dev, touch_data->keys[i].code, 1);
		else if (touch_data->keys[i].status == TS_RELEASE)
			input_report_key(dev, touch_data->keys[i].code, 0);
	}
	input_sync(dev);
}

/**
 * goodix_ts_threadirq_func - Bottom half of interrupt
 * This functions is executed in thread context,
 * sleep in this function is permit.
 *
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static irqreturn_t goodix_ts_threadirq_func(int irq __attribute__((unused)),
					    void *data)
{
	struct goodix_ts_core *core_data = data;
	struct goodix_ts_device *ts_dev = core_data->ts_dev;
	struct goodix_ts_event *ts_event = &core_data->ts_event;
	u8 irq_flag = 0;
	int r;

	core_data->irq_trig_cnt++;

	/* read touch data from touch device */
	r = ts_dev->hw_ops->event_handler(ts_dev, ts_event);
	if (likely(r >= 0)) {
		if (ts_event->event_type == EVENT_TOUCH) {
			/* report touch */
			goodix_ts_report_finger(core_data->input_dev,
						&ts_event->touch_data);
		}
	}

	/* clean irq flag */
	irq_flag = 0;
	ts_dev->hw_ops->write_trans(ts_dev, ts_dev->reg.coor, &irq_flag, 1);

	return IRQ_HANDLED;
}

/**
 * goodix_ts_init_irq - Requset interrupt line from system
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
int goodix_ts_irq_setup(struct goodix_ts_core *core_data)
{
	const struct goodix_ts_board_data *ts_bdata = board_data(core_data);
	int r;

	/* if ts_bdata-> irq is invalid */
	if (ts_bdata->irq <= 0)
		core_data->irq = gpio_to_irq(ts_bdata->irq_gpio);
	else
		core_data->irq = ts_bdata->irq;

	ts_debug("IRQ:%u,flags:%d", core_data->irq, (int)ts_bdata->irq_flags);
	r = devm_request_threaded_irq(&core_data->pdev->dev, core_data->irq,
				      NULL, goodix_ts_threadirq_func,
				      ts_bdata->irq_flags | IRQF_ONESHOT,
				      GOODIX_CORE_DRIVER_NAME, core_data);
	if (r < 0)
		ts_err("Failed to request threaded irq:%d", r);
	else
		atomic_set(&core_data->irq_enabled, 1);

	return r;
}

/**
 * goodix_ts_irq_enable - Enable/Disable a irq
 * @core_data: pointer to touch core data
 * enable: enable or disable irq
 * return: 0 ok, <0 failed
 */
int goodix_ts_irq_enable(struct goodix_ts_core *core_data, bool enable)
{
	if (enable) {
		if (!atomic_cmpxchg(&core_data->irq_enabled, 0, 1)) {
			enable_irq(core_data->irq);
			ts_debug("Irq enabled");
		}
	} else {
		if (atomic_cmpxchg(&core_data->irq_enabled, 1, 0)) {
			disable_irq(core_data->irq);
			ts_debug("Irq disabled");
		}
	}

	return 0;
}
EXPORT_SYMBOL(goodix_ts_irq_enable);

/**
 * goodix_ts_power_init - Get regulator for touch device
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static int goodix_ts_power_init(struct goodix_ts_core *core_data)
{
	struct goodix_ts_board_data *ts_bdata;
	struct device *dev = NULL;
	int r = 0;

	ts_debug("Power init");
	/* dev:i2c client device or spi slave device*/
	dev = core_data->ts_dev->dev;
	ts_bdata = board_data(core_data);

	if (strlen(ts_bdata->avdd_name)) {
		core_data->avdd = devm_regulator_get(dev, ts_bdata->avdd_name);
		if (IS_ERR_OR_NULL(core_data->avdd)) {
			r = PTR_ERR(core_data->avdd);
			ts_err("Failed to get regulator avdd:%d", r);
			core_data->avdd = NULL;
			return r;
		}
		core_data->avdd_load = ts_bdata->avdd_load;
	} else {
		ts_debug("Avdd name is NULL[skip]");
	}

	return r;
}

/**
 * goodix_ts_power_on - Turn on power to the touch device
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
int goodix_ts_power_on(struct goodix_ts_core *core_data)
{
	struct goodix_ts_board_data *ts_bdata = board_data(core_data);
	int r;

	ts_debug("Device power on");
	if (core_data->power_on)
		return 0;

	if (!core_data->avdd) {
		core_data->power_on = 1;
		return 0;
	}

	if (core_data->avdd_load) {
		r = regulator_set_load(core_data->avdd, core_data->avdd_load);
		if (r < 0)
			ts_err("enable 3v3 fail!");
		r = regulator_set_voltage(core_data->avdd, 3000000, 3000000);
		if (r < 0)
			ts_err("enable 3v3 fail!");

		ts_debug("set regulator load SUCCESS");
	}

	r = regulator_enable(core_data->avdd);
	if (!r) {
		ts_debug("regulator enable SUCCESS");
		if (ts_bdata->power_on_delay_us)
			usleep_range(ts_bdata->power_on_delay_us,
				     ts_bdata->power_on_delay_us);
	} else {
		ts_err("Failed to enable analog power:%d", r);
		return r;
	}

	if (ts_bdata->vdd_gpio)
		gpio_direction_output(ts_bdata->vdd_gpio, 1);

	core_data->power_on = 1;
	return 0;
}

/**
 * goodix_ts_power_off - Turn off power to the touch device
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
int goodix_ts_power_off(struct goodix_ts_core *core_data)
{
	struct goodix_ts_board_data *ts_bdata = board_data(core_data);
	int r;

	ts_debug("Device power off");
	if (!core_data->power_on)
		return 0;

	if (core_data->avdd) {
		r = regulator_disable(core_data->avdd);
		if (!r) {
			ts_debug("regulator disable SUCCESS");
			if (ts_bdata->power_off_delay_us)
				usleep_range(ts_bdata->power_off_delay_us,
					     ts_bdata->power_off_delay_us);
		} else {
			ts_err("Failed to disable analog power:%d", r);
			return r;
		}
	}

	if (ts_bdata->vdd_gpio)
		gpio_direction_output(ts_bdata->vdd_gpio, 0);

	core_data->power_on = 0;
	return 0;
}

#ifdef CONFIG_PINCTRL
/**
 * goodix_ts_pinctrl_init - Get pinctrl handler and pinctrl_state
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static int goodix_ts_pinctrl_init(struct goodix_ts_core *core_data)
{
	int r = 0;

	/* get pinctrl handler from of node */
	core_data->pinctrl = devm_pinctrl_get(core_data->ts_dev->dev);
	if (IS_ERR_OR_NULL(core_data->pinctrl)) {
		ts_debug("Failed to get pinctrl handler[need confirm]");
		core_data->pinctrl = NULL;
		return -EINVAL;
	}
	ts_debug("success get pinctrl");
	/* active state */
	core_data->pin_sta_active =
		pinctrl_lookup_state(core_data->pinctrl, PINCTRL_STATE_ACTIVE);
	if (IS_ERR_OR_NULL(core_data->pin_sta_active)) {
		r = PTR_ERR(core_data->pin_sta_active);
		ts_err("Failed to get pinctrl state:%s, r:%d",
		       PINCTRL_STATE_ACTIVE, r);
		core_data->pin_sta_active = NULL;
		goto exit_pinctrl_put;
	}
	ts_debug("success get active pinctrl state");

	/* suspend state */
	core_data->pin_sta_suspend =
		pinctrl_lookup_state(core_data->pinctrl, PINCTRL_STATE_SUSPEND);
	if (IS_ERR_OR_NULL(core_data->pin_sta_suspend)) {
		r = PTR_ERR(core_data->pin_sta_suspend);
		ts_err("Failed to get pinctrl state:%s, r:%d",
		       PINCTRL_STATE_SUSPEND, r);
		core_data->pin_sta_suspend = NULL;
		goto exit_pinctrl_put;
	}
	ts_debug("success get suspend pinctrl state");

	return 0;
exit_pinctrl_put:
	devm_pinctrl_put(core_data->pinctrl);
	core_data->pinctrl = NULL;
	return r;
}
#endif

/**
 * goodix_ts_gpio_setup - Request gpio resources from GPIO subsysten
 *	reset_gpio and irq_gpio number are obtained from goodix_ts_device
 *  which created in hardware layer driver. e.g.goodix_xx_i2c.c
 *	A goodix_ts_device should set those two fields to right value
 *	before registered to touch core driver.
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static int goodix_ts_gpio_setup(struct goodix_ts_core *core_data)
{
	struct goodix_ts_board_data *ts_bdata = board_data(core_data);
	int r = 0;

	ts_debug("GPIO setup,reset-gpio:%d, irq-gpio:%d", ts_bdata->reset_gpio,
		 ts_bdata->irq_gpio);

	if (ts_bdata->vdd_gpio) {
		r = devm_gpio_request_one(&core_data->pdev->dev,
					  ts_bdata->vdd_gpio,
					  GPIOF_OUT_INIT_HIGH, "ts_vdd_gpio");
		if (r < 0) {
			ts_err("Failed to request vdd gpio, r:%d", r);
			return r;
		}
	}

	r = devm_gpio_request_one(&core_data->pdev->dev, ts_bdata->reset_gpio,
				  GPIOF_OUT_INIT_HIGH, "ts_reset_gpio");
	if (r < 0) {
		ts_err("Failed to request reset gpio, r:%d", r);
		return r;
	}

	r = devm_gpio_request_one(&core_data->pdev->dev, ts_bdata->irq_gpio,
				  GPIOF_IN, "ts_irq_gpio");
	if (r < 0) {
		ts_err("Failed to request irq gpio, r:%d", r);
		return r;
	}

	return 0;
}

/**
 * goodix_input_set_params - set input parameters
 */
static void goodix_ts_set_input_params(struct input_dev *input_dev,
				       struct goodix_ts_board_data *ts_bdata)
{
	unsigned int i;

	if (ts_bdata->swap_axis)
		swap(ts_bdata->panel_max_x, ts_bdata->panel_max_y);

	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0,
			     ts_bdata->panel_max_x, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0,
			     ts_bdata->panel_max_y, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0,
			     ts_bdata->panel_max_w, 0, 0);

	if (ts_bdata->panel_max_key) {
		for (i = 0; i < ts_bdata->panel_max_key; i++)
			input_set_capability(input_dev, EV_KEY,
					     ts_bdata->panel_key_map[i]);
	}
}

/**
 * goodix_ts_input_dev_config - Requset and config a input device
 *  then register it to input subsystem.
 *  NOTE that some hardware layer may provide a input device
 *  (ts_dev->input_dev not NULL).
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static int goodix_ts_input_dev_config(struct goodix_ts_core *core_data)
{
	struct goodix_ts_board_data *ts_bdata = board_data(core_data);
	struct input_dev *input_dev = NULL;
	int r;

	input_dev = input_allocate_device();
	if (!input_dev) {
		ts_err("Failed to allocated input device");
		return -ENOMEM;
	}

	core_data->input_dev = input_dev;
	input_set_drvdata(input_dev, core_data);

	input_dev->name = GOODIX_CORE_DRIVER_NAME;
	input_dev->phys = GOOIDX_INPUT_PHYS;
	input_dev->id.product = 0xDEAD;
	input_dev->id.vendor = 0xBEEF;
	input_dev->id.version = 10427;

	__set_bit(EV_SYN, input_dev->evbit);
	__set_bit(EV_KEY, input_dev->evbit);
	__set_bit(EV_ABS, input_dev->evbit);
	__set_bit(BTN_TOUCH, input_dev->keybit);
	__set_bit(BTN_TOOL_FINGER, input_dev->keybit);

#ifdef INPUT_PROP_DIRECT
	__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);
#endif

	/* set input parameters */
	goodix_ts_set_input_params(input_dev, ts_bdata);

#ifdef INPUT_TYPE_B_PROTOCOL
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 7, 0)
	input_mt_init_slots(input_dev, GOODIX_MAX_TOUCH, INPUT_MT_DIRECT);
#else
	input_mt_init_slots(input_dev, GOODIX_MAX_TOUCH);
#endif
#endif

	input_set_capability(input_dev, EV_KEY, KEY_POWER);

	r = input_register_device(input_dev);
	if (r < 0) {
		ts_err("Unable to register input device");
		input_free_device(input_dev);
		return r;
	}

	return 0;
}

static void goodix_ts_input_dev_remove(struct goodix_ts_core *core_data)
{
	input_unregister_device(core_data->input_dev);
	input_free_device(core_data->input_dev);
	core_data->input_dev = NULL;
}

static void goodix_ts_release_connects(struct goodix_ts_core *core_data)
{
	struct input_dev *input_dev = core_data->input_dev;
	struct input_mt *mt = input_dev->mt;
	int i;

	if (mt) {
		for (i = 0; i < mt->num_slots; i++) {
			input_mt_slot(input_dev, i);
			input_mt_report_slot_state(input_dev, MT_TOOL_FINGER,
						   false);
		}
		input_report_key(input_dev, BTN_TOUCH, 0);
		input_mt_sync_frame(input_dev);
		input_sync(input_dev);
	}
}

/**
 * goodix_ts_suspend - Touchscreen suspend function
 * Called by PM/FB/EARLYSUSPEN module to put the device to  sleep
 */
static int goodix_ts_suspend(struct goodix_ts_core *core_data)
{
	struct goodix_ts_device *ts_dev = core_data->ts_dev;
	int r;

	ts_debug("Suspend start");

	/*
	 * notify suspend event, inform the esd protector
	 * and charger detector to turn off the work
	 */
	goodix_ts_blocking_notify(NOTIFY_SUSPEND, NULL);

	/* disable irq */
	goodix_ts_irq_enable(core_data, false);

	/* let touch ic work in sleep mode */
	if (ts_dev && ts_dev->hw_ops->suspend)
		ts_dev->hw_ops->suspend(ts_dev);
	atomic_set(&core_data->suspended, 1);

#ifdef CONFIG_PINCTRL
	if (core_data->pinctrl) {
		r = pinctrl_select_state(core_data->pinctrl,
					 core_data->pin_sta_suspend);
		if (r < 0)
			ts_err("Failed to select active pinstate, r:%d", r);
	}
#endif

	ts_debug("Suspend end");
	return 0;
}

/**
 * goodix_ts_resume - Touchscreen resume function
 * Called by PM/FB/EARLYSUSPEN module to wakeup device
 */
static int goodix_ts_resume(struct goodix_ts_core *core_data)
{
	struct goodix_ts_device *ts_dev = core_data->ts_dev;
	int r;

	ts_debug("Resume start");
	goodix_ts_release_connects(core_data);

#ifdef CONFIG_PINCTRL
	if (core_data->pinctrl) {
		r = pinctrl_select_state(core_data->pinctrl,
					 core_data->pin_sta_active);
		if (r < 0)
			ts_err("Failed to select active pinstate, r:%d", r);
	}
#endif

	atomic_set(&core_data->suspended, 0);
	/* resume device */
	if (ts_dev && ts_dev->hw_ops->resume)
		ts_dev->hw_ops->resume(ts_dev);

	goodix_ts_irq_enable(core_data, true);

	/*
	 * notify resume event, inform the esd protector
	 * and charger detector to turn on the work
	 */
	ts_debug("try notify resume");
	goodix_ts_blocking_notify(NOTIFY_RESUME, NULL);
	ts_debug("Resume end");
	return 0;
}

#ifdef CONFIG_PM
/**
 * goodix_ts_pm_suspend - PM suspend function
 * Called by kernel during system suspend phrase
 */
static int goodix_ts_pm_suspend(struct device *dev)
{
	struct goodix_ts_core *core_data = dev_get_drvdata(dev);

	return goodix_ts_suspend(core_data);
}
/**
 * goodix_ts_pm_resume - PM resume function
 * Called by kernel during system wakeup
 */
static int goodix_ts_pm_resume(struct device *dev)
{
	struct goodix_ts_core *core_data = dev_get_drvdata(dev);

	return goodix_ts_resume(core_data);
}
#endif

static int goodix_ts_init(struct goodix_ts_core *core_data)
{
	int r;
	struct goodix_ts_device *ts_dev = ts_device(core_data);

	r = ts_dev->hw_ops->read_version(ts_dev, &ts_dev->chip_version);
	if (r < 0)
		ts_debug("failed read fw version info[ignore]");

	/* alloc/config/register input device */
	r = goodix_ts_input_dev_config(core_data);
	if (r < 0) {
		ts_err("failed set input device");
		return r;
	}

	/* request irq line */
	r = goodix_ts_irq_setup(core_data);
	if (r < 0) {
		ts_debug("failed set irq");
		goto err_finger;
	}
	ts_debug("success register irq");

	return 0;
err_finger:
	goodix_ts_input_dev_remove(core_data);
	return r;
}

/**
 * goodix_ts_probe - called by kernel when a Goodix touch
 *  platform driver is added.
 */
static int goodix_ts_probe(struct platform_device *pdev)
{
	struct goodix_ts_core *core_data = NULL;
	struct goodix_ts_device *ts_device;
	int r;

	ts_debug("goodix_ts_probe IN");

	ts_device = pdev->dev.platform_data;
	if (!ts_device || !ts_device->hw_ops) {
		ts_err("Invalid touch device");
		core_module_prob_sate = CORE_MODULE_PROB_FAILED;
		return -ENODEV;
	}

	core_data = kzalloc(sizeof(struct goodix_ts_core), GFP_KERNEL);
	if (!core_data) {
		ts_err("Failed to allocate memory for core data");
		core_module_prob_sate = CORE_MODULE_PROB_FAILED;
		return -ENOMEM;
	}

	/* touch core layer is a platform driver */
	core_data->pdev = pdev;
	core_data->ts_dev = ts_device;
	platform_set_drvdata(pdev, core_data);

	r = goodix_ts_power_init(core_data);
	if (r < 0)
		goto out;

	r = goodix_ts_power_on(core_data);
	if (r < 0)
		goto out;

#ifdef CONFIG_PINCTRL
	/* Pinctrl handle is optional. */
	r = goodix_ts_pinctrl_init(core_data);
	if (!r && core_data->pinctrl) {
		r = pinctrl_select_state(core_data->pinctrl,
					 core_data->pin_sta_active);
		if (r < 0)
			ts_err("Failed to select active pinstate, r:%d", r);
	}
#endif

	/* get GPIO resource */
	r = goodix_ts_gpio_setup(core_data);
	if (r < 0)
		goto out;

	/* confirm it's goodix touch dev or not */
	r = ts_device->hw_ops->dev_confirm(ts_device);
	if (r) {
		ts_err("goodix device confirm failed[skip]");
		goto out;
	}

	core_data->initialized = 1;
	core_module_prob_sate = CORE_MODULE_PROB_SUCCESS;
out:
	if (r) {
		kfree(core_data);
		core_data = NULL;
		core_module_prob_sate = CORE_MODULE_PROB_FAILED;
	} else {
		goodix_ts_init(core_data);
	}

	ts_info("goodix_ts_probe OUT, r:%d", r);

	return r;
}

static void goodix_ts_remove(struct platform_device *pdev)
{
	struct goodix_ts_core *core_data = platform_get_drvdata(pdev);

	core_data->initialized = 0;
	core_module_prob_sate = CORE_MODULE_REMOVED;
	goodix_ts_power_off(core_data);
	kfree(core_data);
}

#ifdef CONFIG_PM
static const struct dev_pm_ops dev_pm_ops = {
	.suspend = goodix_ts_pm_suspend,
	.resume = goodix_ts_pm_resume,
};
#endif

static const struct platform_device_id ts_core_ids[] = {
	{ .name = GOODIX_CORE_DRIVER_NAME },
	{}
};
MODULE_DEVICE_TABLE(platform, ts_core_ids);

static struct platform_driver goodix_ts_driver = {
	.driver = {
		.name = GOODIX_CORE_DRIVER_NAME,
		.owner = THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &dev_pm_ops,
#endif
	},
	.probe = goodix_ts_probe,
	.remove = goodix_ts_remove,
	.id_table = ts_core_ids,
};

int goodix_ts_core_init(void)
{
	ts_debug("Core layer init");
	return platform_driver_register(&goodix_ts_driver);
}
