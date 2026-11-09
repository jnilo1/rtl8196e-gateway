// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIO LED driver with software PWM brightness control
 *
 * Drop-in replacement for leds-gpio that adds brightness control (0-255)
 * via low-frequency software PWM using kernel timer_list (jiffies-based).
 * At brightness 0 or max the timer is stopped (zero CPU overhead).
 *
 * Uses HZ-based timers (250 Hz on RTL8196E) instead of hrtimers to avoid
 * hard-IRQ interference with UART transfers.
 *
 * PWM period = PWM_PERIOD_JIFFIES jiffies. With HZ=250 and period=4:
 *   PWM frequency = 62.5 Hz (above flicker threshold).
 *   Brightness 60/255 ≈ 1/4 duty cycle (25%).
 *
 * The sysfs interface keeps the standard LED-class 0-255 scale, but the
 * 4-jiffy window quantizes intermediate values to 4 physical duty levels
 * (25/50/75/100%). Raising the resolution would lower the PWM frequency
 * in the same ratio (8 levels -> 31 Hz: visible flicker), so 4 levels at
 * 62.5 Hz is the deliberate trade-off (issue #120).
 *
 * DTS compatible: "gpio-leds-pwm" (gpio-leds-like supported subset)
 *
 * Copyright (C) 2025 Jacques Nilo
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/leds.h>
#include <linux/of.h>
#include <linux/timer.h>
#include <linux/slab.h>

#define DRV_VERSION		"1.2"

#define PWM_PERIOD_JIFFIES	4	/* 4 jiffies = 16ms @ HZ=250 → 62.5 Hz */
#define MAX_BRIGHTNESS		255

struct gpio_pwm_led {
	struct led_classdev	cdev;
	struct gpio_desc	*gpiod;
	struct timer_list	timer;
	unsigned int		threshold;	/* quantized ON jiffies/period */
	unsigned int		counter;	/* position in PWM cycle   */
	bool			pwm_active;	/* timer currently runs    */
	spinlock_t		lock;
};

/* ----- timer callback --------------------------------------------------- */

static void gpio_pwm_timer_fn(struct timer_list *t)
{
	struct gpio_pwm_led *led = timer_container_of(led, t, timer);
	unsigned long flags;

	spin_lock_irqsave(&led->lock, flags);
	/*
	 * brightness_set() may run from hard-IRQ context. Keep the complete
	 * PWM state machine under one IRQ-safe lock so a rail transition
	 * cannot race this callback between its decision, GPIO write and
	 * rearm (audit LED-005/006).
	 */
	if (!led->pwm_active || led->threshold == 0 ||
	    led->threshold >= PWM_PERIOD_JIFFIES) {
		led->pwm_active = false;
		spin_unlock_irqrestore(&led->lock, flags);
		return;
	}

	gpiod_set_value(led->gpiod,
			led->counter < led->threshold ? 1 : 0);

	led->counter++;
	if (led->counter >= PWM_PERIOD_JIFFIES)
		led->counter = 0;

	/*
	 * Re-arm for the next tick. The timer wheel rounds every expiry up
	 * by one level granularity so a timer can never fire early
	 * (calc_index() in kernel/time/timer.c): "jiffies + 1" lands in the
	 * "jiffies + 2" bucket, doubling the PWM step to 8 ms and halving
	 * the PWM frequency to 31 Hz — visible flicker (issue #120).
	 * "jiffies" (expire ASAP) is bucketed at the next tick, giving a
	 * true 4 ms step; it cannot fire earlier than that by construction.
	 */
	mod_timer(&led->timer, jiffies);
	spin_unlock_irqrestore(&led->lock, flags);
}

/* ----- brightness_set --------------------------------------------------- */

static void gpio_pwm_brightness_set(struct led_classdev *cdev,
				    enum led_brightness value)
{
	struct gpio_pwm_led *led = container_of(cdev, struct gpio_pwm_led, cdev);
	unsigned int threshold;
	unsigned long flags;

	/* Quantize once here (audit LED-002): the 4-jiffy window maps
	 * 0-255 to 0..4 ON-jiffies per period. The 0/4 and 4/4 bands are
	 * steady GPIO levels — no reason to keep a 250 Hz timer alive to
	 * re-write a constant.
	 */
	threshold = (value * PWM_PERIOD_JIFFIES + MAX_BRIGHTNESS / 2) /
		    MAX_BRIGHTNESS;

	spin_lock_irqsave(&led->lock, flags);
	led->threshold = threshold;

	if (threshold == 0) {
		/*
		 * brightness_set() is an atomic callback. Mark inactive before
		 * the non-blocking delete; the callback observes that state
		 * under the same lock and cannot drive or rearm afterwards.
		 */
		led->pwm_active = false;
		timer_delete(&led->timer);
		gpiod_set_value(led->gpiod, 0);
	} else if (threshold >= PWM_PERIOD_JIFFIES) {
		/* Rounds to always-on -- stop PWM, force GPIO high. */
		led->pwm_active = false;
		timer_delete(&led->timer);
		gpiod_set_value(led->gpiod, 1);
	} else {
		/* Intermediate -- start PWM if not already running.
		 * Arm at "jiffies", never "jiffies + 1": the timer wheel
		 * rounds expiries up a bucket, so +1 lands at +2 and halves
		 * the PWM frequency (the #120 rule, audit LED-003).
		 */
		if (!led->pwm_active) {
			led->counter = 0;
			led->pwm_active = true;
			mod_timer(&led->timer, jiffies);
		}
		/*
		 * If already running the new duty cycle is picked up on the
		 * next timer callback via led->threshold -- no restart needed.
		 */
	}

	spin_unlock_irqrestore(&led->lock, flags);
}

/* ----- DT parsing & probe ----------------------------------------------- */

static void gpio_pwm_timer_shutdown(void *data)
{
	struct gpio_pwm_led *led = data;

	/*
	 * Registered before the devm LED class device, so teardown order is:
	 * class unregister (sets LED_OFF), synchronous timer shutdown, GPIO
	 * release. This is the only blocking timer operation in the driver.
	 */
	timer_shutdown_sync(&led->timer);
}

static int gpio_pwm_led_probe_child(struct device *dev,
				    struct device_node *np,
				     struct gpio_pwm_led *led)
{
	struct led_init_data init_data = {};
	enum gpiod_flags gflags = GPIOD_OUT_LOW;
	const char *state = NULL;
	const char *trigger;
	bool keep = false;
	int ret;

	/* Parse default-state before requesting the line: "keep" must
	 * request GPIOD_ASIS or the OUT_LOW request itself destroys the
	 * state we are supposed to keep (audit LED-001).
	 */
	if (!of_property_read_string(np, "default-state", &state) &&
	    !strcmp(state, "keep")) {
		keep = true;
		gflags = GPIOD_ASIS;
	}

	led->gpiod = devm_fwnode_gpiod_get(dev, of_fwnode_handle(np),
					   NULL, gflags, NULL);
	if (IS_ERR(led->gpiod))
		return PTR_ERR(led->gpiod);
	if (gpiod_cansleep(led->gpiod))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "%pOFn GPIO may sleep; software PWM requires MMIO GPIO\n",
				     np);

	spin_lock_init(&led->lock);

	timer_setup(&led->timer, gpio_pwm_timer_fn, 0);

	led->cdev.max_brightness = MAX_BRIGHTNESS;
	led->cdev.brightness_set = gpio_pwm_brightness_set;

	init_data.fwnode = of_fwnode_handle(np);

	/* Default state */
	if (keep) {
		int val = gpiod_get_value(led->gpiod);

		if (val < 0)
			return dev_err_probe(dev, val,
					     "failed to read kept state for %pOFn\n",
					     np);

		led->cdev.brightness = val ? MAX_BRIGHTNESS : 0;
		/*
		 * The ASIS request left the line untouched (possibly an
		 * input); make it an output at the level we just read.
		 */
		ret = gpiod_direction_output(led->gpiod, val);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to set %pOFn as output\n",
					     np);
	} else if (state && !strcmp(state, "on")) {
		led->cdev.brightness = led->cdev.max_brightness;
	}
	/* else "off" -> 0 (default) */

	/* Default trigger */
	if (!of_property_read_string(np, "linux,default-trigger", &trigger))
		led->cdev.default_trigger = trigger;

	/*
	 * Add this before classdev registration so devres releases classdev
	 * first, then waits out the PWM callback, then releases the GPIO.
	 */
	ret = devm_add_action_or_reset(dev, gpio_pwm_timer_shutdown, led);
	if (ret)
		return ret;

	ret = devm_led_classdev_register_ext(dev, &led->cdev, &init_data);
	if (ret)
		return ret;

	/* Apply initial brightness (may start PWM if intermediate) */
	gpio_pwm_brightness_set(&led->cdev, led->cdev.brightness);

	return 0;
}

