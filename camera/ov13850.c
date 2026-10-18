// SPDX-License-Identifier: GPL-2.0
/*
 * OmniVision OV13850 13MP RAW10 sensor driver, mainline style.
 *
 * Rear camera of the Lenovo PHAB Plus (PB1-770N, QCOM msm8939 "phoenix"):
 * module sunny_q13v04b, running on CCI master 0 (I2C adapter 3) and fed to
 * camss csiphy0/csid0 as a 2-lane MIPI CSI-2 source.
 *
 * Register tables:
 *  - global init: the stock vendor table decoded from
 *    /system/vendor/lib/libmmcamera_ov13850.so (8-byte blocks,
 *    [u16 addr][u8 val][u8 pad]), i.e. Lenovo's own tuning for this exact
 *    module. Where the stream-critical window/PLL registers differ from
 *    the Rockchip downstream table, the Rockchip values are used (they
 *    were validated together at 600 Mbps/lane; window registers are
 *    re-written per mode at every stream start anyway).
 *  - per-mode regs: Rockchip downstream values for 2112x1568@30 and
 *    4224x3136@7.5, both at XI = 24 MHz, 2 lanes @ 600 Mbps/lane.
 *
 * Skeleton adapted from the mainline ov5693.c driver. Bring-up note: the
 * DTS reg for this sensor is still a placeholder (the rear module never
 * ACKs while asleep -- standby floats -- so its address cannot be scanned
 * off the bus beforehand), so detect() retries the chip-id read for
 * ~2.5 s, keeping the module awake for an i2ctransfer address scan from
 * userspace: i2ctransfer -f -y 3 w2@<addr> 0x30 0x0a r2 -> 0xd8 0x50.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

/* System control */
#define OV13850_SW_STREAM_REG		CCI_REG8(0x0100)
#define OV13850_START_STREAMING		0x01
#define OV13850_STOP_STREAMING		0x00

/* Chip identification */
#define OV13850_REG_CHIP_ID		CCI_REG16(0x300a)
#define OV13850_CHIP_ID			0xd850
#define OV13850_REG_CHIP_REV		CCI_REG8(0x302a)

/* Exposure: 24-bit big-endian reg at 0x3500, integration time in lines
 * shifted left by 4 (4 fractional bits), as in OV5693. */
#define OV13850_REG_EXPOSURE		CCI_REG24(0x3500)
#define OV13850_EXPOSURE_MASK		GENMASK(19, 4)
#define OV13850_EXPOSURE_SHIFT		4
#define OV13850_EXPOSURE_MIN		4U
#define OV13850_EXPOSURE_STEP		1

/* Analogue gain: 16-bit big-endian reg pair 0x350a/0x350b, low 11 bits.
 * Raw register semantics carried over from the Rockchip driver, 0x10 = 1x. */
#define OV13850_REG_GAIN		CCI_REG16(0x350a)
#define OV13850_REG_GAIN_MASK		GENMASK(10, 0)
#define OV13850_GAIN_MIN		0x10
#define OV13850_GAIN_MAX		0xf8
#define OV13850_GAIN_DEFAULT		0x10
#define OV13850_GAIN_STEP		1

/* Frame timing */
#define OV13850_REG_VTS			CCI_REG16(0x380e)
#define OV13850_VTS_MAX			0x7fff
#define OV13850_VTS_MARGIN		4

/* Test pattern */
#define OV13850_REG_TEST_PATTERN	CCI_REG8(0x5e00)
#define OV13850_TEST_PATTERN_ENABLE	0x80

/* Clocking */
#define OV13850_XVCLK_FREQ		24000000
#define OV13850_LINK_FREQ_300MHZ	300000000

/* pixel rate = link frequency * 2 (DDR) * 2 lanes / 10 bits */
#define OV13850_PIXEL_RATE		(OV13850_LINK_FREQ_300MHZ * 2 * 2 / 10)

#define OV13850_CANONICAL_LANES		2
#define OV13850_NUM_SUPPLIES		3

static const char * const ov13850_supply_names[] = {
	"avdd",		/* analog power  (pm8916_l17, cam_vana 2.85V) */
	"dovdd",	/* I/O power     (pm8916_l6,  cam_vio  1.8V)  */
	"dvdd",		/* digital core  (pm8916_l2,  cam_vdig 1.2V)  */
};

struct ov13850_reg {
	u16 addr;
	u8 val;
};

struct ov13850_mode {
	u32 width;
	u32 height;
	u32 hts;		/* total width in pixels incl. blanking */
	u32 vts;		/* total height in lines incl. blanking */
	u32 exp_def;		/* default integration time, lines */
	const struct ov13850_reg *regs;
	unsigned int nregs;
};

