// SPDX-License-Identifier: GPL-2.0-only

#include "asm-generic/errno-base.h"
#include "linux/array_size.h"
#include "linux/bitfield.h"
#include "linux/dev_printk.h"
#include "linux/device/devres.h"
#include "linux/iio/types.h"
#include "linux/mod_devicetable.h"
#include "linux/property.h"
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/events.h>
#include <linux/interrupt.h>

/* Device registers */
#define CM36283_REG_ALS_CONF		0x00
#define CM36283_REG_PS_CONF1		0x03
#define CM36283_REG_PS_CONF3		0x04
#define CM36283_REG_PS_THD		0x06
#define CM36686_REG_PS_THDL		0x06
#define CM36686_REG_PS_THDH		0x07
#define CM36283_REG_PS_DATA		0x08
#define CM36283_REG_ALS_DATA	 	0x09
#define CM36283_REG_INT_FLAG		0x0B
#define CM36283_REG_ID_FLAG		0x0C

/* ALS_CONF */
#define CM36283_ALS_IT			GENMASK(7, 6)
#define CM36283_ALS_GAIN		GENMASK(3, 2)
#define CM36283_ALS_INT_EN		BIT(1)
#define CM36283_ALS_SD			BIT(0)

/* PS_CONF1 bitfields for cm36283 */
#define CM36283_PS_DR			GENMASK(7, 6)
#define CM36283_PS_IT			GENMASK(5, 4)
#define CM36283_PS_PERS			GENMASK(3, 2)
#define CM36283_PS_RES_1		BIT(1)
#define CM36283_PS_SD			BIT(0)

#define CM36283_PS_INT_IN		BIT(8)
#define CM36283_PS_INT_OUT		BIT(9)

/* PS_CONF1 bitfields for cm36686 */
#define CM36686_PS_DR			GENMASK(7, 6)
#define CM36686_PS_PERS			GENMASK(5, 4)
#define CM36686_PS_IT			GENMASK(3, 1)
#define CM36686_PS_SD			BIT(0)

#define CM36686_PS_INT_IN		BIT(9)
#define CM36686_PS_INT_OUT		BIT(8)

#define CM36283_PS_ITB			GENMASK(15, 14)

/* PS_CONF3 bitfields for cm36283 */
#define CM36283_PS_MS			BIT(14)
#define CM36283_PS_PROL			GENMASK(13, 12)
#define CM36283_PS_SMART_PERS_ENABLE	BIT(4)
#define CM36283_PS_ACTIVE_FORCE_MODE	BIT(3)
#define CM36283_PS_ACTIVE_FORCE_TRIG	BIT(2)

/* PS_CONF3 bitfields for cm36686 */
#define CM36686_PS_SMART_PERS_ENABLE	BIT(4)

#define CM36686_LED_I			GENMASK(11, 9)

/* INT_FLAG */
#define CM36283_PS_IF			GENMASK(9, 8)

/* Default values */
#define CM36283_ALS_ENABLE 		0x00
#define CM36283_PS_DR_1_320 		FIELD_PREP(CM36283_PS_DR, 3)
#define CM36283_PS_IT_1_3T 		FIELD_PREP(CM36283_PS_IT, 1)
#define CM36283_PS_PERS_2 		FIELD_PREP(CM36283_PS_PERS, 1)

#define CM36686_PS_DR_1_320 		FIELD_PREP(CM36283_PS_DR, 3)
#define CM36686_PS_PERS_2		FIELD_PREP(CM36686_PS_PERS, 1)
#define CM36686_PS_IT_2_5T		FIELD_PREP(CM36686_PS_IT, 3)
#define CM36686_LED_I_100		FIELD_PREP(CM36686_LED_I, 2)

/* Shifts */
#define CM36283_INT_FLAG_SHIFT		8
#define CM36283_PS_THDH_SHIFT		8

/* Max proximity thresholds */
#define CM36283_MAX_PS_VALUE		BIT(8) - 1
#define CM36686_MAX_PS_VALUE		BIT(12) - 1

enum cm36283_model {
	CM36283 = 0x83,
	CM36686 = 0x86
};

enum cm36283_distance {
	CM36283_AWAY = 1,
	CM36283_CLOSE,
	CM36283_BOTH
};

