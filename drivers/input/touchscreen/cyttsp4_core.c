#include "cyttsp4_core.h"
#include "linux/array_size.h"
#include "linux/completion.h"
#include "linux/delay.h"
#include "linux/dev_printk.h"
#include "linux/gpio/consumer.h"
#include "linux/input.h"
#include "linux/input/mt.h"
#include "linux/input/touchscreen.h"
#include "linux/interrupt.h"
#include "linux/regulator/consumer.h"

#define GET_HSTMODE(reg)		((reg & 0x70) >> 4)
#define CY_REG_BASE 0x00
#define CY_LOW_POWER_MODE 0x04
#define IS_BOOTLOADER(hst_mode, reset_detect)	\
		((hst_mode) & 0x01 || (reset_detect) != 0)

#define CY_NUM_RETRY 3
#define CY_MAX_ID 16
#define CY_DELAY_DFLT 20
#define CY_DELAY_MAX 500

static u8 ldr_exit[] = {
	0xFF, 0x01, 0x3B, 0x00, 0x00, 0x4F, 0x6D, 0x17
};

static inline size_t merge_bytes(u8 high, u8 low)
{
	return (high << 8) | low;
}

static const struct regulator_bulk_data cyttsp4_supplies[] = {
	{ .supply = "vccd", },
	{ .supply = "vddd", },
};

static int ttsp_read_block_data(struct cyttsp4 *ts, u8 command,
				u8 length, void *buf)
{
	int error;
	int tries;

	for (tries = 0; tries < CY_NUM_RETRY; tries++) {
		error = ts->bus_ops->read(ts->dev, ts->xfer_buf, command,
				length, buf);
		if (!error)
			return 0;

		msleep(CY_DELAY_DFLT);
	}

	return -EIO;
}

static int ttsp_write_block_data(struct cyttsp4 *ts, u8 command,
				 u8 length, void *buf)
{
	int error;
	int tries;

	for (tries = 0; tries < CY_NUM_RETRY; tries++) {
		error = ts->bus_ops->write(ts->dev, ts->xfer_buf, command,
				length, buf);
		if (!error)
			return 0;
		dev_err(ts->dev, "Attempt %d, error received: %pe", tries + 1, ERR_PTR(error));

		msleep(CY_DELAY_DFLT);
	}

	return -EIO;
}

static int ttsp_send_command(struct cyttsp4 *ts, u8 cmd)
{
	return ttsp_write_block_data(ts, CY_REG_BASE, sizeof(cmd), &cmd);
}

static void cyttsp4_hard_reset(struct cyttsp4 *ts)
{
	if (ts->rst_gpio) {
		gpiod_set_value_cansleep(ts->rst_gpio, 1);
		usleep_range(1000, 2000);
		gpiod_set_value_cansleep(ts->rst_gpio, 0);
		usleep_range(5000, 6000);
	}
}

static int cyttsp4_enable(struct cyttsp4 *ts)
{
	int error;

	/*
	 * The device firmware can wake on an I2C or SPI memory slave
	 * address match. So just reading a register is sufficient to
	 * wake up the device. The first read attempt will fail but it
	 * will wake it up making the second read attempt successful.
	 */
	error = ttsp_read_block_data(ts, CY_REG_BASE,
				     sizeof(ts->xy_data), &ts->xy_data);
	if (error)
		return error;

	if (GET_HSTMODE(ts->xy_data.hst_mode))
		return -EIO;

	enable_irq(ts->irq);

	return 0;
}

static int cyttsp4_disable(struct cyttsp4 *ts)
{
	int error;

	error = ttsp_send_command(ts, CY_LOW_POWER_MODE);
	if (error)
		return error;

	disable_irq(ts->irq);

	return 0;
}


static int cyttsp4_open(struct input_dev *dev)
{
	struct cyttsp4 *ts = input_get_drvdata(dev);
	int retval = 0;

	if (!ts->suspended)
		retval = cyttsp4_enable(ts);

	return retval;
}

static void cyttsp4_close(struct input_dev *dev)
{
	struct cyttsp4 *ts = input_get_drvdata(dev);

	if (!ts->suspended)
		cyttsp4_disable(ts);
}

static int cyttsp4_get_sysinfo_offsets(struct cyttsp4 *ts)
{
	struct cyttsp4_sysinfo_data *si = &ts->sysinfo_data;
	struct cyttsp4_sysinfo_ofs *ofs = &ts->sysinfo_offsets;
	int ret;

	guard(mutex)(&ts->mutex);
	ret = ttsp_read_block_data(ts, CY_REG_BASE, sizeof(*si), si);
	if (ret)
		return ret;

	ofs->map_sz = merge_bytes(si->map_szh, si->map_szl);
	ofs->cydata_ofs = merge_bytes(si->cydata_ofsh, si->cydata_ofsl);
	ofs->test_ofs = merge_bytes(si->test_ofsh, si->test_ofsl);
	ofs->pcfg_ofs = merge_bytes(si->pcfg_ofsh, si->pcfg_ofsl);
	ofs->opcfg_ofs = merge_bytes(si->opcfg_ofsh, si->opcfg_ofsl);
	ofs->ddata_ofs = merge_bytes(si->ddata_ofsh, si->ddata_ofsl);
	ofs->mdata_ofs = merge_bytes(si->mdata_ofsh, si->mdata_ofsl);

	dev_info(ts->dev, "ofs->map_sz = 0x%04zx", ofs->map_sz);
	dev_info(ts->dev, "ofs->cydata_ofs = 0x%04zx", ofs->cydata_ofs);
	dev_info(ts->dev, "ofs->test_ofs = 0x%04zx", ofs->test_ofs);
	dev_info(ts->dev, "ofs->pcfg_ofs = 0x%04zx", ofs->pcfg_ofs);
	dev_info(ts->dev, "ofs->opcfg_ofs = 0x%04zx", ofs->opcfg_ofs);
	dev_info(ts->dev, "ofs->ddata_ofs = 0x%04zx", ofs->ddata_ofs);
	dev_info(ts->dev, "ofs->mdata_ofs = 0x%04zx", ofs->mdata_ofs);

	return ret;
}