struct ov13850_device {
	struct device *dev;
	struct regmap *regmap;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct clk *xvclk;
	struct gpio_desc *reset;
	struct gpio_desc *powerdown;
	struct regulator_bulk_data supplies[OV13850_NUM_SUPPLIES];
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *analogue_gain;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *test_pattern;
	struct mutex lock;
	const struct ov13850_mode *mode;
};

static inline struct ov13850_device *to_ov13850_sensor(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov13850_device, sd);
}

/*
 * Global init sequence, decoded from the stock vendor sensor library
 * (/system/vendor/lib/libmmcamera_ov13850.so, Lenovo tuning for the
 * sunny_q13v04b module, 24 MHz XI + ~600 Mbps/lane). Consecutive
 * duplicate writes are intentional (as trimmed in vendor flow).
 * Stream-critical window/PLL registers (0x3012, 0x38xx family, 0x4601)
 * carry the Rockchip downstream values so they match the mode tables;
 * the sensor-specific analog blocks (0x3605/0x3702/0x370a/0x372f/0x373x,
 * 0x4020 family, BLC 0x5xxx) stay on Lenovo's values.
 */
static const struct ov13850_reg ov13850_global_init[] = {
	{0x0300, 0x01}, {0x0301, 0x00}, {0x0302, 0x28}, {0x0303, 0x00}, {0x030a, 0x00}, 
	{0x300f, 0x11}, {0x3010, 0x01}, {0x3011, 0x76}, {0x3012, 0x21}, {0x3013, 0x12}, 
	{0x3014, 0x11}, {0x301f, 0x03}, {0x3106, 0x00}, {0x3210, 0x47}, {0x3500, 0x00}, 
	{0x3501, 0x29}, {0x3502, 0xa0}, {0x3506, 0x00}, {0x3507, 0x02}, {0x3508, 0x00}, 
	{0x350a, 0x00}, {0x350b, 0x80}, {0x350e, 0x00}, {0x350f, 0x10}, {0x351a, 0x00}, 
	{0x351b, 0x10}, {0x351c, 0x00}, {0x351d, 0x20}, {0x351e, 0x00}, {0x351f, 0x40}, 
	{0x3520, 0x00}, {0x3521, 0x80}, {0x3600, 0xc0}, {0x3601, 0xfc}, {0x3602, 0x02}, 
	{0x3603, 0x78}, {0x3604, 0xb1}, {0x3605, 0x95}, {0x3606, 0x73}, {0x3607, 0x07}, 
	{0x3609, 0x40}, {0x360a, 0x30}, {0x360b, 0x91}, {0x360c, 0x09}, {0x360f, 0x02}, 
	{0x3611, 0x10}, {0x3612, 0x27}, {0x3613, 0x33}, {0x3615, 0x0c}, {0x3616, 0x0e}, 
	{0x3641, 0x02}, {0x3660, 0x82}, {0x3668, 0x54}, {0x3669, 0x00}, {0x366a, 0x3f}, 
	{0x3667, 0xa0}, {0x3702, 0x5a}, {0x3703, 0x44}, {0x3704, 0x2c}, {0x3705, 0x01}, 
	{0x3706, 0x15}, {0x3707, 0x44}, {0x3708, 0x3c}, {0x3709, 0x1f}, {0x370a, 0xa9}, 
	{0x370b, 0x3c}, {0x3720, 0x55}, {0x3722, 0x84}, {0x3728, 0x40}, {0x372a, 0x00}, 
	{0x372b, 0x02}, {0x372e, 0x22}, {0x372f, 0x88}, {0x3730, 0x00}, {0x3731, 0x00}, 
	{0x3732, 0x00}, {0x3733, 0x00}, {0x3710, 0x28}, {0x3716, 0x03}, {0x3718, 0x10}, 
	{0x3719, 0x0c}, {0x371a, 0x08}, {0x371c, 0xfc}, {0x3748, 0x00}, {0x3760, 0x13}, 
	{0x3761, 0x33}, {0x3762, 0x86}, {0x3763, 0x16}, {0x3767, 0x24}, {0x3768, 0x06}, 
	{0x3769, 0x45}, {0x376c, 0x23}, {0x376f, 0x80}, {0x3773, 0x06}, {0x3710, 0x28}, 
	{0x3716, 0x03}, {0x3718, 0x1c}, {0x3719, 0x0c}, {0x371a, 0x08}, {0x371b, 0x00}, 
	{0x371c, 0xfc}, {0x372a, 0x00}, {0x3730, 0x00}, {0x3731, 0x00}, {0x3732, 0x00}, 
	{0x3733, 0x00}, {0x3738, 0x00}, {0x3739, 0x00}, {0x373a, 0x00}, {0x373b, 0x00}, 
	{0x3740, 0x00}, {0x3741, 0x00}, {0x3742, 0x00}, {0x3743, 0x00}, {0x3748, 0x00}, 
	{0x3749, 0x00}, {0x374a, 0x00}, {0x3780, 0x10}, {0x3781, 0x00}, {0x3782, 0x00}, 
	{0x3d84, 0x00}, {0x3d85, 0x17}, {0x3d8c, 0x73}, {0x3d8d, 0xbf}, {0x3800, 0x00}, 
	{0x3801, 0x08}, {0x3802, 0x00}, {0x3803, 0x04}, {0x3804, 0x10}, {0x3805, 0x97}, 
	{0x3806, 0x0c}, {0x3807, 0x4b}, {0x3808, 0x08}, {0x3809, 0x40}, {0x380a, 0x06}, 
	{0x380b, 0x20}, {0x380c, 0x12}, {0x380d, 0xc0}, {0x380e, 0x06}, {0x380f, 0x80}, 
	{0x3810, 0x00}, {0x3811, 0x04}, {0x3812, 0x00}, {0x3813, 0x02}, {0x3814, 0x31}, 
	{0x3815, 0x31}, {0x3820, 0x02}, {0x3821, 0x06}, {0x3823, 0x00}, {0x3826, 0x00}, 
	{0x3827, 0x02}, {0x3834, 0x00}, {0x3835, 0x1c}, {0x3836, 0x08}, {0x3837, 0x02}, 
	{0x4000, 0xf1}, {0x4001, 0x00}, {0x400b, 0x0c}, {0x4011, 0x00}, {0x401a, 0x00}, 
	{0x401b, 0x00}, {0x401c, 0x00}, {0x401d, 0x00}, {0x4020, 0x00}, {0x4021, 0xe4}, 
	{0x4022, 0x03}, {0x4023, 0x3f}, {0x4024, 0x04}, {0x4025, 0x20}, {0x4026, 0x04}, 
	{0x4027, 0x25}, {0x4028, 0x00}, {0x4029, 0x02}, {0x402a, 0x02}, {0x402b, 0x04}, 
	{0x402c, 0x06}, {0x402d, 0x02}, {0x402e, 0x08}, {0x402f, 0x04}, {0x403d, 0x2c}, 
	{0x403f, 0x7f}, {0x4041, 0x07}, {0x4500, 0x82}, {0x4501, 0x3c}, {0x458b, 0x00}, 
	{0x459c, 0x00}, {0x459d, 0x00}, {0x459e, 0x00}, {0x4601, 0x83}, {0x4602, 0x22}, 
	{0x4603, 0x01}, {0x4837, 0x19}, {0x4d00, 0x04}, {0x4d01, 0x42}, {0x4d02, 0xd1}, 
	{0x4d03, 0x90}, {0x4d04, 0x66}, {0x4d05, 0x65}, {0x4d0b, 0x00}, {0x5001, 0x03}, 
	{0x5002, 0x07}, {0x5003, 0x4f}, {0x5013, 0x40}, {0x501c, 0x00}, {0x501d, 0x10}, 
	{0x5100, 0x30}, {0x5101, 0x02}, {0x5102, 0x01}, {0x5103, 0x01}, {0x5104, 0x02}, 
	{0x5105, 0x01}, {0x5106, 0x01}, {0x5107, 0x00}, {0x5108, 0x00}, {0x5109, 0x00}, 
	{0x510f, 0xfc}, {0x5110, 0xf0}, {0x5111, 0x10}, {0x536d, 0x02}, {0x536e, 0x67}, 
	{0x536f, 0x01}, {0x5370, 0x4c}, {0x5400, 0x00}, {0x5400, 0x00}, {0x5401, 0x51}, 
	{0x5402, 0x00}, {0x5403, 0x00}, {0x5404, 0x00}, {0x5405, 0x20}, {0x540c, 0x05}, 
	{0x5501, 0x00}, {0x5b00, 0x00}, {0x5b01, 0x00}, {0x5b02, 0x01}, {0x5b03, 0xff}, 
};