enum {
	CM36283_PS_CONF1,
	CM36283_PS_CONF3,
	CM36283_PS_CONF_NUM
};

enum {
	CM36283_SUPPLY_VDD,
	CM36283_SUPPLY_VIO,
	CM36283_SUPPLY_COUNT,
};

static const int cm36283_als_it_times[] = {
	0, 80000, 
	0, 160000,
	0, 320000,
	0, 640000 
};

static const int cm36283_ps_it_times[] = {
	0, 320,
	0, 420,
	0, 520,
	0, 640
};

static const int cm36686_ps_it_times[] = {
	0, 320,
	0, 480, 
	0, 640, 
	0, 800,
	0, 960,
	0, 1120,
	0, 1280,
	0, 2560
};

struct cm36283_data {
	struct mutex lock;
	struct i2c_client *client;
	struct regulator_bulk_data supplies[CM36283_SUPPLY_COUNT];
	enum cm36283_model model;
	const int *als_it_times;
	const int *ps_it_times;
	int max_ps_value;
	int num_als_it;
	int num_ps_it;
	int ps_close;
	int ps_away;
	int als_conf;
	int ps_conf[CM36283_PS_CONF_NUM];
};

static const struct iio_event_spec cm36283_proximity_event_spec[] = {
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
	}
};

static const struct iio_chan_spec cm36283_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_INT_TIME),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_INT_TIME),
		.address = CM36283_REG_ALS_DATA,
	},
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_INT_TIME),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_INT_TIME),
		.address = CM36283_REG_PS_DATA,
		.event_spec = cm36283_proximity_event_spec,
		.num_event_specs = ARRAY_SIZE(cm36283_proximity_event_spec),
	}
};

static int cm36283_read_avail(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      const int **vals, int *type, int *length,
			      long mask)
{
	struct cm36283_data *chip = iio_priv(indio_dev);
	if (mask == IIO_CHAN_INFO_INT_TIME) {
		switch (chan->type) {
		case IIO_LIGHT:
			*vals = chip->als_it_times;
			*length = chip->num_als_it;
			*type = IIO_VAL_INT_PLUS_MICRO;
			return IIO_AVAIL_LIST;
		case IIO_PROXIMITY:
			*vals = chip->ps_it_times;
			*length = chip->num_ps_it;
			*type = IIO_VAL_INT_PLUS_MICRO;
			return IIO_AVAIL_LIST;
		default:
			return -EINVAL;
		}
	} else {
		return -EINVAL;
	}
}

static int cm36283_read_channel(struct cm36283_data *chip,
				struct iio_chan_spec const *chan, int *val,
				int *val2)
{
	struct i2c_client *client = chip->client;
	int ret = IIO_VAL_INT;

	int data = i2c_smbus_read_word_data(client, chan->address);	
	if (data < 0) {
		dev_err(&client->dev, "Failed to read register: %d\n", data);
		ret = -EIO;
	} else {
		*val = data;
	}
	return ret;
}

