#include <linux/clk.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define T4K37_REG_CHIP_ID CCI_REG16(0x0000)
#define T4K37_REG_MODE_SELECT CCI_REG8(0x0100)
#define T4K37_REG_GROUP_PARA_HOLD CCI_REG8(0x0104)
#define T4K37_REG_VT_PIX_CLK_DIV CCI_REG8(0x0301)
#define T4K37_REG_VT_SYS_CLK_DIV CCI_REG8(0x0303)
#define T4K37_REG_PRE_PLL_CLK_DIV CCI_REG8(0x0305)
#define T4K37_REG_PLL_MULTIPLIER CCI_REG16(0x030E)
#define T4K37_REG_TEST_PATTERN CCI_REG16(0x0600)

#define T4K37_TEST_PATTERN_DISABLE 0
#define T4K37_TEST_PATTERN_ENABLE 1

#define T4K37_GROUP_PARA_HOLD_DISABLE 0x00
#define T4K37_GROUP_PARA_HOLD_ENABLE 0x01

#define T4K37_MODE_STANDBY 0x00
#define T4K37_MODE_STREAMING 0x01

#define T4K37_EXTCLK_RATE 19200000
#define T4K37_NUM_SUPPLIES 3
#define T4K37_CHIP_ID 0x1C21

#define T4K37_MODE(_width, _height, _fps, _regs) {	\
	.width = _width,				\
	.height = _height,				\
	.code = MEDIA_BUS_FMT_SGRBG10_1X10,		\
	.fps = _fps,					\
	.regs = _regs,					\
	.num_regs = ARRAY_SIZE(_regs)			\
}							

struct t4k37_mode {
	u32 height;
	u32 width;
	u32 code;
	u32 fps;

	const struct cci_reg_sequence *regs;
	size_t num_regs;
};

static const char * const t4k37_test_pattern_menu[] = {
	"Disabled",
	"Enabled",
};

static const int t4k37_test_pattern_val[] = {
	T4K37_TEST_PATTERN_DISABLE,
	T4K37_TEST_PATTERN_ENABLE,
};

struct t4k37 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[T4K37_NUM_SUPPLIES];
	struct clk *extclk;
	struct regmap *regmap;

	const struct t4k37_mode *current_mode;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_fract frame_interval;

	struct media_pad pad;
	/* Protect the sensor from concurrent access */
	struct mutex lock;
	bool streaming;

	u32 nlanes;
	u32 extclk_rate;
};

