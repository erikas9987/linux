#ifndef _LINUX_CYTTSP4_CORE_H
#define _LINUX_CYTTSP4_CORE_H

#include <linux/err.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/regulator/consumer.h>

#include "cyttsp_common.h"

#define CYTTSP4_I2C_NAME "cyttsp4-i2c"

struct cyttsp4_tch {
	__be16 x, y;
	u8 z;
} __packed;

enum cyttsp4_tch_abs {	/* for ordering within the extracted touch data array */
	CY_TCH_X,	/* X */
	CY_TCH_Y,	/* Y */
	CY_TCH_P,	/* P (Z) */
	CY_TCH_T,	/* TOUCH ID */
	CY_TCH_E,	/* EVENT ID */
	CY_TCH_O,	/* OBJECT ID */
	CY_TCH_W,	/* SIZE */
	CY_TCH_MAJ,	/* TOUCH_MAJOR */
	CY_TCH_MIN,	/* TOUCH_MINOR */
	CY_TCH_OR,	/* ORIENTATION */
	CY_TCH_NUM_ABS
};

struct cyttsp4_sysinfo_data {
	u8 hst_mode;
	u8 reserved;
	u8 map_szh;
	u8 map_szl;
	u8 cydata_ofsh;
	u8 cydata_ofsl;
	u8 test_ofsh;
	u8 test_ofsl;
	u8 pcfg_ofsh;
	u8 pcfg_ofsl;
	u8 opcfg_ofsh;
	u8 opcfg_ofsl;
	u8 ddata_ofsh;
	u8 ddata_ofsl;
	u8 mdata_ofsh;
	u8 mdata_ofsl;
} __packed;

struct cyttsp4_tch_abs_params {
	size_t ofs;	/* abs byte offset */
	size_t size;	/* size in bits */
	size_t max;	/* max value */
	size_t bofs;	/* bit offset */
};

struct cyttsp4_sysinfo_ofs {
	size_t chip_type;
	size_t cmd_ofs;
	size_t rep_ofs;
	size_t rep_sz;
	size_t num_btns;
	size_t num_btn_regs;	/* ceil(num_btns/4) */
	size_t tt_stat_ofs;
	size_t tch_rec_size;
	size_t obj_cfg0;
	size_t max_tchs;
	size_t mode_size;
	size_t data_size;
	size_t rep_hdr_size;
	size_t map_sz;
	size_t max_x;
	size_t x_origin;	/* left or right corner */
	size_t max_y;
	size_t y_origin;	/* upper or lower corner */
	size_t max_p;
	size_t cydata_ofs;
	size_t test_ofs;
	size_t pcfg_ofs;
	size_t opcfg_ofs;
	size_t ddata_ofs;
	size_t mdata_ofs;
	size_t cydata_size;
	size_t test_size;
	size_t pcfg_size;
	size_t opcfg_size;
	size_t ddata_size;
	size_t mdata_size;
	size_t btn_keys_size;
	struct cyttsp4_tch_abs_params tch_abs[CY_TCH_NUM_ABS];
	size_t btn_rec_size; /* btn record size (in bytes) */
	size_t btn_diff_ofs;/* btn data loc ,diff counts, (Op-Mode byte ofs) */
	size_t btn_diff_size;/* btn size of diff counts (in bits) */
	size_t noise_data_ofs;
	size_t noise_data_sz;
};

/* TrueTouch Standard Product Gen3 interface definition */
struct cyttsp4_xydata {
	u8 hst_mode;
	u8 tt_mode;
	u8 tt_stat;
	struct cyttsp4_tch tch1;
	u8 touch12_id;
	struct cyttsp4_tch tch2;
	u8 gest_cnt;
	u8 gest_id;
	struct cyttsp4_tch tch3;
	u8 touch34_id;
	struct cyttsp4_tch tch4;
	u8 tt_undef[3];
	u8 act_dist;
	u8 tt_reserved;
} __packed;

enum cyttsp4_state {
	CY_IDLE_STATE,
	CY_ACTIVE_STATE,
	CY_BL_STATE,
};

struct cyttsp4 {
	struct device *dev;
	struct input_dev *input;
	struct gpio_desc *rst_gpio;
	struct regulator_bulk_data *supplies;
	struct mutex mutex;
	const struct cyttsp_bus_ops *bus_ops;
	struct completion bl_ready;
	struct cyttsp4_sysinfo_data sysinfo_data;
	struct cyttsp4_sysinfo_ofs sysinfo_offsets;
	struct cyttsp4_xydata xy_data;
	enum cyttsp4_state state;
	bool suspended;
	int irq;
	
	u8 *xfer_buf;
};

extern struct cyttsp4 *cyttsp4_probe(const struct cyttsp_bus_ops *ops,
		struct device *dev, u16 irq, size_t xfer_buf_size);
extern int cyttsp4_remove(struct cyttsp4 *ts);
int cyttsp_i2c_write_block_data(struct device *dev, u8 *xfer_buf, u16 addr,
		u8 length, const void *values);
int cyttsp_i2c_read_block_data(struct device *dev, u8 *xfer_buf, u16 addr,
		u8 length, void *values);
extern const struct dev_pm_ops cyttsp4_pm_ops;
#endif