static int cyttsp4_enter_sysinfo_mode(struct cyttsp4 *ts)
{
	int ret;

	reinit_completion(&ts->bl_ready);
	enable_irq(ts->irq);

	guard(mutex)(&ts->mutex);
	ret = ttsp_write_block_data(ts, CY_REG_BASE, sizeof(ldr_exit), (u8 *)ldr_exit);
	if (ret)
		goto out;

	dev_info(ts->dev, "Wrote ldr_exit");

	if (!wait_for_completion_timeout(&ts->bl_ready,
			msecs_to_jiffies(CY_DELAY_DFLT * CY_DELAY_MAX))) {
		dev_err(ts->dev, "timeout waiting for soft reset");
		ret = -EIO;
	}
	
out:
	disable_irq(ts->irq);
	return ret;
}

static void cyttsp4_disable_regulators(void *data)
{
	struct cyttsp4 *ts = data;
	int ret;

	ret = regulator_bulk_disable(ARRAY_SIZE(cyttsp4_supplies),
				 ts->supplies);
	if (ret)
		dev_warn(ts->dev, "Failed to disable regulators: %pe",
			ERR_PTR(ret));
}

static irqreturn_t cyttsp4_irq(int irq, void *handle)
{
	struct cyttsp4 *ts = handle;
	dev_info(ts->dev, "Received an interrupt!");
	if (unlikely(ts->state == CY_BL_STATE)) {
		complete(&ts->bl_ready);
		dev_info(ts->dev, "Completed bootloader state");
		goto out;
	}

out:
    return IRQ_HANDLED;
}

struct cyttsp4 *cyttsp4_probe(const struct cyttsp_bus_ops *ops,
		struct device *dev, u16 irq, size_t xfer_buf_size)
{
	int ret;
	struct cyttsp4 *ts;
	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (IS_ERR(ts))
		return ERR_PTR(-ENOMEM);

	ts->input = devm_input_allocate_device(dev);
	if (IS_ERR(ts->input))
		return ERR_PTR(-ENOMEM);

	ts->dev = dev;
	ts->irq = irq;
	ts->bus_ops = ops;

	dev_info(ts->dev, "Allocated input device");

	ts->xfer_buf = devm_kzalloc(dev, xfer_buf_size, GFP_KERNEL);
	if (IS_ERR(ts->xfer_buf))
		return ERR_PTR(-ENOMEM);

	dev_info(ts->dev, "Allocated xfer buf");

	ts->rst_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ts->rst_gpio))
		return ERR_PTR(PTR_ERR(ts->rst_gpio));
	dev_info(ts->dev, "Got reset gpio");

	ret = devm_regulator_bulk_get_const(dev,
					    ARRAY_SIZE(cyttsp4_supplies),
					    cyttsp4_supplies,
					    &ts->supplies);
	if (ret)
		return ERR_PTR(ret);
	dev_info(ts->dev, "Got regulators");

	ret = regulator_bulk_enable(ARRAY_SIZE(cyttsp4_supplies),
				    ts->supplies);
	if (ret)
		return ERR_PTR(ret);
	dev_info(ts->dev, "Enabled regulators");

	ret = devm_add_action_or_reset(dev, cyttsp4_disable_regulators, ts);
	if (ret)
		return ERR_PTR(ret);
	dev_info(ts->dev, "Added disabled regulator reset");

	ret = devm_mutex_init(dev, &ts->mutex);
	if (ret)
		return ERR_PTR(ret);
	dev_info(ts->dev, "Created mutex");

	init_completion(&ts->bl_ready);

	ts->input->name = "Cypress TTSP4 TouchScreen";
	ts->input->id.bustype = ops->bustype;
	ts->input->dev.parent = ts->dev;

	ts->input->open = cyttsp4_open;
	ts->input->close = cyttsp4_close;

	input_set_drvdata(ts->input, ts);

	input_set_capability(ts->input, EV_ABS, ABS_MT_POSITION_X);
	input_set_capability(ts->input, EV_ABS, ABS_MT_POSITION_Y);
	/* One byte for width 0..255 so this is the limit */
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);

	touchscreen_parse_properties(ts->input, true, NULL);

	ret = input_mt_init_slots(ts->input, CY_MAX_ID, INPUT_MT_DIRECT);
	if (ret)
		return ERR_PTR(ret);
	dev_info(ts->dev, "Initialized slots");

	ret = devm_request_threaded_irq(dev, ts->irq, NULL, cyttsp4_irq, IRQF_ONESHOT | IRQF_NO_AUTOEN, "cyttsp4", ts);
	if (ret)
		return ERR_PTR(ret);
	dev_info(ts->dev, "Initialized irq");

	cyttsp4_hard_reset(ts);

	ret = cyttsp4_enter_sysinfo_mode(ts);
	if (ret)
		return ERR_PTR(ret);
	dev_info(ts->dev, "Entered sysinfo mode");

	ret = cyttsp4_get_sysinfo_offsets(ts);
	if (ret)
		return ERR_PTR(ret);

	return ts;
}
EXPORT_SYMBOL_GPL(cyttsp4_probe);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cypress TrueTouch4(R) Standard touchscreen driver core");