static struct cci_reg_sequence const t4k37_init_settings[] = {
	{T4K37_REG_GROUP_PARA_HOLD, T4K37_GROUP_PARA_HOLD_ENABLE},
	{CCI_REG8(0x0101), 0x00},	// -/-/-/-/-/-/IMAGE_ORIENT[1:0];
	{CCI_REG8(0x0103), 0x00},	// -/-/-/-/-/-/MIPI_RST/SOFTWARE_RESET;
	{T4K37_REG_GROUP_PARA_HOLD, T4K37_GROUP_PARA_HOLD_DISABLE},
	{CCI_REG8(0x0105), 0x00},	// -/-/-/-/-/-/-/MSK_CORRUPT_FR;
	{CCI_REG8(0x0110), 0x00},	// -/-/-/-/-/CSI_CHAN_IDNTF[2:0];
	{CCI_REG8(0x0111), 0x02},	// -/-/-/-/-/-/CSI_SIGNAL_MOD[1:0];
	{CCI_REG8(0x0112), 0x0A},	// CSI_DATA_FORMAT[15:8];
	{CCI_REG8(0x0113), 0x0A},	// CSI_DATA_FORMAT[7:0];
	{CCI_REG8(0x0114), 0x03},	// -/-/-/-/-/-/CSI_LANE_MODE[1:0];
	{CCI_REG8(0x0115), 0x30},	// -/-/CSI_10TO8_DT[5:0];
	{CCI_REG8(0x0117), 0x32},	// -/-/CSI_10TO6_DT[5:0];
	{CCI_REG8(0x0130), 0x13},	// EXTCLK_FRQ_MHZ[15:8];
	{CCI_REG8(0x0131), 0x33},	// EXTCLK_FRQ_MHZ[7:0];
	{CCI_REG8(0x0141), 0x00},	// -/-/-/-/-/CTX_SW_CTL[2:0];
	{CCI_REG8(0x0142), 0x00},	// -/-/-/-/CONT_MDSEL_FRVAL[1:0]/CONT_FRCNT_MSK/CONT_GRHOLD_MSK;
	{CCI_REG8(0x0143), 0x00},	// R_FRAME_COUNT[7:0];
	{CCI_REG8(0x0202), 0x0C},	// COAR_INTEGR_TIM[15:8];
	{CCI_REG8(0x0203), 0x42},	// COAR_INTEGR_TIM[7:0];
	{CCI_REG8(0x0204), 0x00},	// -/-/-/-/ANA_GA_CODE_GL[11:8];
	{CCI_REG8(0x0205), 0x37},	// ANA_GA_CODE_GL[7:0];
	{CCI_REG8(0x0210), 0x01},	// -/-/-/-/-/-/DG_GA_GREENR[9:8];
	{CCI_REG8(0x0211), 0x00},	// DG_GA_GREENR[7:0];
	{CCI_REG8(0x0212), 0x01},	// -/-/-/-/-/-/DG_GA_RED[9:8];
	{CCI_REG8(0x0213), 0x00},	// DG_GA_RED[7:0];
	{CCI_REG8(0x0214), 0x01},	// -/-/-/-/-/-/DG_GA_BLUE[9:8];
	{CCI_REG8(0x0215), 0x00},	// DG_GA_BLUE[7:0];
	{CCI_REG8(0x0216), 0x01},	// -/-/-/-/-/-/DG_GA_GREENB[9:8];
	{CCI_REG8(0x0217), 0x00},	// DG_GA_GREENB[7:0];
	{CCI_REG8(0x0230), 0x00},	// -/-/-/HDR_MODE[4:0];
	{CCI_REG8(0x0232), 0x04},	// HDR_RATIO_1[7:0];
	{CCI_REG8(0x0234), 0x00},	// HDR_SHT_INTEGR_TIM[15:8];
	{CCI_REG8(0x0235), 0x19},	// HDR_SHT_INTEGR_TIM[7:0];
	{T4K37_REG_VT_PIX_CLK_DIV, 0x02},	// -/-/-/-/VT_PIX_CLK_DIV[3:0];
	{T4K37_REG_VT_SYS_CLK_DIV, 0x08},	// -/-/-/-/VT_SYS_CLK_DIV[3:0];
	{T4K37_REG_PRE_PLL_CLK_DIV, 0x03},	// -/-/-/-/-/PRE_PLL_CLK_DIV[2:0];
	{CCI_REG8(0x0306), 0x00},	// -/-/-/-/-/-/-/PLL_MULTIPLIER[8];
	{CCI_REG8(0x0307), 0xDA},	// PLL_MULTIPLIER[7:0];
	{CCI_REG8(0x030B), 0x04},	// -/-/-/-/OP_SYS_CLK_DIV[3:0];
	{CCI_REG8(0x030D), 0x03},	// -/-/-/-/-/PRE_PLL_ST_CLK_DIV[2:0];
	{T4K37_REG_PLL_MULTIPLIER, 0x87},	// -/-/-/-/-/-/-/PLL_MULT_ST[8];
	{CCI_REG8(0x0310), 0x00},	// -/-/-/-/-/-/-/OPCK_PLLSEL;
	{CCI_REG8(0x0340), 0x0C},	// FR_LENGTH_LINES[15:8];
	{CCI_REG8(0x0341), 0x48},	// FR_LENGTH_LINES[7:0];
	{CCI_REG8(0x0342), 0x11},	// LINE_LENGTH_PCK[15:8];
	{CCI_REG8(0x0343), 0xE8},	// LINE_LENGTH_PCK[7:0];
	{CCI_REG8(0x0344), 0x00},	// -/-/-/-/H_CROP[3:0];
	{CCI_REG8(0x0346), 0x00},	// Y_ADDR_START[15:8];
	{CCI_REG8(0x0347), 0x00},	// Y_ADDR_START[7:0];
	{CCI_REG8(0x034A), 0x0C},	// Y_ADDR_END[15:8];
	{CCI_REG8(0x034B), 0x2F},	// Y_ADDR_END[7:0];
	{CCI_REG8(0x034C), 0x10},	// X_OUTPUT_SIZE[15:8];
	{CCI_REG8(0x034D), 0x70},	// X_OUTPUT_SIZE[7:0];
	{CCI_REG8(0x034E), 0x0C},	// Y_OUTPUT_SIZE[15:8];
	{CCI_REG8(0x034F), 0x30},	// Y_OUTPUT_SIZE[7:0];
	{CCI_REG8(0x0401), 0x00},	// -/-/-/-/-/-/SCALING_MODE[1:0];
	{CCI_REG8(0x0403), 0x00},	// -/-/-/-/-/-/SPATIAL_SAMPLING[1:0];
	{CCI_REG8(0x0404), 0x10},	// SCALE_M[7:0];
	{CCI_REG8(0x0408), 0x00},	// DCROP_XOFS[15:8];
	{CCI_REG8(0x0409), 0x00},	// DCROP_XOFS[7:0];
	{CCI_REG8(0x040A), 0x00},	// DCROP_YOFS[15:8];
	{CCI_REG8(0x040B), 0x00},	// DCROP_YOFS[7:0];
	{CCI_REG8(0x040C), 0x10},	// DCROP_WIDTH[15:8];
	{CCI_REG8(0x040D), 0x70},	// DCROP_WIDTH[7:0];
	{CCI_REG8(0x040E), 0x0C},	// DCROP_HIGT[15:8];
	{CCI_REG8(0x040F), 0x30},	// DCROP_HIGT[7:0];
	{CCI_REG8(0x0601), 0x00},	// TEST_PATT_MODE[7:0];
	{CCI_REG8(0x0602), 0x02},	// -/-/-/-/-/-/TEST_DATA_RED[9:8];
	{CCI_REG8(0x0603), 0xC0},	// TEST_DATA_RED[7:0];
	{CCI_REG8(0x0604), 0x02},	// -/-/-/-/-/-/TEST_DATA_GREENR[9:8];
	{CCI_REG8(0x0605), 0xC0},	// TEST_DATA_GREENR[7:0];
	{CCI_REG8(0x0606), 0x02},	// -/-/-/-/-/-/TEST_DATA_BLUE[9:8];
	{CCI_REG8(0x0607), 0xC0},	// TEST_DATA_BLUE[7:0];
	{CCI_REG8(0x0608), 0x02},	// -/-/-/-/-/-/TEST_DATA_GREENB[9:8];
	{CCI_REG8(0x0609), 0xC0},	// TEST_DATA_GREENB[7:0];
	{CCI_REG8(0x060A), 0x00},	// HO_CURS_WIDTH[15:8];
	{CCI_REG8(0x060B), 0x00},	// HO_CURS_WIDTH[7:0];
	{CCI_REG8(0x060C), 0x00},	// HO_CURS_POSITION[15:8];
	{CCI_REG8(0x060D), 0x00},	// HO_CURS_POSITION[7:0];
	{CCI_REG8(0x060E), 0x00},	// VE_CURS_WIDTH[15:8];
	{CCI_REG8(0x060F), 0x00},	// VE_CURS_WIDTH[7:0];
	{CCI_REG8(0x0610), 0x00},	// VE_CURS_POSITION[15:8];
	{CCI_REG8(0x0611), 0x00},	// VE_CURS_POSITION[7:0];
	{CCI_REG8(0x0800), 0x88},	// TCLK_POST[7:3]/-/-/-;
	{CCI_REG8(0x0801), 0x38},	// THS_PREPARE[7:3]/-/-/-;
	{CCI_REG8(0x0802), 0x78},	// THS_ZERO[7:3]/-/-/-;
	{CCI_REG8(0x0803), 0x48},	// THS_TRAIL[7:3]/-/-/-;
	{CCI_REG8(0x0804), 0x48},	// TCLK_TRAIL[7:3]/-/-/-;
	{CCI_REG8(0x0805), 0x40},	// TCLK_PREPARE[7:3]/-/-/-;
	{CCI_REG8(0x0806), 0x00},	// TCLK_ZERO[7:3]/-/-/-;
	{CCI_REG8(0x0807), 0x48},	// TLPX[7:3]/-/-/-;
	{CCI_REG8(0x0808), 0x01},	// -/-/-/-/-/-/DPHY_CTRL[1:0];
	{CCI_REG8(0x0820), 0x08},	// MSB_LBRATE[31:24];
	{CCI_REG8(0x0821), 0x40},	// MSB_LBRATE[23:16];
	{CCI_REG8(0x0822), 0x00},	// MSB_LBRATE[15:8];
	{CCI_REG8(0x0823), 0x00},	// MSB_LBRATE[7:0];
	{CCI_REG8(0x0900), 0x00},	// -/-/-/-/-/-/H_BIN[1:0];
	{CCI_REG8(0x0901), 0x00},	// -/-/-/-/-/-/V_BIN_MODE[1:0] : 0x01/0x00 for SUM/AVE;
	{CCI_REG8(0x0902), 0x00},	// -/-/-/-/-/-/BINNING_WEIGHTING[1:0]; (binning-average)
	{CCI_REG8(0x0A05), 0x01},	// -/-/-/-/-/-/-/MAP_DEF_EN;
	{CCI_REG8(0x0A06), 0x01},	// -/-/-/-/-/-/-/SGL_DEF_EN;
	{CCI_REG8(0x0A07), 0x98},	// SGL_DEF_W[7:0];
	{CCI_REG8(0x0A0A), 0x01},	// -/-/-/-/-/-/-/COMB_CPLT_SGL_DEF_EN;
	{CCI_REG8(0x0A0B), 0x98},	// COMB_CPLT_SGL_DEF_W[7:0];
	{CCI_REG8(0x0C00), 0x00},	// -/-/-/-/-/-/GLBL_RST_CTRL1[1:0];
	{CCI_REG8(0x0C02), 0x00},	// GLBL_RST_CFG_1[7:0];
	{CCI_REG8(0x0C04), 0x00},	// TRDY_CTRL[15:8];
	{CCI_REG8(0x0C05), 0x32},	// TRDY_CTRL[7:0];
	{CCI_REG8(0x0C06), 0x00},	// TRDOUT_CTRL[15:8];
	{CCI_REG8(0x0C07), 0x10},	// TRDOUT_CTRL[7:0];
	{CCI_REG8(0x0C08), 0x00},	// TSHT_STB_DLY_CTRL[15:8];
	{CCI_REG8(0x0C09), 0x49},	// TSHT_STB_DLY_CTRL[7:0];
	{CCI_REG8(0x0C0A), 0x01},	// TSHT_STB_WDTH_CTRL[15:8];
	{CCI_REG8(0x0C0B), 0x68},	// TSHT_STB_WDTH_CTRL[7:0];
	{CCI_REG8(0x0C0C), 0x00},	// TFLSH_STB_DLY_CTRL[15:8];
	{CCI_REG8(0x0C0D), 0x34},	// TFLSH_STB_DLY_CTRL[7:0];
	{CCI_REG8(0x0C0E), 0x00},	// TFLSH_STB_WDTH_CTRL[15:8];
	{CCI_REG8(0x0C0F), 0x40},	// TFLSH_STB_WDTH_CTRL[7:0];
	{CCI_REG8(0x0C12), 0x01},	// FLASH_ADJ[7:0];
	{CCI_REG8(0x0C14), 0x00},	// FLASH_LINE[15:8];
	{CCI_REG8(0x0C15), 0x01},	// FLASH_LINE[7:0];
	{CCI_REG8(0x0C16), 0x00},	// FLASH_DELAY[15:8];
	{CCI_REG8(0x0C17), 0x20},	// FLASH_DELAY[7:0];
	{CCI_REG8(0x0C18), 0x00},	// FLASH_WIDTH[15:8];
	{CCI_REG8(0x0C19), 0x40},	// FLASH_WIDTH[7:0];
	{CCI_REG8(0x0C1A), 0x00},	// -/-/FLASH_MODE[5:0];
	{CCI_REG8(0x0C1B), 0x00},	// -/-/-/-/-/-/-/FLASH_TRG;
	{CCI_REG8(0x0F00), 0x00},	// -/-/-/-/-/ABF_LUT_CTL[2:0];
	{CCI_REG8(0x0F01), 0x01},	// -/-/-/-/-/-/ABF_LUT_MODE[1:0];
	{CCI_REG8(0x0F02), 0x01},	// ABF_ES_A[15:8];
	{CCI_REG8(0x0F03), 0x40},	// ABF_ES_A[7:0];
	{CCI_REG8(0x0F04), 0x00},	// -/-/-/-/ABF_AG_A[11:8];
	{CCI_REG8(0x0F05), 0x40},	// ABF_AG_A[7:0];
	{CCI_REG8(0x0F06), 0x01},	// -/-/-/-/-/-/-/ABF_DG_GR_A[8];
	{CCI_REG8(0x0F07), 0x00},	// ABF_DG_GR_A[7:0];
	{CCI_REG8(0x0F08), 0x01},	// -/-/-/-/-/-/-/ABF_DG_R_A[8];
	{CCI_REG8(0x0F09), 0x00},	// ABF_DG_R_A[7:0];
	{CCI_REG8(0x0F0A), 0x01},	// -/-/-/-/-/-/-/ABF_DG_B_A[8];
	{CCI_REG8(0x0F0B), 0x00},	// ABF_DG_B_A[7:0];
	{CCI_REG8(0x0F0C), 0x01},	// -/-/-/-/-/-/-/ABF_DG_GB_A[8];
	{CCI_REG8(0x0F0D), 0x00},	// ABF_DG_GB_A[7:0];
	{CCI_REG8(0x0F0E), 0x00},	// -/-/-/-/-/-/-/F_ENTRY_A;
	{CCI_REG8(0x0F0F), 0x01},	// ABF_ES_B[15:8];
	{CCI_REG8(0x0F10), 0x50},	// ABF_ES_B[7:0];
	{CCI_REG8(0x0F11), 0x00},	// -/-/-/-/ABF_AG_B[11:8];
	{CCI_REG8(0x0F12), 0x50},	// ABF_AG_B[7:0];
	{CCI_REG8(0x0F13), 0x01},	// -/-/-/-/-/-/-/ABF_DG_GR_B[8];
	{CCI_REG8(0x0F14), 0x00},	// ABF_DG_GR_B[7:0];
	{CCI_REG8(0x0F15), 0x01},	// -/-/-/-/-/-/-/ABF_DG_R_B[8];
	{CCI_REG8(0x0F16), 0x00},	// ABF_DG_R_B[7:0];
	{CCI_REG8(0x0F17), 0x01},	// -/-/-/-/-/-/-/ABF_DG_B_B[8];
	{CCI_REG8(0x0F18), 0x00},	// ABF_DG_B_B[7:0];
	{CCI_REG8(0x0F19), 0x01},	// -/-/-/-/-/-/-/ABF_DG_GB_B[8];
	{CCI_REG8(0x0F1A), 0x00},	// ABF_DG_GB_B[7:0];
	{CCI_REG8(0x0F1B), 0x00},	// -/-/-/-/-/-/-/F_ENTRY_B;
	{CCI_REG8(0x0F1C), 0x01},	// ABF_ES_C[15:8];
	{CCI_REG8(0x0F1D), 0x60},	// ABF_ES_C[7:0];
	{CCI_REG8(0x0F1E), 0x00},	// -/-/-/-/ABF_AG_C[11:8];
	{CCI_REG8(0x0F1F), 0x60},	// ABF_AG_C[7:0];
	{CCI_REG8(0x0F20), 0x01},	// -/-/-/-/-/-/-/ABF_DG_GR_C[8];
	{CCI_REG8(0x0F21), 0x00},	// ABF_DG_GR_C[7:0];
	{CCI_REG8(0x0F22), 0x01},	// -/-/-/-/-/-/-/ABF_DG_R_C[8];
	{CCI_REG8(0x0F23), 0x00},	// ABF_DG_R_C[7:0];
	{CCI_REG8(0x0F24), 0x01},	// -/-/-/-/-/-/-/ABF_DG_B_C[8];
	{CCI_REG8(0x0F25), 0x00},	// ABF_DG_B_C[7:0];
	{CCI_REG8(0x0F26), 0x01},	// -/-/-/-/-/-/-/ABF_DG_GB_C[8];
	{CCI_REG8(0x0F27), 0x00},	// ABF_DG_GB_C[7:0];
	{CCI_REG8(0x0F28), 0x00},	// -/-/-/-/-/-/-/F_ENTRY_C;
	{CCI_REG8(0x1101), 0x00},	// -/-/-/-/-/-/IMAGE_ORIENT_1B[1:0];
	{CCI_REG8(0x1143), 0x00},	// R_FRAME_COUNT_1B[7:0];
	{CCI_REG8(0x1202), 0x00},	// COAR_INTEGR_TIM_1B[15:8];
	{CCI_REG8(0x1203), 0x19},	// COAR_INTEGR_TIM_1B[7:0];
	{CCI_REG8(0x1204), 0x00},	// -/-/-/-/ANA_GA_CODE_GL_1B[11:8];
	{CCI_REG8(0x1205), 0x40},	// ANA_GA_CODE_GL_1B[7:0];
	{CCI_REG8(0x1210), 0x01},	// -/-/-/-/-/-/DG_GA_GREENR_1B[9:8];
	{CCI_REG8(0x1211), 0x00},	// DG_GA_GREENR_1B[7:0];
	{CCI_REG8(0x1212), 0x01},	// -/-/-/-/-/-/DG_GA_RED_1B[9:8];
	{CCI_REG8(0x1213), 0x00},	// DG_GA_RED_1B[7:0];
	{CCI_REG8(0x1214), 0x01},	// -/-/-/-/-/-/DG_GA_BLUE_1B[9:8];
	{CCI_REG8(0x1215), 0x00},	// DG_GA_BLUE_1B[7:0];
	{CCI_REG8(0x1216), 0x01},	// -/-/-/-/-/-/DG_GA_GREENB_1B[9:8];
	{CCI_REG8(0x1217), 0x00},	// DG_GA_GREENB_1B[7:0];
	{CCI_REG8(0x1230), 0x00},	// -/-/-/HDR_MODE_1B[4:0];
	{CCI_REG8(0x1232), 0x04},	// HDR_RATIO_1_1B[7:0];
	{CCI_REG8(0x1234), 0x00},	// HDR_SHT_INTEGR_TIM_1B[15:8];
	{CCI_REG8(0x1235), 0x19},	// HDR_SHT_INTEGR_TIM_1B[7:0];
	{CCI_REG8(0x1340), 0x0C},	// FR_LENGTH_LINES_1B[15:8];
	{CCI_REG8(0x1341), 0x80},	// FR_LENGTH_LINES_1B[7:0];
	{CCI_REG8(0x1342), 0x15},	// LINE_LENGTH_PCK_1B[15:8];
	{CCI_REG8(0x1343), 0xE0},	// LINE_LENGTH_PCK_1B[7:0];
	{CCI_REG8(0x1344), 0x00},	// -/-/-/-/H_CROP_1B[3:0];
	{CCI_REG8(0x1346), 0x00},	// Y_ADDR_START_1B[15:8];
	{CCI_REG8(0x1347), 0x00},	// Y_ADDR_START_1B[7:0];
	{CCI_REG8(0x134A), 0x0C},	// Y_ADDR_END_1B[15:8];
	{CCI_REG8(0x134B), 0x2F},	// Y_ADDR_END_1B[7:0];
	{CCI_REG8(0x134C), 0x10},	// X_OUTPUT_SIZE_1B[15:8];
	{CCI_REG8(0x134D), 0x70},	// X_OUTPUT_SIZE_1B[7:0];
	{CCI_REG8(0x134E), 0x0C},	// Y_OUTPUT_SIZE_1B[15:8];
	{CCI_REG8(0x134F), 0x30},	// Y_OUTPUT_SIZE_1B[7:0];
	{CCI_REG8(0x1401), 0x00},	// -/-/-/-/-/-/SCALING_MODE_1B[1:0];
	{CCI_REG8(0x1403), 0x00},	// -/-/-/-/-/-/SPATIAL_SAMPLING_1B[1:0];
	{CCI_REG8(0x1404), 0x10},	// SCALE_M_1B[7:0];
	{CCI_REG8(0x1408), 0x00},	// DCROP_XOFS_1B[15:8];
	{CCI_REG8(0x1409), 0x00},	// DCROP_XOFS_1B[7:0];
	{CCI_REG8(0x140A), 0x00},	// DCROP_YOFS_1B[15:8];
	{CCI_REG8(0x140B), 0x00},	// DCROP_YOFS_1B[7:0];
	{CCI_REG8(0x140C), 0x10},	// DCROP_WIDTH_1B[15:8];
	{CCI_REG8(0x140D), 0x70},	// DCROP_WIDTH_1B[7:0];
	{CCI_REG8(0x140E), 0x0C},	// DCROP_HIGT_1B[15:8];
	{CCI_REG8(0x140F), 0x30},	// DCROP_HIGT_1B[7:0];
	{CCI_REG8(0x1601), 0x00},	// TEST_PATT_MODE_1B[7:0];
	{CCI_REG8(0x1602), 0x02},	// -/-/-/-/-/-/TEST_DATA_RED_1B[9:8];
	{CCI_REG8(0x1603), 0xC0},	// TEST_DATA_RED_1B[7:0];
	{CCI_REG8(0x1604), 0x02},	// -/-/-/-/-/-/TEST_DATA_GREENR_1B[9:8];
	{CCI_REG8(0x1605), 0xC0},	// TEST_DATA_GREENR_1B[7:0];
	{CCI_REG8(0x1606), 0x02},	// -/-/-/-/-/-/TEST_DATA_BLUE_1B[9:8];
	{CCI_REG8(0x1607), 0xC0},	// TEST_DATA_BLUE_1B[7:0];
	{CCI_REG8(0x1608), 0x02},	// -/-/-/-/-/-/TEST_DATA_GREENB_1B[9:8];
	{CCI_REG8(0x1609), 0xC0},	// TEST_DATA_GREENB_1B[7:0];
	{CCI_REG8(0x160A), 0x00},	// HO_CURS_WIDTH_1B[15:8];
	{CCI_REG8(0x160B), 0x00},	// HO_CURS_WIDTH_1B[7:0];
	{CCI_REG8(0x160C), 0x00},	// HO_CURS_POSITION_1B[15:8];
	{CCI_REG8(0x160D), 0x00},	// HO_CURS_POSITION_1B[7:0];
	{CCI_REG8(0x160E), 0x00},	// VE_CURS_WIDTH_1B[15:8];
	{CCI_REG8(0x160F), 0x00},	// VE_CURS_WIDTH_1B[7:0];
	{CCI_REG8(0x1610), 0x00},	// VE_CURS_POSITION_1B[15:8];
	{CCI_REG8(0x1611), 0x00},	// VE_CURS_POSITION_1B[7:0];
	{CCI_REG8(0x1900), 0x00},	// -/-/-/-/-/-/H_BIN_1B[1:0];
	{CCI_REG8(0x1901), 0x00},	// -/-/-/-/-/-/V_BIN_MODE_1B[1:0];
	{CCI_REG8(0x1902), 0x00},	// -/-/-/-/-/-/BINNING_WEIGHTING_1B[1:0];
	{CCI_REG8(0x3002), 0x0E},	// Reserved ;
	{CCI_REG8(0x301A), 0x66},	// Reserved ;
	{CCI_REG8(0x301B), 0x66},	// Reserved ;
	{CCI_REG8(0x3024), 0x00},	// Reserved ;
	{CCI_REG8(0x3025), 0x7C},	// Reserved ;
	{CCI_REG8(0x3053), 0xE0},	// Reserved ;
	{CCI_REG8(0x305D), 0x10},	// Reserved ;
	{CCI_REG8(0x305E), 0x06},	// Reserved ;
	{CCI_REG8(0x306B), 0x08},	// Reserved ;
	{CCI_REG8(0x3073), 0x26},	// Reserved ;
	{CCI_REG8(0x3074), 0x1A},	// Reserved ;
	{CCI_REG8(0x3075), 0x0F},	// Reserved ;
	{CCI_REG8(0x3076), 0x03},	// Reserved ;
	{CCI_REG8(0x307E), 0x02},	// Reserved ;
	{CCI_REG8(0x308D), 0x03},	// Reserved ;
	{CCI_REG8(0x308E), 0x20},	// Reserved ;
	{CCI_REG8(0x3091), 0x04},	// Reserved ;
	{CCI_REG8(0x3096), 0x75},	// Reserved ;
	{CCI_REG8(0x3097), 0x7E},	// Reserved ;
	{CCI_REG8(0x3098), 0x20},	// Reserved ;
	{CCI_REG8(0x30A0), 0x82},	// Reserved ;
	{CCI_REG8(0x30AB), 0x30},	// Reserved ;
	{CCI_REG8(0x30B0), 0x3E},	// Reserved ;
	{CCI_REG8(0x30B2), 0x1F},	// Reserved ;
	{CCI_REG8(0x30B4), 0x3E},	// Reserved ;
	{CCI_REG8(0x30B6), 0x1F},	// Reserved ;
	{CCI_REG8(0x30CC), 0xC0},	// Reserved ;
	{CCI_REG8(0x30CF), 0x75},	// Reserved ;
	{CCI_REG8(0x30D2), 0xB3},	// Reserved ;
	{CCI_REG8(0x30D5), 0x09},	// Reserved ;
	{CCI_REG8(0x30E5), 0x80},	// Reserved ;
	{CCI_REG8(0x3134), 0x01},	// Reserved ;
	{CCI_REG8(0x314D), 0x80},	// Reserved ;
	{CCI_REG8(0x3165), 0x67},	// Reserved ;
	{CCI_REG8(0x3169), 0x77},	// Reserved ;
	{CCI_REG8(0x316A), 0x77},	// Reserved ;
	{CCI_REG8(0x3173), 0x30},	// Reserved ;
	{CCI_REG8(0x31B1), 0x40},	// Reserved ;
	{CCI_REG8(0x31C1), 0x27},	// Reserved ;
	{CCI_REG8(0x31DB), 0x15},	// Reserved ;
	{CCI_REG8(0x31DC), 0xE0},	// Reserved ;
	{CCI_REG8(0x3204), 0x00},	// Reserved ;
	{CCI_REG8(0x3231), 0x00},	// PWB_RG[7:0];
	{CCI_REG8(0x3232), 0x00},	// PWB_GRG[7:0];
	{CCI_REG8(0x3233), 0x00},	// PWB_GBG[7:0];
	{CCI_REG8(0x3234), 0x00},	// PWB_BG[7:0];
	{CCI_REG8(0x3282), 0xC0},	// ABPC_EN/ABPC_CK_EN/-/-/-/-/-/-;
	{CCI_REG8(0x3284), 0x06},	// Reserved ;
	{CCI_REG8(0x3285), 0x03},	// Reserved ;
	{CCI_REG8(0x3286), 0x02},	// Reserved ;
	{CCI_REG8(0x328A), 0x03},	// Reserved ;
	{CCI_REG8(0x328B), 0x02},	// Reserved ;
	{CCI_REG8(0x3290), 0x20},	// Reserved ;
	{CCI_REG8(0x3294), 0x10},	// Reserved ;
	{CCI_REG8(0x32A8), 0x84},	// CNR : 0x84/0x04 for ON/OFF ;
	{CCI_REG8(0x32B3), 0x10},	// Reserved ;
	{CCI_REG8(0x32B4), 0x1F},	// Reserved ;
	{CCI_REG8(0x32B7), 0x3B},	// Reserved ;
	{CCI_REG8(0x32BB), 0x0F},	// Reserved ;
	{CCI_REG8(0x32BC), 0x0F},	// Reserved ;
	{CCI_REG8(0x32BE), 0x04},	// Reserved ;
	{CCI_REG8(0x32BF), 0x0F},	// Reserved ;
	{CCI_REG8(0x32C0), 0x0F},	// Reserved ;
	{CCI_REG8(0x32C6), 0x50},	// Reserved ;
	{CCI_REG8(0x32C8), 0x0E},	// Reserved ;
	{CCI_REG8(0x32C9), 0x0E},	// Reserved ;
	{CCI_REG8(0x32CA), 0x0E},	// Reserved ;
	{CCI_REG8(0x32CB), 0x0E},	// Reserved ;
	{CCI_REG8(0x32CC), 0x0E},	// Reserved ;
	{CCI_REG8(0x32CD), 0x0E},	// Reserved ;
	{CCI_REG8(0x32CE), 0x08},	// Reserved ;
	{CCI_REG8(0x32CF), 0x08},	// Reserved ;
	{CCI_REG8(0x32D0), 0x08},	// Reserved ;
	{CCI_REG8(0x32D1), 0x0F},	// Reserved ;
	{CCI_REG8(0x32D2), 0x0F},	// Reserved ;
	{CCI_REG8(0x32D3), 0x0F},	// Reserved ;
	{CCI_REG8(0x32D4), 0x08},	// Reserved ;
	{CCI_REG8(0x32D5), 0x08},	// Reserved ;
	{CCI_REG8(0x32D6), 0x08},	// Reserved ;
	{CCI_REG8(0x32DD), 0x02},	// Reserved ;
	{CCI_REG8(0x32E0), 0x20},	// Reserved ;
	{CCI_REG8(0x32E1), 0x20},	// Reserved ;
	{CCI_REG8(0x32E2), 0x20},	// Reserved ;
	{CCI_REG8(0x32F4), 0x03},	// DPC : 0x03/0x01 for ON/OFF ;
	{CCI_REG8(0x32F7), 0x00},	// -/-/-/-/-/-/-/PP_DCROP_SW;
	{CCI_REG8(0x3301), 0x05},	// Reserved ;
	{CCI_REG8(0x3307), 0x37},	// Reserved ;
	{CCI_REG8(0x3308), 0x36},	// Reserved ;
	{CCI_REG8(0x3309), 0x0D},	// Reserved ;
	{CCI_REG8(0x3383), 0x08},	// Reserved ;
	{CCI_REG8(0x3384), 0x10},	// Reserved ;
	{CCI_REG8(0x338C), 0x05},	// Reserved ;
	{CCI_REG8(0x3424), 0x00},	// -/-/-/-/B_TRIG_Z5_X/B_TX_TRIGOPT/B_CLKULPS/B_ESCREQ;
	{CCI_REG8(0x3425), 0x78},	// B_ESCDATA[7:0];
	{CCI_REG8(0x3427), 0x00},	// B_MIPI_CLKVBLK/B_MIPI_CLK_MODE/-/-/B_HS_SR_CNT[1:0]/B_LP_SR_CNT[
	{CCI_REG8(0x3430), 0xA7},	// B_NUMWAKE[7:0];
	{CCI_REG8(0x3431), 0x60},	// B_NUMINIT[7:0];
	{CCI_REG8(0x3432), 0x11},	// -/-/-/B_CLK0_M/-/-/-/B_LNKBTWK_ON;
	{CCI_REG8(0x3439), 0x01},	// THS_PREPARE_LINKOFF;
};