/* 2112x1568@30: window/dummy/PLL per-mode regs (Rockchip downstream). */
static const struct ov13850_reg ov13850_2112x1568_regs[] = {
	{0x3612, 0x27}, {0x370a, 0x26}, {0x372a, 0x00}, {0x372f, 0x90}, {0x3801, 0x08}, 
	{0x3805, 0x97}, {0x3807, 0x4b}, {0x3808, 0x08}, {0x3809, 0x40}, {0x380a, 0x06}, 
	{0x380b, 0x20}, {0x380c, 0x12}, {0x380d, 0xc0}, {0x380e, 0x06}, {0x380f, 0x80}, 
	{0x3813, 0x02}, {0x3814, 0x31}, {0x3815, 0x31}, {0x3820, 0x02}, {0x3821, 0x05}, 
	{0x3836, 0x08}, {0x3837, 0x02}, {0x4601, 0x04}, {0x4603, 0x00}, {0x4020, 0x00}, 
	{0x4021, 0xe4}, {0x4022, 0x07}, {0x4023, 0x5f}, {0x4024, 0x08}, {0x4025, 0x44}, 
};

/* 4224x3136@7.5 full-res mode regs (Rockchip downstream). */
static const struct ov13850_reg ov13850_4224x3136_regs[] = {
	{0x3612, 0x2f}, {0x370a, 0x24}, {0x372a, 0x04}, {0x372f, 0xa0}, {0x3801, 0x0c}, 
	{0x3805, 0x93}, {0x3807, 0x4b}, {0x3808, 0x10}, {0x3809, 0x80}, {0x380a, 0x0c}, 
	{0x380b, 0x40}, {0x380e, 0x0d}, {0x380f, 0x00}, {0x3813, 0x04}, {0x3814, 0x11}, 
	{0x3815, 0x11}, {0x3820, 0x00}, {0x3821, 0x04}, {0x3836, 0x04}, {0x3837, 0x01}, 
	{0x4601, 0x87}, {0x4603, 0x01}, {0x4020, 0x02}, {0x4021, 0x4c}, {0x4022, 0x0e}, 
	{0x4023, 0x37}, {0x4024, 0x0f}, {0x4025, 0x1c}, {0x4026, 0x0f}, {0x4027, 0x1f}, 
	{0x4603, 0x00}, {0x5401, 0x71}, {0x5405, 0x80}, 
};