static int cm36283_read_int_time(struct cm36283_data *chip,
				 struct iio_chan_spec const *chan, int *val,
				 int *val2)
{
	int als_it_index, ps_it_index;

	switch (chan->type) {
	case IIO_LIGHT:
		als_it_index = FIELD_GET(CM36283_ALS_IT, chip->als_conf);
		*val = chip->als_it_times[2 * als_it_index + 0];
		*val2 = chip->als_it_times[2 * als_it_index + 1];
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_PROXIMITY:
		switch (chip->model) {
		case CM36283:
			ps_it_index = FIELD_GET(CM36283_PS_IT, chip->ps_conf[CM36283_PS_CONF1]);
			break;
		case CM36686:
			ps_it_index = FIELD_GET(CM36686_PS_IT, chip->ps_conf[CM36283_PS_CONF1]);
			break;
		default:
			return -EINVAL;
		}
		*val = chip->ps_it_times[2 * ps_it_index + 0];
		*val2 = chip->ps_it_times[2 * ps_it_index + 1];
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int cm36283_write_light_int_time(struct cm36283_data *chip, int val2)
{
	struct i2c_client *client = chip->client;
	int index = -1, ret;
	for (int i = 0; i < chip->num_als_it / 2; i++) {
		if (chip->als_it_times[2 * i + 1] == val2) {
			index = i;
			break;
		}
	}

	if (index == -1)
		return -EINVAL;

	int new_int_time = chip->als_conf & ~CM36283_ALS_IT;
	new_int_time |= FIELD_PREP(CM36283_ALS_IT, index);

	ret = i2c_smbus_write_word_data(chip->client, CM36283_REG_ALS_CONF,
					new_int_time);
	if (ret < 0)
		dev_err(&client->dev,
			"Failed to set ALS integration time: %d\n", ret);
	else
		chip->als_conf = new_int_time;

	return ret;
}

static int cm36283_write_prox_int_time(struct cm36283_data *chip, int val2)
{
	struct i2c_client *client = chip->client;
	int index = -1, ret, new_int_time;
	for (int i = 0; i < chip->num_ps_it / 2; i++) {
		if (chip->ps_it_times[2 * i + 1] == val2) {
			index = i;
			break;
		}
	}

	if (index == -1)
		return -EINVAL;

	switch (chip->model) {
	case CM36283:
		new_int_time = chip->ps_conf[CM36283_PS_CONF1] & ~CM36283_PS_IT;
		new_int_time |= FIELD_PREP(CM36283_PS_IT, index);
		break;
	case CM36686:
		new_int_time = chip->ps_conf[CM36283_PS_CONF1] & ~CM36686_PS_IT;
		new_int_time |= FIELD_PREP(CM36686_PS_IT, index);
		break;
	default:
		return -EINVAL;
	}

	ret = i2c_smbus_write_word_data(chip->client, CM36283_REG_PS_CONF1,
					new_int_time);
	if (ret < 0) 
		dev_err(&client->dev, "Failed to set PS integration time: %d\n",
			ret);
	else
		chip->ps_conf[CM36283_PS_CONF1] = new_int_time;

	return ret;
}

static int cm36283_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	struct cm36283_data *chip = iio_priv(indio_dev);
	int ret;

	mutex_lock(&chip->lock);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = cm36283_read_channel(chip, chan, val, val2);
		break;
	case IIO_CHAN_INFO_INT_TIME:
		ret = cm36283_read_int_time(chip, chan, val, val2);
		break;
	default:
		return -EINVAL;
	}

	mutex_unlock(&chip->lock);
	return ret;
}

static int cm36283_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, int val,
			     int val2, long mask)
{
	struct cm36283_data *chip = iio_priv(indio_dev);
	int ret;

	if (val)
		return -EINVAL;

	if (mask != IIO_CHAN_INFO_INT_TIME)
		return -EINVAL;

	mutex_lock(&chip->lock);

	switch (chan->type) {
	case IIO_LIGHT:
		ret = cm36283_write_light_int_time(chip, val2);
		break;
	case IIO_PROXIMITY:
		ret = cm36283_write_prox_int_time(chip, val2);
		break;
	default:
		ret = -EINVAL;
	}

	mutex_unlock(&chip->lock);
	return ret;
}

static int cm36283_set_prox_thresh(struct cm36283_data *chip)
{
	struct i2c_client *client = chip->client;

	int ps_data, ret;
	switch (chip->model) {
	case CM36283:
		ps_data = chip->ps_close << CM36283_PS_THDH_SHIFT |
			  chip->ps_away;

		ret = i2c_smbus_write_word_data(client, CM36283_REG_PS_THD,
						ps_data);
		if (ret < 0) {
			dev_err(&client->dev,
				"Failed to set PS threshold: %d\n", ret);
			return ret;
		}

		break;
	case CM36686:
		ret = i2c_smbus_write_word_data(client, CM36686_REG_PS_THDL,
						chip->ps_away);
		if (ret < 0) {
			dev_err(&client->dev,
				"Failed to set PS away threshold: %d\n", ret);
			return ret;
		}

		ret = i2c_smbus_write_word_data(client, CM36686_REG_PS_THDH,
						chip->ps_close);
		if (ret < 0) {
			dev_err(&client->dev,
				"Failed to set PS close threshold: %d\n", ret);
			return ret;
		}

		break;
	}
	return 0;
}

