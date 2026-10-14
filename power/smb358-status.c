// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only status driver for the Summit SMB358 charger
 * (Lenovo PHAB Plus PB1-770N does all charging in the SMB358;
 * the PMIC linear block is dead on this board).
 *
 * Deliberately NEVER writes charger configuration: charging already works
 * stand-alone (bootloader/OTP defaults reach 100%). This driver only reads
 * status registers to feed power_supply ONLINE/STATUS for battery indication.
 *
 * Register map cribbed from the Oculus Go smb358-charger.c reference.
 */
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>

#define SMB358_IRQ_F_REG		0x3a
#define SMB358_IRQ_F_POWER_OK		BIT(0)

#define SMB358_STATUS_C_REG		0x3d
#define SMB358_STATUS_C_CHARGING_MASK	GENMASK(2, 1)
#define SMB358_STATUS_C_HOLD_OFF	BIT(3)
#define SMB358_STATUS_C_CHG_ERR		BIT(6)

struct smb358_status {
	struct i2c_client *client;
	struct power_supply *psy;
	struct mutex lock;
};

static int smb358_read(struct smb358_status *chip, u8 reg, u8 *val)
{
	s32 ret;

	mutex_lock(&chip->lock);
	ret = i2c_smbus_read_byte_data(chip->client, reg);
	mutex_unlock(&chip->lock);
	if (ret < 0)
		return ret;
	*val = ret;
	return 0;
}

/* Input power OK => cable attached and SMB powered from VBUS. */
static int smb358_is_online(struct smb358_status *chip, int *online)
{
	u8 reg;
	int ret;

	ret = smb358_read(chip, SMB358_IRQ_F_REG, &reg);
	if (ret)
		return ret;
	*online = !!(reg & SMB358_IRQ_F_POWER_OK);
	return 0;
}

static int smb358_get_status(struct smb358_status *chip, int *status)
{
	u8 reg;
	int ret, online;

	ret = smb358_is_online(chip, &online);
	if (ret)
		return ret;
	if (!online) {
		*status = POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	}

	ret = smb358_read(chip, SMB358_STATUS_C_REG, &reg);
	if (ret)
		return ret;

	if (reg & SMB358_STATUS_C_CHG_ERR)
		*status = POWER_SUPPLY_STATUS_NOT_CHARGING;
	else if (reg & SMB358_STATUS_C_HOLD_OFF)
		*status = POWER_SUPPLY_STATUS_NOT_CHARGING;
	else if (reg & SMB358_STATUS_C_CHARGING_MASK)
		*status = POWER_SUPPLY_STATUS_CHARGING;
	else
		*status = POWER_SUPPLY_STATUS_DISCHARGING;
	return 0;
}

static int smb358_get_property(struct power_supply *psy,
			       enum power_supply_property psp,
			       union power_supply_propval *val)
{
	struct smb358_status *chip = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		ret = smb358_is_online(chip, &val->intval);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		ret = smb358_get_status(chip, &val->intval);
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

static enum power_supply_property smb358_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
};

static const struct power_supply_desc smb358_psy_desc = {
	.name = "smb358-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = smb358_properties,
	.num_properties = ARRAY_SIZE(smb358_properties),
	.get_property = smb358_get_property,
};

static irqreturn_t smb358_irq(int irq, void *data)
{
	struct smb358_status *chip = data;

	power_supply_changed(chip->psy);
	return IRQ_HANDLED;
}

static int smb358_probe(struct i2c_client *client)
{
	struct power_supply_config psy_cfg = {};
	struct smb358_status *chip;
	u8 reg;
	int ret, irq;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->client = client;
	mutex_init(&chip->lock);

	/* Sanity: STATUS_C must be readable; anything else means no SMB358. */
	ret = smb358_read(chip, SMB358_STATUS_C_REG, &reg);
	if (ret)
		return dev_err_probe(&client->dev, ret, "no SMB358 at 0x%02x\n",
				     client->addr);

	psy_cfg.drv_data = chip;
	psy_cfg.of_node = client->dev.of_node;
	chip->psy = devm_power_supply_register(&client->dev, &smb358_psy_desc,
					       &psy_cfg);
	if (IS_ERR(chip->psy))
		return dev_err_probe(&client->dev, PTR_ERR(chip->psy),
				     "failed to register power supply\n");

	irq = client->irq;
	if (irq > 0) {
		ret = devm_request_threaded_irq(&client->dev, irq, NULL,
						smb358_irq,
						IRQF_TRIGGER_RISING |
						IRQF_TRIGGER_FALLING |
						IRQF_ONESHOT,
						"smb358-stat", chip);
		if (ret)
			dev_warn(&client->dev, "no IRQ (%d), polling only\n",
				 ret);
	}

	dev_info(&client->dev, "SMB358 status monitor ready (STATUS_C=%02x)\n",
		 reg);
	return 0;
}

static const struct of_device_id smb358_of_match[] = {
	{ .compatible = "summit,smb358" },
	{},
};
MODULE_DEVICE_TABLE(of, smb358_of_match);

static const struct i2c_device_id smb358_id[] = {
	{ "smb358", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, smb358_id);

static struct i2c_driver smb358_driver = {
	.driver = {
		.name = "smb358-status",
		.of_match_table = smb358_of_match,
	},
	.probe = smb358_probe,
};
module_i2c_driver(smb358_driver);

MODULE_AUTHOR("phoenix-pmos-port");
MODULE_DESCRIPTION("Read-only Summit SMB358 charger status monitor");
MODULE_LICENSE("GPL");