static const struct ov13850_mode ov13850_modes[] = {
	{
		.width = 2112,
		.height = 1568,
		.hts = 0x12c0,
		.vts = 0x0680,
		.exp_def = 0x600,
		.regs = ov13850_2112x1568_regs,
		.nregs = ARRAY_SIZE(ov13850_2112x1568_regs),
	}, {
		.width = 4224,
		.height = 3136,
		.hts = 0x12c0,
		.vts = 0x0d00,
		.exp_def = 0x600,
		.regs = ov13850_4224x3136_regs,
		.nregs = ARRAY_SIZE(ov13850_4224x3136_regs),
	},
};


static const s64 ov13850_link_freq_menu_items[] = {
	OV13850_LINK_FREQ_300MHZ,
};

static const char * const ov13850_test_pattern_menu[] = {
	"Disabled",
	"Vertical Color Bar Type 1",
	"Vertical Color Bar Type 2",
	"Vertical Color Bar Type 3",
	"Vertical Color Bar Type 4",
};

/* Register accesses */

static int ov13850_write_regs(struct ov13850_device *ov13850,
			      const struct ov13850_reg *regs, unsigned int n)
{
	unsigned int i;
	int ret = 0;

	for (i = 0; i < n; i++)
		cci_write(ov13850->regmap, CCI_REG8(regs[i].addr),
			  regs[i].val, &ret);

	return ret;
}

/* Ctrl application helpers (each fills ret, aborting early on error) */

static int ov13850_exposure_configure(struct ov13850_device *ov13850,
				      unsigned int val)
{
	int ret = 0;
	u64 exposure;

	cci_read(ov13850->regmap, OV13850_REG_EXPOSURE, &exposure, &ret);
	exposure &= ~OV13850_EXPOSURE_MASK;
	exposure |= ((u64)val << OV13850_EXPOSURE_SHIFT) & OV13850_EXPOSURE_MASK;
	cci_write(ov13850->regmap, OV13850_REG_EXPOSURE, exposure, &ret);

	return ret;
}

static int ov13850_gain_configure(struct ov13850_device *ov13850,
				  unsigned int val)
{
	int ret = 0;

	cci_update_bits(ov13850->regmap, OV13850_REG_GAIN,
			OV13850_REG_GAIN_MASK, val, &ret);

	return ret;
}

static int ov13850_vts_configure(struct ov13850_device *ov13850,
				 unsigned int val)
{
	int ret = 0;

	cci_write(ov13850->regmap, OV13850_REG_VTS,
		  val + ov13850->mode->height, &ret);

	return ret;
}

static int ov13850_test_pattern_configure(struct ov13850_device *ov13850,
					  unsigned int val)
{
	int ret = 0;

	cci_write(ov13850->regmap, OV13850_REG_TEST_PATTERN,
		  val ? OV13850_TEST_PATTERN_ENABLE : 0, &ret);

	return ret;
}

/* Ctrl handler callback */