static int cm36283_read_prox_thresh(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info, int *val,
				    int *val2)
{
	struct cm36283_data *chip = iio_priv(indio_dev);

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

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

static int cm36283_write_prox_thresh(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir,
				     enum iio_event_info info, int val,
				     int val2)
{
	struct cm36283_data *chip = iio_priv(indio_dev);
	struct i2c_client *client = chip->client;
	int ret = 0, old_value, *old_thresh;

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (dir) {
	case IIO_EV_DIR_FALLING:
		if (val > chip->ps_close || val < 0)
			return -EINVAL;
		old_value = chip->ps_away;
		chip->ps_away = val;
		old_thresh = &chip->ps_away;
		break;
	case IIO_EV_DIR_RISING:
		if (val < chip->ps_away || val > chip->max_ps_value)
			return -EINVAL;
		old_value = chip->ps_close;
		chip->ps_close = val;
		old_thresh = &chip->ps_close;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&chip->lock);

	ret = cm36283_set_prox_thresh(chip);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to set PS threshold: %d\n", ret);
		*old_thresh = old_value;
	}

	mutex_unlock(&chip->lock);

	return ret;
}

static int cm36283_read_prox_event_config(struct iio_dev *indio_dev,
					  const struct iio_chan_spec *chan,
					  enum iio_event_type type,
					  enum iio_event_direction dir)
{
	struct cm36283_data *chip = iio_priv(indio_dev);

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (dir) {
	case IIO_EV_DIR_FALLING:
		return FIELD_GET(CM36283_PS_INT_OUT, chip->ps_conf[CM36283_PS_CONF1]);
	case IIO_EV_DIR_RISING:
		return FIELD_GET(CM36283_PS_INT_IN, chip->ps_conf[CM36283_PS_CONF1]);
	default:
		return -EINVAL;
	}
}

static int cm36283_write_prox_event_config(struct iio_dev *indio_dev,
					   const struct iio_chan_spec *chan,
					   enum iio_event_type type,
					   enum iio_event_direction dir,
					   bool state)
{
	struct cm36283_data *chip = iio_priv(indio_dev);
	struct i2c_client *client = chip->client;
	int ret = 0;

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	int new_ps_conf;
	switch (dir) {
	case IIO_EV_DIR_FALLING:
		new_ps_conf = chip->ps_conf[CM36283_PS_CONF1] &
			      ~CM36283_PS_INT_OUT;
		new_ps_conf |= FIELD_PREP(CM36283_PS_INT_OUT, state);
		break;
	case IIO_EV_DIR_RISING:
		new_ps_conf = chip->ps_conf[CM36283_PS_CONF1] &
			      ~CM36283_PS_INT_IN;
		new_ps_conf |= FIELD_PREP(CM36283_PS_INT_IN, state);
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&chip->lock);

	ret = i2c_smbus_write_word_data(chip->client, CM36283_REG_PS_CONF1, new_ps_conf);
	if (ret < 0) 
		dev_err(&client->dev,
			"Failed to set proximity event interrupt config: %d\n", ret);
	else 
		chip->ps_conf[CM36283_PS_CONF1] = new_ps_conf;

	mutex_unlock(&chip->lock);

	return ret;
}

static int cm36283_fallback_read_ps(struct iio_dev *indio_dev)
{
	struct cm36283_data *chip = iio_priv(indio_dev);
	struct i2c_client *client = chip->client;
	int data = i2c_smbus_read_word_data(client, CM36283_REG_PS_DATA);

	if (data < 0)
		return data;

	if (data < chip->ps_away) {
		return IIO_EV_DIR_FALLING;
	} else if (data > chip->ps_close) {
		return IIO_EV_DIR_RISING;
	} else {
		return IIO_EV_DIR_NONE;
	}
}