static struct cci_reg_sequence const t4k37_mode_4112x3088_30_regs[] = {
	{CCI_REG8(0x0113), 0x0A},	// CSI_DATA_FORMAT[7:0];
	{T4K37_REG_VT_PIX_CLK_DIV, 0x01},	// -/-/-/-/VT_PIX_CLK_DIV[3:0];
	{T4K37_REG_VT_SYS_CLK_DIV, 0x06},	// -/-/-/-/VT_SYS_CLK_DIV[3:0];
	{T4K37_REG_PRE_PLL_CLK_DIV, 0x03},	// -/-/-/-/-/PRE_PLL_CLK_DIV[2:0];
	{CCI_REG8(0x030B), 0x01},	// -/-/-/-/OP_SYS_CLK_DIV[3:0];
	{CCI_REG8(0x0340), 0x0C},	// FR_LENGTH_LINES[15:8];
	{CCI_REG8(0x0341), 0x48},	// FR_LENGTH_LINES[7:0];
	{CCI_REG8(0x0342), 0x11},	// LINE_LENGTH_PCK[15:8];
	{CCI_REG8(0x0343), 0xE8},	// LINE_LENGTH_PCK[7:0];
	{CCI_REG8(0x0344), 0x00},	// -/-/-/-/H_CROP[3:0];
	{CCI_REG8(0x0346), 0x00},	// Y_ADDR_START[15:8];
	{CCI_REG8(0x0347), 0x00},	// Y_ADDR_START[7:0];
	{CCI_REG8(0x034A), 0x0C},	// Y_ADDR_END[15:8];
	{CCI_REG8(0x034B), 0x2F},	// Y_ADDR_END[7:0];
	{CCI_REG8(0x034C), 0x10},	// X_OUTPUT_SIZE[15:8];
	{CCI_REG8(0x034D), 0x10},	// X_OUTPUT_SIZE[7:0];
	{CCI_REG8(0x034E), 0x0C},	// Y_OUTPUT_SIZE[15:8];
	{CCI_REG8(0x034F), 0x10},	// Y_OUTPUT_SIZE[7:0];
	{CCI_REG8(0x0401), 0x00},	// -/-/-/-/-/-/SCALING_MODE[1:0];
	{CCI_REG8(0x0404), 0x10},	// SCALE_M[7:0];
	{CCI_REG8(0x0408), 0x00},	// DCROP_XOFS[15:8];
	{CCI_REG8(0x0409), 0x30},	// DCROP_XOFS[7:0];
	{CCI_REG8(0x040A), 0x00},	// DCROP_YOFS[15:8];
	{CCI_REG8(0x040B), 0x10},	// DCROP_YOFS[7:0];
	{CCI_REG8(0x040C), 0x10},	// DCROP_WIDTH[15:8];
	{CCI_REG8(0x040D), 0x10},	// DCROP_WIDTH[7:0];
	{CCI_REG8(0x040E), 0x0C},	// DCROP_HIGT[15:8];
	{CCI_REG8(0x040F), 0x10},	// DCROP_HIGT[7:0];
	{CCI_REG8(0x0820), 0x10},	// MSB_LBRATE[31:24];
	{CCI_REG8(0x0821), 0x80},	// MSB_LBRATE[23:16];
	{CCI_REG8(0x0900), 0x00},	// -/-/-/-/-/-/H_BIN[1:0];
	{CCI_REG8(0x0901), 0x00},	// -/-/-/-/-/-/V_BIN_MODE[1:0];
	{CCI_REG8(0x32F7), 0x01},	// -/-/-/-/-/-/-/PP_DCROP_SW;
};