static int ov13850_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov13850_device *ov13850 =
		container_of(ctrl->handler, struct ov13850_device,
			     ctrl_handler);
	int ret = 0;

	/* Propagate VBLANK changes to the exposure range */
	if (ctrl->id == V4L2_CID_VBLANK) {
		int exposure_max = ov13850->mode->height + ctrl->val -
				   OV13850_VTS_MARGIN;

		__v4l2_ctrl_modify_range(ov13850->exposure,
					 ov13850->exposure->minimum,
					 exposure_max,
					 ov13850->exposure->step,
					 min(ov13850->exposure->val,
					     exposure_max));
	}

	/* Only apply to the hardware while the sensor is powered */
	if (!pm_runtime_get_if_in_use(ov13850->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = ov13850_exposure_configure(ov13850, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = ov13850_gain_configure(ov13850, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		ret = ov13850_vts_configure(ov13850, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = ov13850_test_pattern_configure(ov13850, ctrl->val);
		break;
	default:
		ret = -EINVAL;
	}

	pm_runtime_put(ov13850->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ov13850_ctrl_ops = {
	.s_ctrl = ov13850_s_ctrl,
};

static int ov13850_initialize_controls(struct ov13850_device *ov13850)
{
	const struct v4l2_ctrl_ops *ops = &ov13850_ctrl_ops;
	const struct ov13850_mode *mode = ov13850->mode;
	struct v4l2_ctrl_handler *handler = &ov13850->ctrl_handler;
	struct v4l2_fwnode_device_properties props;
	s64 exposure_max;
	u32 hblank, vblank_max, vblank_def;
	int ret;

	ret = v4l2_ctrl_handler_init(handler, 10);
	if (ret)
		return ret;

	v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ, 0, 0,
			       ov13850_link_freq_menu_items);
	if (handler->error)
		goto err_free_handler;

	v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE, 0,
			  OV13850_PIXEL_RATE, 1, OV13850_PIXEL_RATE);
	if (handler->error)
		goto err_free_handler;

	/* Fixed HTS; per-MIPI-etiquette hblank is a read-only value */
	hblank = mode->hts - mode->width;
	ov13850->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
					    hblank, hblank, 1, hblank);
	if (ov13850->hblank)
		ov13850->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank_max = OV13850_VTS_MAX - mode->height;
	vblank_def = mode->vts - mode->height;
	ov13850->vblank = v4l2_ctrl_new_std(handler, ops, V4L2_CID_VBLANK,
					    vblank_def, vblank_max, 1,
					    vblank_def);

	exposure_max = mode->vts - OV13850_VTS_MARGIN;
	ov13850->exposure = v4l2_ctrl_new_std(handler, ops,
					      V4L2_CID_EXPOSURE,
					      OV13850_EXPOSURE_MIN,
					      exposure_max,
					      OV13850_EXPOSURE_STEP,
					      mode->exp_def);

	ov13850->analogue_gain = v4l2_ctrl_new_std(handler, ops,
						   V4L2_CID_ANALOGUE_GAIN,
						   OV13850_GAIN_MIN,
						   OV13850_GAIN_MAX,
						   OV13850_GAIN_STEP,
						   OV13850_GAIN_DEFAULT);

	ov13850->test_pattern = v4l2_ctrl_new_std_menu_items(handler, ops,
					V4L2_CID_TEST_PATTERN,
					ARRAY_SIZE(ov13850_test_pattern_menu) - 1,
					0, 0, ov13850_test_pattern_menu);

	if (handler->error) {
		ret = handler->error;
		dev_err(ov13850->dev, "Error initialising v4l2 ctrls\n");
		goto err_free_handler;
	}

	/* Set properties from fwnode (rotation, orientation, ...) */
	ret = v4l2_fwnode_device_parse(ov13850->dev, &props);
	if (ret)
		goto err_free_handler;

	ret = v4l2_ctrl_new_fwnode_properties(handler, ops, &props);
	if (ret)
		goto err_free_handler;

	handler->lock = &ov13850->lock;
	ov13850->sd.ctrl_handler = handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);
	return ret;
}

/* Pad and video ops */

static int ov13850_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	/* Only a single mbus format is supported */
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;

	return 0;
}

static int ov13850_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	unsigned int i;

	if (fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ov13850_modes); i++) {
		if (fse->index == i) {
			fse->min_width = ov13850_modes[i].width;
			fse->max_width = ov13850_modes[i].width;
			fse->min_height = ov13850_modes[i].height;
			fse->max_height = ov13850_modes[i].height;
			return 0;
		}
	}

	return -EINVAL;
}

static const struct ov13850_mode *
ov13850_find_best_fit(struct v4l2_subdev_format *fmt)
{
	const struct ov13850_mode *best = &ov13850_modes[0];
	unsigned int dist, best_dist = UINT_MAX;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ov13850_modes); i++) {
		const struct ov13850_mode *m = &ov13850_modes[i];

		dist = abs(m->width - fmt->format.width) +
		       abs(m->height - fmt->format.height);
		if (dist < best_dist) {
			best_dist = dist;
			best = m;
		}
	}

	return best;
}

static int ov13850_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
{
	struct ov13850_device *ov13850 = to_ov13850_sensor(sd);

	if (fmt->pad != 0)
		return -EINVAL;

	mutex_lock(&ov13850->lock);

	fmt->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->format.width = ov13850->mode->width;
	fmt->format.height = ov13850->mode->height;
	fmt->format.field = V4L2_FIELD_NONE;

	mutex_unlock(&ov13850->lock);

	return 0;
}

