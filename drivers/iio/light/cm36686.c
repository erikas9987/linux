// SPDX-License-Identifier: GPL-2.0-only
/**
 * Copyright (C) 2026 Erikas Bitovtas <xerikasxx@gmail.com>
 *
 * Based on downstream driver created by Capella Microsystems and modified
 * by ASUS in drivers/misc/input/cm36283.c found in android_kernel_asus_msm8916
 * Based on downstream IIO driver created by Qian Wenfa of Xiaomi
 * in android_kernel_xiaomi_msm8992 in drivers/iio/light/cm36686.c
 * Based on previous mailing list submission for cm36672p by Kevin Tsai:
 * https://lore.kernel.org/linux-iio/1465462845-1571-1-git-send-email-capellamicro@gmail.com/
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/dev_printk.h>
#include <linux/device/devres.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>

#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/types.h>

/* Device registers */
#define CM36686_REG_ALS_CONF		0x00
#define CM36686_REG_PS_CONF1		0x03
#define CM36686_REG_PS_CONF3		0x04
#define CM36686_REG_PS_THDL		0x06
#define CM36686_REG_PS_THDH		0x07
#define CM36686_REG_PS_DATA		0x08
#define CM36686_REG_ALS_DATA		0x09
#define CM36686_REG_INT_FLAG		0x0B
#define CM36686_REG_ID_FLAG		0x0C

/* ALS_CONF */
#define CM36686_ALS_IT			GENMASK(7, 6)
#define CM36686_ALS_GAIN		GENMASK(3, 2)
#define CM36686_ALS_INT_EN		BIT(1)
#define CM36686_ALS_SD			BIT(0)

/* PS_CONF1 */
#define CM36686_PS_DR			GENMASK(7, 6)
#define CM36686_PS_PERS			GENMASK(5, 4)
#define CM36686_PS_IT			GENMASK(3, 1)
#define CM36686_PS_SD			BIT(0)

#define CM36686_PS_INT_OUT		BIT(9)
#define CM36686_PS_INT_IN		BIT(8)

/* PS_CONF3 */
#define CM36686_PS_SMART_PERS_ENABLE	BIT(4)

#define CM36686_LED_I			GENMASK(10, 8)

/* INT_FLAG */
#define CM36686_PS_IF			GENMASK(9, 8)

/* Default values */
#define CM36686_ALS_ENABLE		0x00
#define CM36686_PS_PERS_2		FIELD_PREP(CM36686_PS_PERS, 1)
#define CM36686_LED_I_50		0

/* Max proximity thresholds */
#define CM36686_MAX_PS_VALUE		(BIT(12) - 1)

#define CM36686_DEVICE_ID		0x86
#define CM36686_ALS_TRANS_RATIO		16

enum cm36686_distance {
	CM36686_AWAY = 1,
	CM36686_CLOSE,
	CM36686_BOTH,
};

enum {
	CM36686_PS_CONF1,
	CM36686_PS_CONF3,
	CM36686_PS_CONF_NUM,
};

static const int cm36686_als_it_times[][2] = {
	{ 0, 80000 },
	{ 0, 160000 },
	{ 0, 320000 },
	{ 0, 640000 },
};

static const int cm36686_ps_it_times[][2] = {
	{ 0, 80 },
	{ 0, 120 },
	{ 0, 160 },
	{ 0, 200 },
	{ 0, 240 },
	{ 0, 280 },
	{ 0, 320 },
	{ 0, 640 },
};

static const int cm36686_ps_led_current_mA[] = {
	50,
	75,
	100,
	120,
	140,
	160,
	180,
	200
};

struct cm36686_data {
	/* Mutex lock to prevent simultaneous reads/writes into registers */
	struct mutex lock;
	struct i2c_client *client;
	int als_conf;
	int ps_conf[CM36686_PS_CONF_NUM];
	int ps_close;
	int ps_away;
};

struct cm366xx_chip_info {
	const char *name;
	const struct iio_info *indio_info;
	const struct iio_info *indio_info_no_irq;
	const struct iio_chan_spec *channels;
	const int num_channels;
	const struct iio_chan_spec *channels_no_events;
	const int num_channels_no_events;
};

static int cm36686_current_to_index(int led_current)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cm36686_ps_led_current_mA); i++)
		if (led_current <= cm36686_ps_led_current_mA[i])
			return i;

	return -EINVAL;
}

