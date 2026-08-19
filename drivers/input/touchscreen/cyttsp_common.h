#ifndef __CYTTSP_COMMON_H__
#define __CYTTSP_COMMON_H__

#include <linux/kernel.h>
#include <linux/types.h>

struct cyttsp_bus_ops {
	u16 bustype;
	int (*write)(struct device *dev, u8 *xfer_buf, u16 addr, u8 length,
			const void *values);
	int (*read)(struct device *dev, u8 *xfer_buf, u16 addr, u8 length,
			void *values);
};

#endif