static struct cci_reg_sequence const t4k37_mode_3280x2464_30_regs[] = {
	{CCI_REG8(0x0113), 0x0A},	// CSI_DATA_FORMAT[7:0];
	{T4K37_REG_VT_PIX_CLK_DIV, 0x01},	// -/-/-/-/VT_PIX_CLK_DIV[3:0];
	{T4K37_REG_VT_SYS_CLK_DIV, 0x06},	// -/-/-/-/VT_SYS_CLK_DIV[3:0];
	{T4K37_REG_PRE_PLL_CLK_DIV, 0x03},	// -/-/-/-/-/PRE_PLL_CLK_DIV[2:0];
	{CCI_REG8(0x030B), 0x01},	// -/-/-/-/OP_SYS_CLK_DIV[3:0];
	{CCI_REG8(0x0340), 0x0C},	// FR_LENGTH_LINES[15:8];
	{CCI_REG8(0x0341), 0x48},	// FR_LENGTH_LINES[7:0];
	{CCI_REG8(0x0342), 0x11},	// LINE_LENGTH_PCK[15:8];
	{CCI_REG8(0x0343), 0xE8},	// LINE_LENGTH_PCK[7:0];
	{CCI_REG8(0x0346), 0x00},	// Y_ADDR_START[15:8];
	{CCI_REG8(0x0347), 0x00},	// Y_ADDR_START[7:0];
	{CCI_REG8(0x034A), 0x0C},	// Y_ADDR_END[15:8];
	{CCI_REG8(0x034B), 0x2F},	// Y_ADDR_END[7:0];
	{CCI_REG8(0x034C), 0x0C},	// X_OUTPUT_SIZE[15:8];
	{CCI_REG8(0x034D), 0xD0},	// X_OUTPUT_SIZE[7:0];
	{CCI_REG8(0x034E), 0x09},	// Y_OUTPUT_SIZE[15:8];
	{CCI_REG8(0x034F), 0xA0},	// Y_OUTPUT_SIZE[7:0];
	{CCI_REG8(0x0401), 0x00},	// -/-/-/-/-/-/SCALING_MODE[1:0];
	{CCI_REG8(0x0404), 0x10},	// SCALE_M[7:0];
	{CCI_REG8(0x0408), 0x01},	// DCROP_XOFS[15:8];
	{CCI_REG8(0x0409), 0xD0},	// DCROP_XOFS[7:0];
	{CCI_REG8(0x040A), 0x01},	// DCROP_YOFS[15:8];
	{CCI_REG8(0x040B), 0x48},	// DCROP_YOFS[7:0];
	{CCI_REG8(0x040C), 0x0C},	// DCROP_WIDTH[15:8];
	{CCI_REG8(0x040D), 0xD0},	// DCROP_WIDTH[7:0];
	{CCI_REG8(0x040E), 0x09},	// DCROP_HIGT[15:8];
	{CCI_REG8(0x040F), 0xA0},	// DCROP_HIGT[7:0];
	{CCI_REG8(0x0801), 0x60},	// THS_PREPARE[7:3]/-/-/-;
	{CCI_REG8(0x0820), 0x10},	// MSB_LBRATE[31:24];
	{CCI_REG8(0x0821), 0x59},	// MSB_LBRATE[23:16];
	{CCI_REG8(0x0900), 0x00},	// -/-/-/-/-/-/H_BIN[1:0];
	{CCI_REG8(0x0901), 0x00},	// -/-/-/-/-/-/V_BIN_MODE[1:0];
	{CCI_REG8(0x32F7), 0x01},	// -/-/-/-/-/-/-/PP_DCROP_SW;
	{CCI_REG8(0x3294), 0x10},	// -/-/-/-/-/-/-/-;
	{CCI_REG8(0x3295), 0x20},	// -/-/-/-/-/-/-/-;
	{CCI_REG8(0x3169), 0x77},	// -/-/-/-/-/-/-/-;
	{CCI_REG8(0x316A), 0x77},	// -/-/-/-/-/-/-/-;
};