static ssize_t cm36686_read_near_level(struct iio_dev *indio_dev,
				       uintptr_t priv,
				       const struct iio_chan_spec *chan,
				       char *buf)
{
	struct cm36686_data *chip = iio_priv(indio_dev);

	return sysfs_emit(buf, "%u\n", chip->ps_close);
}

static const struct iio_chan_spec_ext_info cm36686_ext_info[] = {
	{
		.name = "nearlevel",
		.shared = IIO_SEPARATE,
		.read = cm36686_read_near_level,
	},
	{ }
};

static const struct iio_event_spec cm36686_proximity_event_spec[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	},
};

static const struct iio_chan_spec cm36686_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_INT_TIME) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_INT_TIME),
		.address = CM36686_REG_ALS_DATA,
	},
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_INT_TIME),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_INT_TIME),
		.address = CM36686_REG_PS_DATA,
		.event_spec = cm36686_proximity_event_spec,
		.num_event_specs = ARRAY_SIZE(cm36686_proximity_event_spec),
		.ext_info = cm36686_ext_info,
	},
};

static const struct iio_chan_spec cm36686_channels_no_events[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_INT_TIME) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_INT_TIME),
		.address = CM36686_REG_ALS_DATA,
	},
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_INT_TIME),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_INT_TIME),
		.address = CM36686_REG_PS_DATA,
		.ext_info = cm36686_ext_info,
	},
};

static const struct iio_chan_spec cm36672p_channels[] = {
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_INT_TIME),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_INT_TIME),
		.address = CM36686_REG_PS_DATA,
		.event_spec = cm36686_proximity_event_spec,
		.num_event_specs = ARRAY_SIZE(cm36686_proximity_event_spec),
		.ext_info = cm36686_ext_info,
	},
};

static const struct iio_chan_spec cm36672p_channels_no_events[] = {
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_INT_TIME),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_INT_TIME),
		.address = CM36686_REG_PS_DATA,
		.ext_info = cm36686_ext_info,
	},
};

