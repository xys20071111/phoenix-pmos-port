// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017-18 Linaro Limited

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/regmap.h>

#define PON_SOFT_RB_SPARE		0x8f

/* Local phoenix addition: downstream programs the power-off *type*
 * (shutdown vs reset) into PS_HOLD_RST_CTL before dropping PS_HOLD.
 * Mainline msm-poweroff only drops the hold, so whatever the bootloader
 * left (warm reset on this device) decides the outcome -> poweroff reboots.
 * This handler runs before msm-poweroff's (higher priority) and forces
 * SHUTDOWN semantics. Register layout from downstream qpnp-power-on. */
#define QPNP_PON_REVISION2(base)	((base) + 0x01)
#define QPNP_PON_PS_HOLD_RST_CTL(base)	((base) + 0x5A)
#define QPNP_PON_PS_HOLD_RST_CTL2(base)	((base) + 0x5B)
#define QPNP_PON_RESET_EN		BIT(7)
#define QPNP_PON_POWER_OFF_MASK		0xF
#define PON_REV2_VALUE			0x00
#define PON_POWER_OFF_SHUTDOWN		0x04

#define GEN1_REASON_SHIFT		2
#define GEN2_REASON_SHIFT		1

#define NO_REASON_SHIFT			0

struct pm8916_pon {
	struct device *dev;
	struct regmap *regmap;
	u32 baseaddr;
	struct reboot_mode_driver reboot_mode;
	long reason_shift;
};

static int pm8916_reboot_mode_write(struct reboot_mode_driver *reboot,
				    unsigned int magic)
{
	struct pm8916_pon *pon = container_of
			(reboot, struct pm8916_pon, reboot_mode);
	int ret;

	ret = regmap_update_bits(pon->regmap,
				 pon->baseaddr + PON_SOFT_RB_SPARE,
				 GENMASK(7, pon->reason_shift),
				 magic << pon->reason_shift);
	if (ret < 0)
		dev_err(pon->dev, "update reboot mode bits failed\n");

	return ret;
}

static int pm8916_power_off(struct sys_off_data *data)
{
	struct pm8916_pon *pon = data->cb_data;
	unsigned int rev;
	u16 rst_en_reg;
	int ret;

	ret = regmap_read(pon->regmap, pon->baseaddr + 0x01, &rev);
	if (ret < 0) {
		dev_err(pon->dev, "failed to read PON revision\n");
		return NOTIFY_DONE;
	}

	rst_en_reg = (rev == PON_REV2_VALUE) ?
		QPNP_PON_PS_HOLD_RST_CTL(pon->baseaddr) :
		QPNP_PON_PS_HOLD_RST_CTL2(pon->baseaddr);

	ret = regmap_update_bits(pon->regmap, rst_en_reg,
				 QPNP_PON_RESET_EN, 0);
	if (ret < 0) {
		dev_err(pon->dev, "failed to clear reset enable\n");
		return NOTIFY_DONE;
	}

	/* Needs ~10 sleep-clock cycles; clock is internal so be generous. */
	udelay(500);

	ret = regmap_update_bits(pon->regmap,
				 QPNP_PON_PS_HOLD_RST_CTL(pon->baseaddr),
				 QPNP_PON_POWER_OFF_MASK,
				 PON_POWER_OFF_SHUTDOWN);
	if (ret < 0) {
		dev_err(pon->dev, "failed to set shutdown type\n");
		return NOTIFY_DONE;
	}

	ret = regmap_update_bits(pon->regmap, rst_en_reg,
				 QPNP_PON_RESET_EN, QPNP_PON_RESET_EN);
	if (ret < 0)
		dev_err(pon->dev, "failed to set reset enable\n");

	return NOTIFY_DONE;
}

static int pm8916_pon_probe(struct platform_device *pdev)
{
	struct pm8916_pon *pon;
	long reason_shift;
	int error;

	pon = devm_kzalloc(&pdev->dev, sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;

	pon->dev = &pdev->dev;

	pon->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!pon->regmap) {
		dev_err(&pdev->dev, "failed to locate regmap\n");
		return -ENODEV;
	}

	error = of_property_read_u32(pdev->dev.of_node, "reg",
				     &pon->baseaddr);
	if (error)
		return error;

	reason_shift = (long)of_device_get_match_data(&pdev->dev);

	if (reason_shift != NO_REASON_SHIFT) {
		pon->reboot_mode.dev = &pdev->dev;
		pon->reason_shift = reason_shift;
		pon->reboot_mode.write = pm8916_reboot_mode_write;
		error = devm_reboot_mode_register(&pdev->dev, &pon->reboot_mode);
		if (error) {
			dev_err(&pdev->dev, "can't register reboot mode\n");
			return error;
		}
	}

	platform_set_drvdata(pdev, pon);

	/* Must run before msm-poweroff's DEFAULT-priority handler drops
	 * PS_HOLD, otherwise the bootloader's leftover reset config wins. */
	devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_POWER_OFF,
				      SYS_OFF_PRIO_DEFAULT + 1,
				      pm8916_power_off, pon);

	return devm_of_platform_populate(&pdev->dev);
}

static const struct of_device_id pm8916_pon_id_table[] = {
	{ .compatible = "qcom,pm8916-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8941-pon", .data = (void *)NO_REASON_SHIFT },
	{ .compatible = "qcom,pms405-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8998-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ .compatible = "qcom,pmk8350-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ }
};
MODULE_DEVICE_TABLE(of, pm8916_pon_id_table);

static struct platform_driver pm8916_pon_driver = {
	.probe = pm8916_pon_probe,
	.driver = {
		.name = "pm8916-pon",
		.of_match_table = pm8916_pon_id_table,
	},
};
module_platform_driver(pm8916_pon_driver);

MODULE_DESCRIPTION("pm8916 Power On driver");
MODULE_LICENSE("GPL v2");