static int ov13850_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
{
	struct ov13850_device *ov13850 = to_ov13850_sensor(sd);
	const struct ov13850_mode *mode;
	s64 exposure_max;
	u32 vblank_def;

	if (fmt->pad != 0)
		return -EINVAL;

	mutex_lock(&ov13850->lock);

	mode = ov13850_find_best_fit(fmt);
	fmt->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;

	if (ov13850->mode != mode) {
		ov13850->mode = mode;

		/* Adjust exposure/vblank ranges for the new mode */
		exposure_max = mode->vts - OV13850_VTS_MARGIN;
		__v4l2_ctrl_modify_range(ov13850->exposure,
					 ov13850->exposure->minimum,
					 exposure_max,
					 ov13850->exposure->step,
					 min(ov13850->exposure->val,
					     exposure_max));
		vblank_def = mode->vts - mode->height;
		__v4l2_ctrl_modify_range(ov13850->vblank,
					 ov13850->vblank->minimum,
					 ov13850->vblank->maximum,
					 ov13850->vblank->step,
					 vblank_def);
	}

	mutex_unlock(&ov13850->lock);

	return 0;
}

static int ov13850_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov13850_device *ov13850 = to_ov13850_sensor(sd);
	int ret;

	if (enable) {
		ret = pm_runtime_resume_and_get(ov13850->dev);
		if (ret)
			return ret;

		mutex_lock(&ov13850->lock);

		/* Window/timing/binning for the selected mode, then ctrls */
		ret = ov13850_write_regs(ov13850, ov13850->mode->regs,
					 ov13850->mode->nregs);
		if (!ret)
			ret = __v4l2_ctrl_handler_setup(&ov13850->ctrl_handler);
		if (!ret)
			cci_write(ov13850->regmap, OV13850_SW_STREAM_REG,
				  OV13850_START_STREAMING, &ret);

		mutex_unlock(&ov13850->lock);

		if (ret) {
			pm_runtime_put_noidle(ov13850->dev);
			return ret;
		}
	} else {
		mutex_lock(&ov13850->lock);

		cci_write(ov13850->regmap, OV13850_SW_STREAM_REG,
			  OV13850_STOP_STREAMING, &ret);

		mutex_unlock(&ov13850->lock);

		pm_runtime_put(ov13850->dev);
	}

	return ret;
}

static const struct v4l2_subdev_video_ops ov13850_video_ops = {
	.s_stream = ov13850_s_stream,
};

static const struct v4l2_subdev_pad_ops ov13850_pad_ops = {
	.enum_mbus_code = ov13850_enum_mbus_code,
	.enum_frame_size = ov13850_enum_frame_size,
	.get_fmt = ov13850_get_fmt,
	.set_fmt = ov13850_set_fmt,
};

static const struct v4l2_subdev_ops ov13850_ops = {
	.video = &ov13850_video_ops,
	.pad = &ov13850_pad_ops,
};

/* Power management */

static int ov13850_sensor_powerdown(struct ov13850_device *ov13850)
{
	gpiod_set_value_cansleep(ov13850->reset, 1);
	gpiod_set_value_cansleep(ov13850->powerdown, 1);

	regulator_bulk_disable(OV13850_NUM_SUPPLIES, ov13850->supplies);

	clk_disable_unprepare(ov13850->xvclk);

	return 0;
}

static int ov13850_sensor_init(struct ov13850_device *ov13850)
{
	return ov13850_write_regs(ov13850, ov13850_global_init,
				  ARRAY_SIZE(ov13850_global_init));
}

static int ov13850_sensor_powerup(struct ov13850_device *ov13850)
{
	int ret;

	gpiod_set_value_cansleep(ov13850->reset, 1);
	gpiod_set_value_cansleep(ov13850->powerdown, 1);

	ret = clk_prepare_enable(ov13850->xvclk);
	if (ret) {
		dev_err(ov13850->dev, "Failed to enable xvclk\n");
		goto fail_power;
	}

	ret = regulator_bulk_enable(OV13850_NUM_SUPPLIES, ov13850->supplies);
	if (ret) {
		dev_err(ov13850->dev, "Failed to enable regulators\n");
		goto fail_power;
	}

	gpiod_set_value_cansleep(ov13850->powerdown, 0);
	gpiod_set_value_cansleep(ov13850->reset, 0);

	usleep_range(5000, 7500);

	return 0;

fail_power:
	ov13850_sensor_powerdown(ov13850);
	return ret;
}

static int __maybe_unused ov13850_sensor_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov13850_device *ov13850 = to_ov13850_sensor(sd);

	ov13850_sensor_powerdown(ov13850);

	return 0;
}