static int cm36686_read_avail(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      const int **vals, int *type, int *length,
			      long mask)
{
	if (mask != IIO_CHAN_INFO_INT_TIME)
		return -EINVAL;

	switch (chan->type) {
	case IIO_LIGHT:
		*vals = (int *)(cm36686_als_it_times);
		*length = 2 * ARRAY_SIZE(cm36686_als_it_times);
		*type = IIO_VAL_INT_PLUS_MICRO;
		return IIO_AVAIL_LIST;
	case IIO_PROXIMITY:
		*vals = (int *)(cm36686_ps_it_times);
		*length = 2 * ARRAY_SIZE(cm36686_ps_it_times);
		*type = IIO_VAL_INT_PLUS_MICRO;
		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static int cm36686_read_channel(struct cm36686_data *chip,
				struct iio_chan_spec const *chan, int *val)
{
	struct i2c_client *client = chip->client;
	int data = i2c_smbus_read_word_data(client, chan->address);

	if (data < 0)
		return -EIO;

	*val = data;
	return IIO_VAL_INT;
}

static int cm36686_update_conf(struct cm36686_data *chip, int address,
				int *conf, int val)
{
	struct i2c_client *client = chip->client;
	int ret = i2c_smbus_write_word_data(client, address, val);

	if (!ret)
		*conf = val;

	return ret;
}

static int cm36686_update_als_conf(struct cm36686_data *chip, int val)
{
	return cm36686_update_conf(chip, CM36686_REG_ALS_CONF, &chip->als_conf,
			    val);
}

static int cm36686_update_ps_conf(struct cm36686_data *chip, int val)
{
	return cm36686_update_conf(chip, CM36686_REG_PS_CONF1,
			    &chip->ps_conf[CM36686_PS_CONF1], val);
}

/**
 * This is taken from Xiaomi's driver for cm36686. The device tree for cm36686
 * in android_kernel_xiaomi_msm8992 includes a property called
 * "als_trans_ratio". We don't know what this property is, but we know it is
 * set to 16, and integration time in downstream is set to 160ms. Using this
 * "als_trans_ratio" property, Xiaomi calculates scale like this:
 * scale = als_trans_ratio * 40,000
 * val = scale / 1,000,000
 * val2 = scale % 1,000,000
 * In our driver, however, integration time can be adjusted, so if it changes
 * during runtime, the scale will be incorrect and lux value will be reported
 * double or half what it actually is, depending on whether we increase or
 * decrease integration time. In order to preserve the proportion by which the
 * scale is calculated, we multiply "als_trans_ratio" (16) by 160ms and then
 * divide it by our current integration time. This gives us new
 * "als_trans_ratio" by which the scale will be calculated.
 * Unfortunately, since the datasheet for this sensor is unavailable, this
 * guess is the best we have at the moment.
 */
static int cm36686_read_scale(struct cm36686_data *chip,
			      struct iio_chan_spec const *chan, int *val,
			      int *val2)
{
	if (chan->type != IIO_LIGHT)
		return -EINVAL;

	int als_index = FIELD_GET(CM36686_ALS_IT, chip->als_conf);
	int als_it = cm36686_als_it_times[als_index][1];
	int scale = (256 / als_it) * 4;

	*val = scale / 1000000;
	*val2 = scale % 1000000;

	return IIO_VAL_INT_PLUS_MICRO;
}

static int cm36686_read_int_time(struct cm36686_data *chip,
				 struct iio_chan_spec const *chan, int *val,
				 int *val2)
{
	int als_it_index, ps_it_index;

	switch (chan->type) {
	case IIO_LIGHT:
		als_it_index = FIELD_GET(CM36686_ALS_IT, chip->als_conf);
		*val = cm36686_als_it_times[als_it_index][0];
		*val2 = cm36686_als_it_times[als_it_index][1];
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_PROXIMITY:
		ps_it_index = FIELD_GET(CM36686_PS_IT,
			chip->ps_conf[CM36686_PS_CONF1]);
		*val = cm36686_ps_it_times[ps_it_index][0];
		*val2 = cm36686_ps_it_times[ps_it_index][1];
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int cm36686_write_light_int_time(struct cm36686_data *chip, int val2)
{
	int index = -1, ret, new_it_time = chip->als_conf;

	for (int i = 0; i < ARRAY_SIZE(cm36686_als_it_times); i++) {
		if (cm36686_als_it_times[i][1] == val2) {
			index = i;
			break;
		}
	}

	if (index == -1)
		return -EINVAL;

	FIELD_MODIFY(CM36686_ALS_IT, &new_it_time, index);

	ret = cm36686_update_als_conf(chip, new_it_time);

	return ret;
}

static int cm36686_write_prox_int_time(struct cm36686_data *chip, int val2)
{
	int index = -1, ret, new_it_time = chip->ps_conf[CM36686_PS_CONF1];

	for (int i = 0; i < ARRAY_SIZE(cm36686_ps_it_times); i++) {
		if (cm36686_ps_it_times[i][1] == val2) {
			index = i;
			break;
		}
	}

	if (index == -1)
		return -EINVAL;

	FIELD_MODIFY(CM36686_PS_IT, &new_it_time, index);

	ret = cm36686_update_ps_conf(chip, new_it_time);

	return ret;
}

static int cm36686_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	struct cm36686_data *chip = iio_priv(indio_dev);

	guard(mutex)(&chip->lock);
	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		return cm36686_read_channel(chip, chan, val);
	case IIO_CHAN_INFO_SCALE:
		return cm36686_read_scale(chip, chan, val, val2);
	case IIO_CHAN_INFO_INT_TIME:
		return cm36686_read_int_time(chip, chan, val, val2);
	default:
		return -EINVAL;
	}
}

static int cm36686_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, int val,
			     int val2, long mask)
{
	struct cm36686_data *chip = iio_priv(indio_dev);

	if (val) /* Integration time more than 1s is not supported */
		return -EINVAL;

	if (mask != IIO_CHAN_INFO_INT_TIME)
		return -EINVAL;

	guard(mutex)(&chip->lock);
	switch (chan->type) {
	case IIO_LIGHT:
		return cm36686_write_light_int_time(chip, val2);
	case IIO_PROXIMITY:
		return cm36686_write_prox_int_time(chip, val2);
	default:
		return -EINVAL;
	}
}

static int cm36686_read_prox_thresh(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info, int *val,
				    int *val2)
{
	struct cm36686_data *chip = iio_priv(indio_dev);

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	guard(mutex)(&chip->lock);
	switch (dir) {
	case IIO_EV_DIR_RISING:
		*val = chip->ps_close;
		break;
	case IIO_EV_DIR_FALLING:
		*val = chip->ps_away;
		break;
	default:
		return -EINVAL;
	}

	return IIO_VAL_INT;
}

static int cm36686_write_prox_thresh(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir,
				     enum iio_event_info info, int val,
				     int val2)
{
	struct cm36686_data *chip = iio_priv(indio_dev);
	int ret, address, *thresh;

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (dir) {
	case IIO_EV_DIR_FALLING:
		if (val > chip->ps_close || val < 0)
			return -EINVAL;

		address = CM36686_REG_PS_THDL;
		thresh = &chip->ps_away;
		break;
	case IIO_EV_DIR_RISING:
		if (val < chip->ps_away || val > CM36686_MAX_PS_VALUE)
			return -EINVAL;

		address = CM36686_REG_PS_THDH;
		thresh = &chip->ps_close;
		break;
	default:
		return -EINVAL;
	}

	guard(mutex)(&chip->lock);
	ret = cm36686_update_conf(chip, address, thresh, val);

	return ret;
}

static int cm36686_read_prox_event_config(struct iio_dev *indio_dev,
					  const struct iio_chan_spec *chan,
					  enum iio_event_type type,
					  enum iio_event_direction dir)
{
	struct cm36686_data *chip = iio_priv(indio_dev);

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	guard(mutex)(&chip->lock);
	switch (dir) {
	case IIO_EV_DIR_FALLING:
		return FIELD_GET(CM36686_PS_INT_OUT,
				chip->ps_conf[CM36686_PS_CONF1]);
	case IIO_EV_DIR_RISING:
		return FIELD_GET(CM36686_PS_INT_IN,
				chip->ps_conf[CM36686_PS_CONF1]);
	default:
		return -EINVAL;
	}
}

static int cm36686_write_prox_event_config(struct iio_dev *indio_dev,
					   const struct iio_chan_spec *chan,
					   enum iio_event_type type,
					   enum iio_event_direction dir,
					   bool state)
{
	struct cm36686_data *chip = iio_priv(indio_dev);
	int ret, new_ps_conf = chip->ps_conf[CM36686_PS_CONF1];

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (dir) {
	case IIO_EV_DIR_FALLING:
		FIELD_MODIFY(CM36686_PS_INT_OUT, &new_ps_conf, state);
		break;
	case IIO_EV_DIR_RISING:
		FIELD_MODIFY(CM36686_PS_INT_IN, &new_ps_conf, state);
		break;
	default:
		return -EINVAL;
	}

	guard(mutex)(&chip->lock);
	ret = cm36686_update_ps_conf(chip, new_ps_conf);

	return ret;
}

static int cm36686_fallback_read_ps(struct iio_dev *indio_dev)
{
	struct cm36686_data *chip = iio_priv(indio_dev);
	struct i2c_client *client = chip->client;
	int data = i2c_smbus_read_word_data(client, CM36686_REG_PS_DATA);

	if (data < 0)
		return data;

	if (data < chip->ps_away)
		return IIO_EV_DIR_FALLING;
	else if (data > chip->ps_close)
		return IIO_EV_DIR_RISING;
	return IIO_EV_DIR_EITHER;
}

static irqreturn_t cm36686_irq_handler(int irq, void *data)
{
	struct iio_dev *indio_dev = data;
	struct cm36686_data *chip = iio_priv(indio_dev);
	struct i2c_client *client = chip->client;
	int ev_dir, ret;
	u64 ev_code;

	/* Reading the interrupt flag acknowledges the interrupt */
	ret = i2c_smbus_read_word_data(client, CM36686_REG_INT_FLAG);
	if (ret < 0) {
		dev_err_ratelimited(&client->dev,
			"Interrupt flag register read failed: %pe",
			ERR_PTR(ret));
		return IRQ_HANDLED;
	}

	ret = FIELD_GET(CM36686_PS_IF, ret);
	switch (ret) {
	case CM36686_CLOSE:
		ev_dir = IIO_EV_DIR_RISING;
		break;
	case CM36686_AWAY:
		ev_dir = IIO_EV_DIR_FALLING;
		break;
	case CM36686_BOTH:
		ev_dir = cm36686_fallback_read_ps(indio_dev);
		if (ev_dir < 0) {
			dev_err_ratelimited(&client->dev,
				"Failed to settle interrupt state: %pe",
				ERR_PTR(ret));
			return IRQ_HANDLED;
		}
		break;
	default:
		dev_err_ratelimited(&client->dev,
		      "Unknown interrupt state: %x", ret);
		return IRQ_HANDLED;
	}
	ev_code = IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, IIO_EV_INFO_VALUE,
				       IIO_EV_TYPE_THRESH, ev_dir);

	iio_push_event(indio_dev, ev_code, iio_get_time_ns(indio_dev));
	return IRQ_HANDLED;
}