static int gpio_pwm_leds_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np, *child;
	struct gpio_pwm_led *leds;
	int count, i, ret;

	np = dev_of_node(dev);
	if (!np)
		return -ENODEV;

	count = of_get_available_child_count(np);
	if (count == 0)
		return -ENODEV;

	leds = devm_kcalloc(dev, count, sizeof(*leds), GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	i = 0;
	for_each_available_child_of_node(np, child) {
		ret = gpio_pwm_led_probe_child(dev, child, &leds[i]);
		if (ret) {
			dev_err(dev, "failed to register LED %pOFn: %d\n",
				child, ret);
			of_node_put(child);
			return ret;
		}
		i++;
	}

	dev_info(dev, "v" DRV_VERSION " - %d LED(s) registered\n", count);

	return 0;
}

static const struct of_device_id gpio_pwm_leds_of_match[] = {
	{ .compatible = "gpio-leds-pwm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, gpio_pwm_leds_of_match);

static struct platform_driver gpio_pwm_leds_driver = {
	.probe	= gpio_pwm_leds_probe,
	.driver	= {
		.name		= "leds-gpio-pwm",
		.of_match_table	= gpio_pwm_leds_of_match,
	},
};

module_platform_driver(gpio_pwm_leds_driver);

MODULE_AUTHOR("Jacques Nilo");
MODULE_DESCRIPTION("GPIO LED driver with software PWM brightness control");
MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE("GPL");