static int __maybe_unused ov13850_sensor_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov13850_device *ov13850 = to_ov13850_sensor(sd);
	int ret;

	ret = ov13850_sensor_powerup(ov13850);
	if (ret)
		return ret;

	/* Registers are lost across the power cycle: redo the init table. */
	ret = ov13850_sensor_init(ov13850);
	if (ret) {
		dev_err(ov13850->dev, "sensor init failed on resume: %d\n",
			ret);
		ov13850_sensor_powerdown(ov13850);
	}

	return ret;
}

/* Detection: chip id with a bring-up retry window */

static int ov13850_detect(struct ov13850_device *ov13850)
{
	u64 id = 0;
	int ret = 0;
	unsigned int i;

	/*
	 * Bring-up window: the DT slave address of this module is still a
	 * placeholder, so keep the sensor awake for 2.5 s while the userspace
	 * can scan the shared CCI bus (i2ctransfer -f -y 3 ... w2@0x30 0x0a
	 * r2 == 0xd8 0x50 marks the winner). A successful read short-circuits
	 * the delay; failures retry every 100 ms.
	 */
	for (i = 0; i < 25; i++) {
		ret = cci_read(ov13850->regmap, OV13850_REG_CHIP_ID, &id, NULL);
		if (!ret && id == OV13850_CHIP_ID)
			return 0;
		if (!ret)
			dev_info(ov13850->dev,
				 "ACK with unexpected ID 0x%04llx, retrying\n",
				 id);
		usleep_range(100000, 120000);
	}

	if (!ret)
		return dev_err_probe(ov13850->dev, -ENODEV,
				     "some device at this address (ID 0x%04llx != 0xd850); scan the bus\n",
				     id);

	return dev_err_probe(ov13850->dev, ret,
			     "chip id read failed after 2.5s window\n");

	return 0;
}

/* Probe / remove */

static int ov13850_check_hwcfg(struct ov13850_device *ov13850)
{
	struct fwnode_handle *fwnode = dev_fwnode(ov13850->dev);
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *endpoint;
	unsigned int i;
	int ret;

	endpoint = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!endpoint)
		return dev_err_probe(ov13850->dev, -EPROBE_DEFER,
				     "waiting for camss endpoint\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &bus_cfg);
	fwnode_handle_put(endpoint);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != OV13850_CANONICAL_LANES) {
		dev_err(ov13850->dev, "only a 2-lane CSI2 config is supported\n");
		ret = -EINVAL;
		goto out_free_bus_cfg;
	}

	if (!bus_cfg.nr_of_link_frequencies) {
		dev_err(ov13850->dev, "no link frequencies defined\n");
		ret = -EINVAL;
		goto out_free_bus_cfg;
	}

	for (i = 0; i < bus_cfg.nr_of_link_frequencies; i++)
		if (bus_cfg.link_frequencies[i] == OV13850_LINK_FREQ_300MHZ)
			break;

	if (i == bus_cfg.nr_of_link_frequencies) {
		dev_err(ov13850->dev,
			"link freq %u not supported by the driver\n",
			OV13850_LINK_FREQ_300MHZ);
		ret = -EINVAL;
	}

out_free_bus_cfg:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int ov13850_configure_gpios(struct ov13850_device *ov13850)
{
	ov13850->reset = devm_gpiod_get_optional(ov13850->dev, "reset",
						 GPIOD_OUT_HIGH);
	if (IS_ERR(ov13850->reset)) {
		dev_err(ov13850->dev, "Error fetching reset GPIO\n");
		return PTR_ERR(ov13850->reset);
	}

	ov13850->powerdown = devm_gpiod_get_optional(ov13850->dev, "powerdown",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(ov13850->powerdown)) {
		dev_err(ov13850->dev, "Error fetching powerdown GPIO\n");
		return PTR_ERR(ov13850->powerdown);
	}

	return 0;
}

static int ov13850_get_regulators(struct ov13850_device *ov13850)
{
	unsigned int i;

	for (i = 0; i < OV13850_NUM_SUPPLIES; i++)
		ov13850->supplies[i].supply = ov13850_supply_names[i];

	return devm_regulator_bulk_get(ov13850->dev, OV13850_NUM_SUPPLIES,
				       ov13850->supplies);
}