static const struct iio_info cm36686_info = {
	.read_avail =		cm36686_read_avail,
	.read_raw =		cm36686_read_raw,
	.write_raw =		cm36686_write_raw,
	.read_event_value =	cm36686_read_prox_thresh,
	.write_event_value =	cm36686_write_prox_thresh,
	.read_event_config =	cm36686_read_prox_event_config,
	.write_event_config =	cm36686_write_prox_event_config,
};

static const struct iio_info cm36686_info_no_irq = {
	.read_avail =		cm36686_read_avail,
	.read_raw =		cm36686_read_raw,
	.write_raw =		cm36686_write_raw,
};

static const struct cm366xx_chip_info cm36686_chip_info = {
	.name = "cm36686",
	.indio_info = &cm36686_info,
	.indio_info_no_irq = &cm36686_info_no_irq,
	.channels = cm36686_channels,
	.num_channels = ARRAY_SIZE(cm36686_channels),
	.channels_no_events = cm36686_channels_no_events,
	.num_channels_no_events = ARRAY_SIZE(cm36686_channels_no_events),
};

static const struct cm366xx_chip_info cm36672p_chip_info = {
	.name = "cm36672p",
	.indio_info = &cm36686_info,
	.indio_info_no_irq = &cm36686_info_no_irq,
	.channels = cm36672p_channels,
	.num_channels = ARRAY_SIZE(cm36672p_channels),
	.channels_no_events = cm36672p_channels_no_events,
	.num_channels_no_events = ARRAY_SIZE(cm36672p_channels_no_events),
};