static struct cci_reg_sequence const t4k37_mode_2064x1552_30_regs[] = {	
	{CCI_REG8(0x0113), 0x0A},	// CSI_DATA_FORMAT[7:0];
	{T4K37_REG_VT_PIX_CLK_DIV, 0x02},	// -/-/-/-/VT_PIX_CLK_DIV[3:0];
	{T4K37_REG_VT_SYS_CLK_DIV, 0x08},	// -/-/-/-/VT_SYS_CLK_DIV[3:0];
	{T4K37_REG_PRE_PLL_CLK_DIV, 0x03},	// -/-/-/-/-/PRE_PLL_CLK_DIV[2:0];
	{CCI_REG8(0x030B), 0x03},	// -/-/-/-/OP_SYS_CLK_DIV[3:0];
	{CCI_REG8(0x0340), 0x06},	// FR_LENGTH_LINES[15:8];
	{CCI_REG8(0x0341), 0x30},	// FR_LENGTH_LINES[7:0];
	{CCI_REG8(0x0342), 0x0D},	// LINE_LENGTH_PCK[15:8];
	{CCI_REG8(0x0343), 0x58},	// LINE_LENGTH_PCK[7:0];
	{CCI_REG8(0x0346), 0x00},	// Y_ADDR_START[15:8];
	{CCI_REG8(0x0347), 0x00},	// Y_ADDR_START[7:0];
	{CCI_REG8(0x034A), 0x0C},	// Y_ADDR_END[15:8];
	{CCI_REG8(0x034B), 0x2F},	// Y_ADDR_END[7:0];
	{CCI_REG8(0x034C), 0x08},	// X_OUTPUT_SIZE[15:8];
	{CCI_REG8(0x034D), 0x10},	// X_OUTPUT_SIZE[7:0];
	{CCI_REG8(0x034E), 0x06},	// Y_OUTPUT_SIZE[15:8];
	{CCI_REG8(0x034F), 0x10},	// Y_OUTPUT_SIZE[7:0];
	{CCI_REG8(0x0401), 0x00},	// -/-/-/-/-/-/SCALING_MODE[1:0];
	{CCI_REG8(0x0404), 0x10},	// SCALE_M[7:0];
	{CCI_REG8(0x0408), 0x00},	// DCROP_XOFS[15:8];
	{CCI_REG8(0x0409), 0x14},	// DCROP_XOFS[7:0];
	{CCI_REG8(0x040A), 0x00},	// DCROP_YOFS[15:8];
	{CCI_REG8(0x040B), 0x04},	// DCROP_YOFS[7:0];
	{CCI_REG8(0x040C), 0x08},	// DCROP_WIDTH[15:8];
	{CCI_REG8(0x040D), 0x10},	// DCROP_WIDTH[7:0];
	{CCI_REG8(0x040E), 0x06},	// DCROP_HIGT[15:8];
	{CCI_REG8(0x040F), 0x10},	// DCROP_HIGT[7:0];
	{CCI_REG8(0x0801), 0x20},	// THS_PREPARE[7:3]/-/-/-;
	{CCI_REG8(0x0820), 0x05},	// MSB_LBRATE[31:24];
	{CCI_REG8(0x0821), 0x73},	// MSB_LBRATE[23:16];
	{CCI_REG8(0x0900), 0x01},	// -/-/-/-/-/-/H_BIN[1:0];
	{CCI_REG8(0x0901), 0x01},	// -/-/-/-/-/-/V_BIN_MODE[1:0];
	{CCI_REG8(0x32F7), 0x01},	// -/-/-/-/-/-/-/PP_DCROP_SW;
	{CCI_REG8(0x3294), 0x10},	// -/-/-/-/-/-/-/-;
	{CCI_REG8(0x3295), 0x20},	// -/-/-/-/-/-/-/-;
	{CCI_REG8(0x3169), 0x77},	// -/-/-/-/-/-/-/-;
	{CCI_REG8(0x316A), 0x77},	// -/-/-/-/-/-/-/-;
};