static int ov13850_probe(struct i2c_client *client)
{
	struct ov13850_device *ov13850;
	u32 xvclk_rate;
	u64 chip_rev = 0;
	int ret;

	ov13850 = devm_kzalloc(&client->dev, sizeof(*ov13850), GFP_KERNEL);
	if (!ov13850)
		return -ENOMEM;

	ov13850->dev = &client->dev;
	ov13850->mode = ov13850_modes;

	ov13850->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(ov13850->regmap))
		return PTR_ERR(ov13850->regmap);

	ret = ov13850_check_hwcfg(ov13850);
	if (ret)
		return ret;

	mutex_init(&ov13850->lock);

	v4l2_i2c_subdev_init(&ov13850->sd, client, &ov13850_ops);

	ov13850->xvclk = devm_clk_get_optional(&client->dev, "xvclk");
	if (IS_ERR(ov13850->xvclk))
		return dev_err_probe(&client->dev, PTR_ERR(ov13850->xvclk),
				     "failed to get xvclk: %ld\n",
				     PTR_ERR(ov13850->xvclk));

	if (ov13850->xvclk)
		xvclk_rate = clk_get_rate(ov13850->xvclk);
	else
		xvclk_rate = OV13850_XVCLK_FREQ;

	if (xvclk_rate != OV13850_XVCLK_FREQ)
		dev_warn(&client->dev, "Found clk freq %u, expected %u\n",
			 xvclk_rate, OV13850_XVCLK_FREQ);

	ret = ov13850_configure_gpios(ov13850);
	if (ret)
		return ret;

	ret = ov13850_get_regulators(ov13850);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Error fetching regulators\n");

	ov13850->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ov13850->pad.flags = MEDIA_PAD_FL_SOURCE;
	ov13850->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = ov13850_initialize_controls(ov13850);
	if (ret)
		return ret;

	ret = media_entity_pads_init(&ov13850->sd.entity, 1, &ov13850->pad);
	if (ret)
		goto err_ctrl_handler_free;

	/*
	 * We need the driver to work in the event that pm runtime is
	 * disabled in the kernel, so power up and verify the chip now.
	 * In the event that runtime pm is disabled this will leave the
	 * chip on, so streaming will work.
	 */

	ret = ov13850_sensor_powerup(ov13850);
	if (ret)
		goto err_media_entity_cleanup;

	ret = ov13850_detect(ov13850);
	if (ret)
		goto err_powerdown;

	/* Revision info for bring-up diagnostics (r2a/r1a table choice). */
	cci_read(ov13850->regmap, OV13850_REG_CHIP_REV, &chip_rev, &ret);
	if (ret)
		dev_warn(ov13850->dev, "chip rev read failed: %d\n", ret);
	else
		dev_info(ov13850->dev, "OV13850 rev 0x%02llx detected\n",
			 chip_rev);

	ret = ov13850_sensor_init(ov13850);
	if (ret) {
		dev_err(ov13850->dev, "global init registers failed: %d\n",
			ret);
		goto err_powerdown;
	}

	pm_runtime_set_active(&client->dev);
	pm_runtime_get_noresume(&client->dev);
	pm_runtime_enable(&client->dev);

	ret = v4l2_async_register_subdev_sensor(&ov13850->sd);
	if (ret) {
		dev_err(&client->dev, "failed to register V4L2 subdev: %d",
			ret);
		goto err_pm_runtime;
	}

	pm_runtime_set_autosuspend_delay(&client->dev, 1000);
	pm_runtime_use_autosuspend(&client->dev);
	pm_runtime_put_autosuspend(&client->dev);

	return 0;

err_pm_runtime:
	pm_runtime_disable(&client->dev);
	pm_runtime_put_noidle(&client->dev);
err_powerdown:
	ov13850_sensor_powerdown(ov13850);
err_media_entity_cleanup:
	media_entity_cleanup(&ov13850->sd.entity);
	v4l2_ctrl_handler_free(&ov13850->ctrl_handler);

	return ret;

err_ctrl_handler_free:
	v4l2_ctrl_handler_free(&ov13850->ctrl_handler);

	return ret;
}

static void ov13850_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov13850_device *ov13850 = to_ov13850_sensor(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&ov13850->sd.entity);
	v4l2_ctrl_handler_free(&ov13850->ctrl_handler);
	mutex_destroy(&ov13850->lock);

	/*
	 * Disable runtime PM. In case runtime PM is disabled in the
	 * kernel, make sure to turn power off manually.
	 */
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		ov13850_sensor_powerdown(ov13850);
	pm_runtime_set_suspended(&client->dev);
}

static const struct dev_pm_ops ov13850_pm_ops = {
	SET_RUNTIME_PM_OPS(ov13850_sensor_suspend, ov13850_sensor_resume, NULL)
};

static const struct of_device_id ov13850_of_match[] = {
	{ .compatible = "ovti,ov13850", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ov13850_of_match);

static struct i2c_driver ov13850_driver = {
	.driver = {
		.name = "ov13850",
		.of_match_table = ov13850_of_match,
		.pm = &ov13850_pm_ops,
	},
	.probe = ov13850_probe,
	.remove = ov13850_remove,
};
module_i2c_driver(ov13850_driver);

MODULE_DESCRIPTION("OmniVision OV13850 sensor driver (Lenovo PB1-770N rear camera)");
MODULE_LICENSE("GPL");