static int cm36686_setup(struct cm36686_data *chip, struct iio_dev *indio_dev)
{
	struct i2c_client *client = chip->client;
	const struct cm366xx_chip_info *info = i2c_get_match_data(client);
	int ret, led_current, led_index = CM36686_LED_I_50;

	chip->als_conf = CM36686_ALS_ENABLE;

	ret = i2c_smbus_write_word_data(client, CM36686_REG_ALS_CONF,
					chip->als_conf);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
			"Failed to enable ambient light sensor");

	/* Set proximity sensor persistence to two to prevent
	 * false triggering of the interrupt when probing the
	 * driver.
	 */
	chip->ps_conf[CM36686_PS_CONF1] = CM36686_PS_PERS_2;

	if (client->irq)
		chip->ps_conf[CM36686_PS_CONF1] |= CM36686_PS_INT_IN |
			CM36686_PS_INT_OUT;

	ret = i2c_smbus_write_word_data(client, CM36686_REG_PS_CONF1,
					chip->ps_conf[CM36686_PS_CONF1]);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
		       "Failed to enable proximity sensor");

	chip->ps_conf[CM36686_PS_CONF3] = CM36686_PS_SMART_PERS_ENABLE;

	ret = device_property_read_u32(&client->dev,
		"capella,proximity-led-current-milliamp", &led_current);
	if (!ret) {
		led_index = cm36686_current_to_index(led_current);
		if (led_index < 0)
			return dev_err_probe(&client->dev, led_index,
				"Failed to find appropriate IR LED current.");
	} else if (ret != -EINVAL) {
		return dev_err_probe(&client->dev, ret,
		       "Failed to read IR LED current.");
	}

	FIELD_MODIFY(CM36686_LED_I, &chip->ps_conf[CM36686_PS_CONF3],
			led_index);

	ret = i2c_smbus_write_word_data(client, CM36686_REG_PS_CONF3,
					chip->ps_conf[CM36686_PS_CONF3]);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
			"Failed to enable proximity sensor");

	if (device_property_read_u32(&client->dev, "proximity-near-level",
			      &chip->ps_close))
		chip->ps_close = 0;
	chip->ps_away = chip->ps_close;

	indio_dev->name = info->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	if (client->irq) {
		indio_dev->info = info->indio_info;
		indio_dev->channels = info->channels;
		indio_dev->num_channels = info->num_channels;

		ret = i2c_smbus_write_word_data(client, CM36686_REG_PS_THDH,
						chip->ps_close);
		if (ret < 0)
			return dev_err_probe(&client->dev, ret,
				"Failed to set close proximity threshold");

		ret = i2c_smbus_write_word_data(client, CM36686_REG_PS_THDL,
						chip->ps_away);
		if (ret < 0)
			return dev_err_probe(&client->dev, ret,
			"Failed to set away proximity threshold");

		ret = devm_request_threaded_irq(&client->dev, client->irq,
						NULL, cm36686_irq_handler,
						IRQF_TRIGGER_FALLING |
						IRQF_ONESHOT,
						indio_dev->name, indio_dev);
		if (ret < 0)
			return dev_err_probe(&client->dev, ret,
			       "Failed to request irq");
	} else {
		indio_dev->info = info->indio_info_no_irq;
		indio_dev->channels = info->channels_no_events;
		indio_dev->num_channels = info->num_channels_no_events;
	}

	return 0;
}