static struct t4k37_mode t4k37_modes[] = {
	T4K37_MODE(4112, 3088, 30, t4k37_mode_4112x3088_30_regs),
	T4K37_MODE(3280, 2462, 30, t4k37_mode_3280x2464_30_regs),
	T4K37_MODE(2064, 1552, 30, t4k37_mode_2064x1552_30_regs),
};

static inline struct t4k37 *to_t4k37(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct t4k37, sd);
}

static int t4k37_enum_mbus_code(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_mbus_code_enum *code)
{
	struct t4k37 *t4k37 = to_t4k37(sd);

	dev_info(t4k37->dev, "code->index: %d", code->index);
	if (code->index >= ARRAY_SIZE(t4k37_modes)) {
		dev_err(t4k37->dev, "Code index out of range");
		return -EINVAL;
	}

	code->code = t4k37_modes[code->index].code;

	return 0;
}

static int t4k37_enum_frame_size(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_frame_size_enum *fsize)
{
	struct t4k37 *t4k37 = to_t4k37(sd);
	struct t4k37_mode *mode;

	dev_info(t4k37->dev, "fsize->index: %d", fsize->index);
	if (fsize->index >= ARRAY_SIZE(t4k37_modes)) {
		dev_err(t4k37->dev, "Frame size index out of range");
		return -EINVAL;
	}

	mode = &t4k37_modes[fsize->index];

	if (fsize->code != mode->code) {
		dev_err(t4k37->dev, "Invalid frame size mbus code");
		return -EINVAL;
	}

	fsize->min_width = fsize->max_width = mode->width;
	fsize->min_height = fsize->max_height = mode->height;

	return 0;
}

static void t4k37_fill_pad_format(struct t4k37 *t4k37,
				  const struct t4k37_mode *mode,
				  struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;

	fmt->format.code = mode->code;

	fmt->format.field = V4L2_FIELD_NONE;

	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;

	fmt->format.quantization = V4L2_QUANTIZATION_DEFAULT;

	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;
}

static int t4k37_get_format(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *fmt)
{
	struct t4k37 *t4k37 = to_t4k37(sd);

	guard(mutex)(&t4k37->lock);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt->format = *v4l2_subdev_state_get_format(sd_state, fmt->pad);
	else
		t4k37_fill_pad_format(t4k37, t4k37->current_mode, fmt);

	return 0;
}