// Reading the interrupt flag acknowledges the interrupt
static irqreturn_t cm36283_irq_handler(int irq, void *data)
{
	struct iio_dev *indio_dev = data;
	struct cm36283_data *chip = iio_priv(indio_dev);
	struct i2c_client *client = chip->client;
	int ev_dir, ret;
	u64 ev_code;
	ret = i2c_smbus_read_word_data(client, CM36283_REG_INT_FLAG);
	if (ret < 0) {
		dev_err(&client->dev,
			"Interrupt flag register read failed: %d\n", ret);
		return IRQ_HANDLED;
	}

	ret >>= CM36283_INT_FLAG_SHIFT;
	switch (ret) {
	case CM36283_CLOSE:
		ev_dir = IIO_EV_DIR_RISING;
		break;
	case CM36283_AWAY:
		ev_dir = IIO_EV_DIR_FALLING;
		break;
	case CM36283_BOTH:
		dev_warn(&client->dev, 
			"Interrupt flag was not cleared, reading proximity sensor data...\n");
		ev_dir = cm36283_fallback_read_ps(indio_dev);
		if (ev_dir < 0) {
			dev_err(&client->dev, "Failed to settle interrupt state: %d\n", ret);
			return IRQ_HANDLED;
		}
		break;
	default:
		dev_err(&client->dev, "Unknown interrupt state: %d\n", ret);
		return IRQ_HANDLED;
	}
	ev_code = IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, IIO_EV_INFO_VALUE,
				       IIO_EV_TYPE_THRESH, ev_dir);

	iio_push_event(indio_dev, ev_code, iio_get_time_ns(indio_dev));
	return IRQ_HANDLED;
}

static const struct iio_info cm36283_info = {
	.read_avail = 		cm36283_read_avail,
	.read_raw = 		cm36283_read_raw,
	.write_raw = 		cm36283_write_raw,
	.read_event_value = 	cm36283_read_prox_thresh,
	.write_event_value = 	cm36283_write_prox_thresh,
	.read_event_config = 	cm36283_read_prox_event_config,
	.write_event_config = 	cm36283_write_prox_event_config,
};

static int cm36283_setup(struct cm36283_data *chip)
{
	struct i2c_client *client = chip->client;
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	int ret;

	ret = i2c_smbus_read_byte_data(client, CM36283_REG_ID_FLAG);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
				     "Failed to read ID flag register\n");

	switch (ret) {
	case CM36283:
		chip->model = CM36283;
		indio_dev->name = "cm36283";
		chip->ps_it_times = cm36283_ps_it_times;
		chip->num_ps_it = ARRAY_SIZE(cm36283_ps_it_times);
		chip->max_ps_value = CM36283_MAX_PS_VALUE;
		break;
	case CM36686:
		chip->model = CM36686;
		indio_dev->name = "cm36686";
		chip->ps_it_times = cm36686_ps_it_times;
		chip->num_ps_it = ARRAY_SIZE(cm36686_ps_it_times);
		chip->max_ps_value = CM36686_MAX_PS_VALUE;
		break;
	default:
		dev_err(&client->dev, "Unknown sensor\n");
		return -ENODEV;
	}

	dev_info(&client->dev, "Detected sensor model: %s\n", indio_dev->name);

	chip->als_conf = CM36283_ALS_ENABLE;
	ret = i2c_smbus_write_word_data(client, CM36283_REG_ALS_CONF,
					chip->als_conf);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
				     "Failed to enable ambient light sensor: %d\n", ret);

	switch (chip->model) {
	case CM36283:
		chip->ps_conf[CM36283_PS_CONF1] =
			CM36283_PS_INT_IN | CM36283_PS_INT_OUT |
			CM36283_PS_DR_1_320 | CM36283_PS_IT_1_3T |
			CM36283_PS_PERS_2;
		break;
	case CM36686:
		chip->ps_conf[CM36283_PS_CONF1] =
			CM36686_PS_INT_IN | CM36686_PS_INT_OUT |
			CM36686_PS_DR_1_320 | CM36686_PS_IT_2_5T |
			CM36686_PS_PERS_2;
		break;
	}

	ret = i2c_smbus_write_word_data(client, CM36283_REG_PS_CONF1,
					chip->ps_conf[CM36283_PS_CONF1]);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
				     "Failed to enable proximity sensor: %d\n", ret);

	switch (chip->model) {
	case CM36283:
		chip->ps_conf[CM36283_PS_CONF3] =
			CM36283_PS_SMART_PERS_ENABLE;
		break;
	case CM36686:
		chip->ps_conf[CM36283_PS_CONF3] =
			CM36686_PS_SMART_PERS_ENABLE |
			CM36686_LED_I_100;
		break;
	}

	ret = i2c_smbus_write_word_data(client, CM36283_REG_PS_CONF3,
					chip->ps_conf[CM36283_PS_CONF3]);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
				     "Failed to enable PS: %d\n", ret);

	if ((ret = device_property_read_u32(&client->dev, "capella,proximity-close-threshold",
					    &chip->ps_close)))
		return dev_err_probe(&client->dev, ret,
		       "Failed to read close proximity threshold: %d\n", ret);


	if ((ret = device_property_read_u32(&client->dev, "capella,proximity-away-threshold",
					    &chip->ps_away))) 
		return dev_err_probe(&client->dev, ret, 
		       "Failed to read away proximity threshold: %d\n", ret);
	

	ret = cm36283_set_prox_thresh(chip);
	if (ret) {
		dev_err(&client->dev, "Failed to set proximity thresholds: %d\n", ret);
		return ret;
	}

	return 0;
}