static void cm36686_shutdown(void *data)
{
	struct cm36686_data *chip = data;
	struct i2c_client *client = chip->client;
	int ret, als_shutdown, ps_shutdown;

	als_shutdown = chip->als_conf | CM36686_ALS_SD;

	ret = i2c_smbus_write_word_data(client, CM36686_REG_ALS_CONF,
					als_shutdown);
	if (ret < 0)
		dev_err(&client->dev, "Failed to shutdown ALS");

	ps_shutdown = chip->ps_conf[CM36686_PS_CONF1] | CM36686_PS_SD;

	ret = i2c_smbus_write_word_data(client, CM36686_REG_PS_CONF1,
					ps_shutdown);
	if (ret < 0)
		dev_err(&client->dev, "Failed to shutdown PS");
}

static int cm36686_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct cm36686_data *chip;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev,
					  sizeof(struct cm36686_data));
	if (!indio_dev)
		return -ENOMEM;

	chip = iio_priv(indio_dev);

	ret = i2c_smbus_read_byte_data(client, CM36686_REG_ID_FLAG);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret, "Failed to read device ID");

	if (ret != CM36686_DEVICE_ID)
		dev_warn(&client->dev, "Device ID: %02x, expected: %02x",
			ret, CM36686_DEVICE_ID);

	i2c_set_clientdata(client, &indio_dev);
	chip->client = client;

	ret = devm_mutex_init(&client->dev, &chip->lock);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
		       "Failed to initialize mutex");

	static const char * const regulator_names[] = { "vdd", "vddio", "vled" };

	ret = devm_regulator_bulk_get_enable(&client->dev,
		ARRAY_SIZE(regulator_names), regulator_names);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
				     "Failed to enable regulators");

	ret = cm36686_setup(chip, indio_dev);
	if (ret < 0)
		return ret;

	ret = devm_add_action_or_reset(&client->dev, cm36686_shutdown, chip);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to set shutdown action");

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct i2c_device_id cm36686_id[] = {
	{ "cm36686", (kernel_ulong_t)(&cm36686_chip_info) },
	{ "cm36672p", (kernel_ulong_t)(&cm36672p_chip_info) },
	{ }
};

MODULE_DEVICE_TABLE(i2c, cm36686_id);

static const struct of_device_id cm36686_of_match[] = {
	{ .compatible = "capella,cm36686", .data = &cm36686_chip_info },
	{ .compatible = "capella,cm36672p", .data = &cm36672p_chip_info },
	{ }
};
MODULE_DEVICE_TABLE(of, cm36686_of_match);

static struct i2c_driver cm36686_driver = {
	.driver = {
		.name = "cm36686",
		.of_match_table = cm36686_of_match,
	},
	.probe = cm36686_probe,
	.id_table = cm36686_id,
};
module_i2c_driver(cm36686_driver);

MODULE_AUTHOR("Erikas Bitovtas <xerikasxx@gmail.com>");
MODULE_DESCRIPTION("CM36686 ambient light and proximity sensor driver");
MODULE_LICENSE("GPL");