static int t4k37_calc_pixel_rate(struct t4k37 *t4k37)
{
	/* TODO: Implement pixel rate calculation.
	 * It is different from imx318. Line of 2353 at tsb.c reads this from dev->vt_pix_clk_freq_mhz,
	 * which is calculated by multiplying 4 (number of lanes?) by external clock rate and divided
	 * by div. Div is then calculated by multiplying these values:
	 * pre_pll_clk_div (read from register 0x305)
	 * vt_sys_clk_div (read from register 0x303)
	 * vt_pix_clk_div (read from register 0x301)
	 * It is then multiplied by pll_multiplier, which is read from 0x30E
	 */
	u64 pre_pll_clk_div, vt_sys_clk_div, vt_pix_clk_div;
	u64 pll_mult, div;
	u64 pixel_rate;
	int ret;

	cci_read(t4k37->regmap, T4K37_REG_PRE_PLL_CLK_DIV, &pre_pll_clk_div, &ret);
	if (ret) {
		dev_err(t4k37->dev, "Failed to read pre_pll_clk_div: %pe", ERR_PTR(ret));
		return ret;
	}
	if (pre_pll_clk_div < 1)
		pre_pll_clk_div = 1;

	cci_read(t4k37->regmap, T4K37_REG_VT_SYS_CLK_DIV, &vt_sys_clk_div, &ret);
	if (ret) {
		dev_err(t4k37->dev, "Failed to read vt_sys_clk_div: %pe", ERR_PTR(ret));
		return ret;
	}
	if (vt_sys_clk_div < 1)
		vt_sys_clk_div = 1;

	cci_read(t4k37->regmap, T4K37_REG_VT_PIX_CLK_DIV, &vt_pix_clk_div, &ret);
	if (ret) {
		dev_err(t4k37->dev, "Failed to read vt_pix_clk_div: %pe", ERR_PTR(ret));
		return ret;
	}
	if (vt_pix_clk_div < 1)
		vt_pix_clk_div = 1;

	// FIXME: This reads one byte when it should read two
	cci_read(t4k37->regmap, T4K37_REG_PLL_MULTIPLIER, &pll_mult, &ret);
	if (ret) {
		dev_err(t4k37->dev, "Failed to read pll_multiplier_l: %pe", ERR_PTR(ret));
		return ret;
	}

	div = pre_pll_clk_div * vt_sys_clk_div * vt_pix_clk_div;
	pixel_rate = 4 * T4K37_EXTCLK_RATE;
	do_div(pixel_rate, div);
	pixel_rate *= pll_mult;

	return pixel_rate;
}

static int t4k37_set_format(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *fmt)
{
	struct t4k37 *t4k37 = to_t4k37(sd);
	const struct t4k37_mode *mode;
	
	guard(mutex)(&t4k37->lock);

	mode = v4l2_find_nearest_size(t4k37_modes, ARRAY_SIZE(t4k37_modes),
				       width, height,
				       fmt->format.width, fmt->format.height);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		fmt->format = *v4l2_subdev_state_get_format(sd_state, fmt->pad);
	} else {
		t4k37->current_mode = mode;
		t4k37_fill_pad_format(t4k37, t4k37->current_mode, fmt);

		__v4l2_ctrl_s_ctrl_int64(t4k37->pixel_rate, t4k37_calc_pixel_rate(t4k37));
	}

	return 0;
}

static int t4k37_get_frame_interval(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_interval *fl)
{
	struct t4k37 *t4k37 = to_t4k37(sd);

	guard(mutex)(&t4k37->lock);
	fl->interval = t4k37->frame_interval;

	return 0;
}

static int t4k37_set_frame_interval(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_interval *fi)
{
	struct t4k37 *t4k37 = to_t4k37(sd);
	const struct t4k37_mode *mode;

	if (fi->pad != 0) {
		dev_err(t4k37->dev, "Frame interval pad is 0");
		return -EINVAL;
	}

	guard(mutex)(&t4k37->lock);

	if (t4k37->streaming) {
		dev_err(t4k37->dev, "Cannot set frame interval while streaming");
		return -EBUSY;
	}

	mode = t4k37->current_mode;

	fi->interval.denominator = mode->fps;
	fi->interval.numerator = 1;

	return 0;
}

static int t4k37_start_streaming(struct t4k37 *t4k37)
{
	int ret;
	guard(mutex)(&t4k37->lock);

	ret = cci_multi_reg_write(t4k37->regmap,
			      t4k37_init_settings,
			      ARRAY_SIZE(t4k37_init_settings), NULL);
	if (ret) {
		dev_err(t4k37->dev, "Failed to write init settings: %pe", ERR_PTR(ret));
		return ret;
	}

	cci_write(t4k37->regmap, T4K37_REG_GROUP_PARA_HOLD, T4K37_GROUP_PARA_HOLD_ENABLE, &ret);
	if (ret) {
		dev_err(t4k37->dev, "Failed to enable group parameter hold: %pe", ERR_PTR(ret));
		return ret;
	}

	ret = cci_multi_reg_write(t4k37->regmap,
			      t4k37->current_mode->regs,
			      t4k37->current_mode->num_regs, NULL);
	if (ret) {
		dev_err(t4k37->dev, "Failed to set current mode: %pe", ERR_PTR(ret));
		return ret;
	}

	cci_write(t4k37->regmap, T4K37_REG_GROUP_PARA_HOLD, T4K37_GROUP_PARA_HOLD_DISABLE, &ret);
	if (ret) {
		dev_err(t4k37->dev, "Failed to disable group parameter hold: %pe", ERR_PTR(ret));
		return ret;
	}

	cci_write(t4k37->regmap, T4K37_REG_MODE_SELECT, T4K37_MODE_STREAMING, &ret);
	if (ret) {
		dev_err(t4k37->dev, "Failed to set the streaming mode: %pe", ERR_PTR(ret));
		return ret;
	}

	return 0;
}

static int t4k37_stop_streaming(struct t4k37 *t4k37)
{
	int ret;
	
	guard(mutex)(&t4k37->lock);

	cci_write(t4k37->regmap, T4K37_REG_MODE_SELECT, T4K37_MODE_STANDBY, &ret);
	if (ret)
		dev_err(t4k37->dev, "Failed to stop streaming: %pe", ERR_PTR(ret));

	return ret;
}

static int t4k37_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct t4k37 *t4k37 = to_t4k37(sd);
	int ret;

	if (t4k37->streaming == enable)
		return 0;

	if (enable) {
		ret = pm_runtime_get_sync(t4k37->dev);
		if (ret) {
			pm_runtime_put_noidle(t4k37->dev);
			dev_err(t4k37->dev, "Failed to get_sync at s_stream: %pe", ERR_PTR(ret));
			return ret;
		}

		ret = t4k37_start_streaming(t4k37);
		if (ret) {
			dev_err(t4k37->dev, "Failed to start streaming at s_stream: %pe", ERR_PTR(ret));
			goto err_put;
		}
	} else {
		ret = t4k37_stop_streaming(t4k37);
		if (ret) {
			dev_err(t4k37->dev, "Failed to stop streaming at s_stream: %pe", ERR_PTR(ret));
			goto err_put;
		}
		pm_runtime_put(t4k37->dev);
	}

	t4k37->streaming = enable;
	return 0;
err_put:
	pm_runtime_put(t4k37->dev);
	return ret;
}

static int t4k37_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct t4k37 *t4k37 =
		container_of(ctrl->handler, struct t4k37, ctrl_handler);
	int ret;
	
	if (!pm_runtime_get_if_in_use(t4k37->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_TEST_PATTERN:
		cci_write(t4k37->regmap, T4K37_REG_TEST_PATTERN, t4k37_test_pattern_val[ctrl->val], &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(t4k37->dev);

	return ret;
}

/* TODO: Let's hope it translates one-to-one from tsb.c */
static const struct v4l2_subdev_video_ops t4k37_video_ops = {
	.s_stream = t4k37_s_stream,
};
static const struct v4l2_subdev_pad_ops t4k37_pad_ops = {
	.enum_mbus_code = t4k37_enum_mbus_code,
	.enum_frame_size = t4k37_enum_frame_size,
	.get_frame_interval = t4k37_get_frame_interval,
	.set_frame_interval = t4k37_set_frame_interval,
	.get_fmt = t4k37_get_format,
	.set_fmt = t4k37_set_format,
};

static const struct v4l2_subdev_ops t4k37_subdev_ops = {
	.video = &t4k37_video_ops,
	.pad = &t4k37_pad_ops,
};

static const struct v4l2_ctrl_ops t4k37_ctrl_ops = {
	.s_ctrl = t4k37_set_ctrl,
};

static int t4k37_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct t4k37 *t4k37 = to_t4k37(sd);
	int ret;

	gpiod_set_value_cansleep(t4k37->reset_gpio, 1);
	
	ret = regulator_bulk_enable(T4K37_NUM_SUPPLIES, t4k37->supplies);
	if (ret)
		return ret;

	ret = clk_prepare_enable(t4k37->extclk);
	if (ret)
		goto reg_disable;

	gpiod_set_value_cansleep(t4k37->reset_gpio, 0);

	/* Waiting for device to power up */
	usleep_range(20000, 21000);

	return 0;

reg_disable:
	regulator_bulk_disable(T4K37_NUM_SUPPLIES, t4k37->supplies);

	return ret;
}

static int t4k37_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct t4k37 *t4k37 = to_t4k37(sd);
	int ret;

	gpiod_set_value_cansleep(t4k37->reset_gpio, 1);

	ret = regulator_bulk_disable(T4K37_NUM_SUPPLIES, t4k37->supplies);
	if (ret)
		return ret;

	clk_disable_unprepare(t4k37->extclk);

	return 0;
}