static void cm36283_shutdown(void *data)
{
	struct cm36283_data *chip = data;
	struct i2c_client *client = chip->client;
	int ret;

	int als_shutdown = chip->als_conf | CM36283_ALS_SD;
	ret = i2c_smbus_write_word_data(client, CM36283_REG_ALS_CONF,
					als_shutdown);
	if (ret < 0)
		dev_err(&client->dev, "Failed to shutdown ALS\n");

	int ps_shutdown = chip->ps_conf[CM36283_PS_CONF1] | CM36283_PS_SD;
	ret = i2c_smbus_write_word_data(client, CM36283_REG_PS_CONF1,
					ps_shutdown);
	if (ret < 0)
		dev_err(&client->dev, "Failed to shutdown PS\n");
}

static int cm36283_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct cm36283_data *chip;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev,
					  sizeof(struct cm36283_data));
	if (!indio_dev)
		return -ENOMEM;

	chip = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	chip->client = client;
	chip->als_it_times = cm36283_als_it_times;
	chip->num_als_it = ARRAY_SIZE(cm36283_als_it_times);
	chip->supplies[CM36283_SUPPLY_VDD].supply = "vdd";
	chip->supplies[CM36283_SUPPLY_VIO].supply = "vio";
	mutex_init(&chip->lock);

	ret = devm_regulator_bulk_get(
		&client->dev, CM36283_SUPPLY_COUNT, chip->supplies);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
				     "Failed to get regulators\n");

	ret = regulator_bulk_enable(CM36283_SUPPLY_COUNT, chip->supplies);
	if (ret < 0) 
		return dev_err_probe(&client->dev, ret,
				     "Failed to enable regulators: %d\n", ret);

	ret = devm_add_action_or_reset(&client->dev, cm36283_shutdown, chip);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to set shutdown action: %d", ret);

	indio_dev->channels = cm36283_channels;
	indio_dev->num_channels = ARRAY_SIZE(cm36283_channels);
	indio_dev->info = &cm36283_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = cm36283_setup(chip);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
				     "Failed to set up registers: %d", ret);

	ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					cm36283_irq_handler,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					indio_dev->name, indio_dev);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to request irq: %d", ret);

	ret = devm_iio_device_register(&client->dev, indio_dev);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to register iio device: %d", ret);

	return 0;
}

static const struct i2c_device_id cm36283_id_table[] = { { "cm36283" },
							 { "cm36686" },
							 {} };

MODULE_DEVICE_TABLE(i2c, cm36283_id_table);

static const struct of_device_id cm36283_of_match[] = {
	{ .compatible = "capella,cm36283" },
	{ .compatible = "capella,cm36686" },
	{}
};
MODULE_DEVICE_TABLE(of, cm36283_of_match);

static struct i2c_driver cm36283_driver = {
	.driver = {
		.name = "cm36283",
		.of_match_table = cm36283_of_match,
	},
	.probe = cm36283_probe,
	.id_table = cm36283_id_table,
};

module_i2c_driver(cm36283_driver);

MODULE_AUTHOR("Erikas Bitovtas <xerikasxx@gmail.com>");
MODULE_DESCRIPTION("CM36283 ambient light and proximity sensor driver");
MODULE_LICENSE("GPL v2");