static const struct dev_pm_ops t4k37_pm_ops = {
	SET_RUNTIME_PM_OPS(t4k37_power_off, t4k37_power_on, NULL)
};

static int t4k37_parse_fwnode(struct t4k37 *t4k37)
{
	struct fwnode_handle *fwnode = dev_fwnode(t4k37->dev);
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	int ret;

	endpoint = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!endpoint) {
		dev_err(t4k37->dev, "Failed to get next fwnode endpoint");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &bus_cfg);
	if (ret) {
		dev_err(t4k37->dev, "Failed to parse and allocate v4l2 node: %pe", ERR_PTR(ret));
		return ret;
	}

	t4k37->nlanes = bus_cfg.bus.mipi_csi2.num_data_lanes;
	if (t4k37->nlanes != 4) {
		dev_err(t4k37->dev, "Only 4 data lanes are supported, while %d were provided", t4k37->nlanes);
		return -EINVAL;
	}

	v4l2_fwnode_endpoint_free(&bus_cfg);
	fwnode_handle_put(endpoint);

	return ret;
}

static int t4k37_init_state(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state)
{
	struct t4k37 *t4k37 = to_t4k37(sd);

	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.code = MEDIA_BUS_FMT_SGRBG10_1X10,
			.width = t4k37->current_mode->width,
			.height = t4k37->current_mode->height,
		},
	};

	return t4k37_set_format(sd, sd_state, &fmt);
}

static const struct v4l2_subdev_internal_ops t4k37_internal_ops = {
	.init_state = t4k37_init_state,
};

static int t4k37_probe(struct i2c_client *client)
{
	struct t4k37 *t4k37;
	char *err;
	int ret;

	t4k37 = devm_kzalloc(&client->dev, sizeof(*t4k37), GFP_KERNEL);

	if (!t4k37)
		return -ENOMEM;

	t4k37->dev = &client->dev;
	
	ret = t4k37_parse_fwnode(t4k37);
	if (ret)
		return dev_err_probe(t4k37->dev, ret, "Failed to parse fwnode");

	t4k37->extclk = devm_v4l2_sensor_clk_get(t4k37->dev, NULL);
	if (IS_ERR(t4k37->extclk))
		return dev_err_probe(t4k37->dev, PTR_ERR(t4k37->extclk), "Failed to retrieve clk");

	t4k37->extclk_rate = clk_get_rate(t4k37->extclk);
	if (t4k37->extclk_rate != T4K37_EXTCLK_RATE)
		dev_warn(t4k37->dev, "Mismatched extclk: %d provided while %d expected, continuing anyway", t4k37->extclk_rate, T4K37_EXTCLK_RATE);

	dev_info(t4k37->dev, "extclk rate: %d", t4k37->extclk_rate);
	t4k37->supplies[0].supply = "avdd";
	t4k37->supplies[1].supply = "dvdd";
	t4k37->supplies[2].supply = "vio";
	ret = devm_regulator_bulk_get(t4k37->dev, T4K37_NUM_SUPPLIES, t4k37->supplies);
	if (ret)
		return dev_err_probe(t4k37->dev, ret, "Failed to get regulators");

	t4k37->reset_gpio = devm_gpiod_get(t4k37->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(t4k37->reset_gpio))
		return dev_err_probe(t4k37->dev, PTR_ERR(t4k37->reset_gpio), "Failed to get the reset gpio");

	t4k37->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(t4k37->regmap))
		return dev_err_probe(t4k37->dev, PTR_ERR(t4k37->regmap), "Failed to init regmap");

	t4k37->current_mode = &t4k37_modes[0];
	t4k37->frame_interval.denominator = t4k37->current_mode->fps;
	t4k37->frame_interval.numerator = 1;
	t4k37->streaming = false;

	v4l2_i2c_subdev_init(&t4k37->sd, client, &t4k37_subdev_ops);
	t4k37->sd.internal_ops = &t4k37_internal_ops;

	ret = t4k37_power_on(t4k37->dev);
	if (ret)
		return dev_err_probe(t4k37->dev, ret, "Failed to power on sensor");

	u64 chip_id;
	cci_read(t4k37->regmap, T4K37_REG_CHIP_ID, &chip_id, &ret);
	if (ret)
		return dev_err_probe(t4k37->dev, ret, "Failed to read chip ID");

	dev_info(t4k37->dev, "Chip id: 0x%04llx", chip_id);
	if (chip_id != T4K37_CHIP_ID)
		dev_warn(t4k37->dev, "Mismatched chip id, expected 0x%04x, received 0x%04llx, continuing anyway", T4K37_CHIP_ID, chip_id);
	
	v4l2_ctrl_handler_init(&t4k37->ctrl_handler, 2);
	t4k37->pixel_rate = v4l2_ctrl_new_std(&t4k37->ctrl_handler, &t4k37_ctrl_ops, V4L2_CID_PIXEL_RATE, 0, INT_MAX, 1, t4k37_calc_pixel_rate(t4k37));
	v4l2_ctrl_new_std_menu_items(&t4k37->ctrl_handler, &t4k37_ctrl_ops, V4L2_CID_TEST_PATTERN, ARRAY_SIZE(t4k37_test_pattern_menu) - 1, 0, 0, t4k37_test_pattern_menu);

	ret = t4k37->ctrl_handler.error;
	if (ret) {
		err = "create a v4l2 ctrl handler";
		goto free_ctrl;
	}

	t4k37->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	t4k37->sd.ctrl_handler = &t4k37->ctrl_handler;
	ret = devm_mutex_init(t4k37->dev, &t4k37->lock);
	if (ret) {
		err = "initialize mutex";
		goto free_ctrl;
	}
	t4k37->ctrl_handler.lock = &t4k37->lock;

	t4k37->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	t4k37->pad.flags = MEDIA_PAD_FL_SOURCE;
	t4k37->sd.dev = t4k37->dev;
	t4k37->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&t4k37->sd.entity, 1, &t4k37->pad);
	if (ret) {
		err = "create media entity pads";
		goto free_ctrl;
	}

	ret = v4l2_subdev_init_finalize(&t4k37->sd);
	if (ret) {
		err = "finalize v4l2 subdev";
		goto free_entity;
	}
	
	ret = v4l2_async_register_subdev_sensor(&t4k37->sd);
	if (ret) {
		err = "register a v4l2 subdevice";
		goto free_entity;
	}

	pm_runtime_set_active(t4k37->dev);
	pm_runtime_enable(t4k37->dev);
	pm_runtime_idle(t4k37->dev);

	return 0;

free_entity:
	media_entity_cleanup(&t4k37->sd.entity);
free_ctrl:
	v4l2_ctrl_handler_free(&t4k37->ctrl_handler);
	t4k37_power_off(t4k37->dev);
	return dev_err_probe(t4k37->dev, ret, "Failed to %s", err);
}

static void t4k37_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct t4k37 *t4k37 = to_t4k37(sd);

	v4l2_async_unregister_subdev(&t4k37->sd);
	media_entity_cleanup(&t4k37->sd.entity);
	v4l2_ctrl_handler_free(&t4k37->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(t4k37->dev))
		t4k37_power_off(t4k37->dev);
	pm_runtime_set_suspended(&client->dev);
}

static const struct of_device_id t4k37_of_match[] = {
	{ .compatible = "toshiba,t4k37" },
	{ },
};
MODULE_DEVICE_TABLE(of, t4k37_of_match);

static struct i2c_driver t4k37_i2c_driver = {
	.probe = t4k37_probe,
	.remove = t4k37_remove,
	.driver = {
		.name = "t4k37",
		.pm = &t4k37_pm_ops,
		.of_match_table = t4k37_of_match,
	},
};
module_i2c_driver(t4k37_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Toshiba T4K37 camera sensor driver");
