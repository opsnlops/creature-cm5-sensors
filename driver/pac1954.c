// SPDX-License-Identifier: GPL-2.0-only
/*
 * hwmon driver for the Microchip PAC1954 four-channel power monitor.
 *
 * Exposes, per channel:
 *   inN_input / inN_average     bus voltage (VBUS), mV
 *   currN_input / currN_average shunt current (VSENSE / Rshunt), mA
 *   powerN_input                VBUS * VSENSE product (VPOWER), uW
 *
 * Channels are numbered 1-4 in sysfs to match the datasheet (CH1..CH4).
 * Shunt resistors and labels come from firmware child nodes:
 *
 *   power-monitor@10 {
 *       compatible = "microchip,pac1954";
 *       reg = <0x10>;
 *       #address-cells = <1>;
 *       #size-cells = <0>;
 *       channel@1 {
 *           reg = <1>;
 *           shunt-resistor-micro-ohms = <4000>;
 *           label = "Board In";
 *       };
 *   };
 *
 * With no channel nodes all four channels are shown, voltage only.
 * A channel without a shunt value shows voltage only.
 *
 * All channels use unipolar full-scale ranges (the power-on default):
 * VBUS 0..32 V, VSENSE 0..100 mV.
 *
 * Readings come from the chip's readable registers, which a REFRESH_V
 * command latches without resetting the accumulators. The driver sends one
 * at most every PAC1954_UPDATE_INTERVAL and reads every result register in
 * one block transfer.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/unaligned.h>

/* Set by the build (DKMS passes the package version). */
#ifndef PAC1954_VERSION
#define PAC1954_VERSION			"dev"
#endif

#define PAC1954_NUM_CHANNELS		4

#define PAC1954_REG_REFRESH		0x00
#define PAC1954_REG_CTRL		0x01
#define PAC1954_REG_VBUS_CH1		0x07
#define PAC1954_REG_NEG_PWR_FSR		0x1D
#define PAC1954_REG_REFRESH_V		0x1F
#define PAC1954_REG_ALERT_ENABLE	0x49
#define PAC1954_REG_PRODUCT_ID		0xFD
#define PAC1954_REG_MANUFACTURER_ID	0xFE
#define PAC1954_REG_REVISION_ID		0xFF

#define PAC1954_PRODUCT_ID		0x7B
#define PAC1954_MANUFACTURER_ID		0x54

/*
 * CTRL: sample mode 0000 (1024 SPS, adaptive accumulation),
 * GPIO/ALERT2 = 01 (digital input, its power-on default),
 * SLOW/ALERT1 = 00 (ALERT output). The power-on default for that pin is
 * SLOW input; on boards that pull it up the chip drops to 8 SPS.
 * All channels on.
 */
#define PAC1954_CTRL_VALUE		0x0400

/*
 * One block read from VBUS_CH1 (0x07) through VPOWER_CH4 (0x1A):
 * VBUS[4] u16, VSENSE[4] u16, VBUS_AVG[4] u16, VSENSE_AVG[4] u16,
 * VPOWER[4] u32 (30 bits, left-justified).
 */
#define PAC1954_OFF_VBUS		0
#define PAC1954_OFF_VSENSE		8
#define PAC1954_OFF_VBUS_AVG		16
#define PAC1954_OFF_VSENSE_AVG		24
#define PAC1954_OFF_VPOWER		32
#define PAC1954_BLOCK_LEN		48

/* Full-scale ranges, unipolar. */
#define PAC1954_VBUS_FSR_MV		32000ULL
#define PAC1954_VSENSE_FSR_UV		100000ULL
/* VPOWER full scale = 32 V * 0.1 V / Rshunt; numerator in uW * uOhm. */
#define PAC1954_VPOWER_FSR_UW_UOHM	3200000000000ULL

/* Datasheet: wait at least 1 ms after a REFRESH before reading. */
#define PAC1954_REFRESH_DELAY_US	1100
#define PAC1954_UPDATE_INTERVAL		(HZ / 10)

struct pac1954_channel {
	bool present;
	u32 shunt_uohm;		/* 0 = unknown: current and power hidden */
	const char *label;
};

struct pac1954_data {
	struct i2c_client *client;
	struct mutex lock;	/* protects the cache below and chip access */
	struct pac1954_channel ch[PAC1954_NUM_CHANNELS];
	unsigned long last_update;
	bool valid;
	u8 raw[PAC1954_BLOCK_LEN];
};

static int pac1954_write_u16(struct i2c_client *client, u8 reg, u16 val)
{
	u8 buf[3] = { reg, val >> 8, val & 0xff };
	int ret;

	ret = i2c_master_send(client, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	return ret == sizeof(buf) ? 0 : -EIO;
}

static int pac1954_send_cmd(struct i2c_client *client, u8 cmd)
{
	int ret = i2c_smbus_write_byte(client, cmd);

	if (ret)
		return ret;
	usleep_range(PAC1954_REFRESH_DELAY_US, PAC1954_REFRESH_DELAY_US + 500);
	return 0;
}

static int pac1954_read_block(struct i2c_client *client, u8 reg, u8 *buf,
			      u16 len)
{
	struct i2c_msg msgs[2] = {
		{ .addr = client->addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

static int pac1954_update(struct pac1954_data *data)
{
	struct i2c_client *client = data->client;
	int ret;

	if (data->valid &&
	    time_before(jiffies, data->last_update + PAC1954_UPDATE_INTERVAL))
		return 0;

	ret = pac1954_send_cmd(client, PAC1954_REG_REFRESH_V);
	if (ret)
		goto fail;

	ret = pac1954_read_block(client, PAC1954_REG_VBUS_CH1, data->raw,
				 PAC1954_BLOCK_LEN);
	if (ret)
		goto fail;

	data->last_update = jiffies;
	data->valid = true;
	return 0;

fail:
	data->valid = false;
	return ret;
}

static u16 pac1954_raw16(const struct pac1954_data *data, int off, int ch)
{
	return get_unaligned_be16(&data->raw[off + 2 * ch]);
}

static long pac1954_vbus_mv(u16 raw)
{
	return (long)((raw * PAC1954_VBUS_FSR_MV) >> 16);
}

static long pac1954_curr_ma(u16 raw, u32 shunt_uohm)
{
	/* I[mA] = raw / 2^16 * 100000 uV / Rshunt[uOhm] * 1000 */
	return (long)div64_u64((u64)raw * PAC1954_VSENSE_FSR_UV * 1000ULL,
			       (u64)shunt_uohm << 16);
}

static long pac1954_power_uw(u32 raw30, u32 shunt_uohm)
{
	/* P[uW] = raw / 2^30 * 3.2 V^2 / Rshunt */
	return (long)mul_u64_u64_div_u64(raw30, PAC1954_VPOWER_FSR_UW_UOHM,
					 (u64)shunt_uohm << 30);
}

/*
 * Map an hwmon channel index to a chip channel (0..3). "in" has a dummy
 * in0, so in1..in4 are hwmon indexes 1..4; curr and power start at 1 in
 * sysfs but at index 0 here.
 */
static int pac1954_chan(enum hwmon_sensor_types type, int channel)
{
	if (type == hwmon_in)
		channel--;
	if (channel < 0 || channel >= PAC1954_NUM_CHANNELS)
		return -EINVAL;
	return channel;
}

static int pac1954_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct pac1954_data *data = dev_get_drvdata(dev);
	u32 shunt;
	int ret;

	channel = pac1954_chan(type, channel);
	if (channel < 0)
		return channel;
	shunt = data->ch[channel].shunt_uohm;

	mutex_lock(&data->lock);
	ret = pac1954_update(data);
	if (ret)
		goto out;

	switch (type) {
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			*val = pac1954_vbus_mv(pac1954_raw16(data,
					PAC1954_OFF_VBUS, channel));
			break;
		case hwmon_in_average:
			*val = pac1954_vbus_mv(pac1954_raw16(data,
					PAC1954_OFF_VBUS_AVG, channel));
			break;
		default:
			ret = -EOPNOTSUPP;
		}
		break;
	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
			*val = pac1954_curr_ma(pac1954_raw16(data,
					PAC1954_OFF_VSENSE, channel), shunt);
			break;
		case hwmon_curr_average:
			*val = pac1954_curr_ma(pac1954_raw16(data,
					PAC1954_OFF_VSENSE_AVG, channel), shunt);
			break;
		default:
			ret = -EOPNOTSUPP;
		}
		break;
	case hwmon_power:
		if (attr == hwmon_power_input)
			*val = pac1954_power_uw(get_unaligned_be32(
					&data->raw[PAC1954_OFF_VPOWER + 4 * channel]) >> 2,
					shunt);
		else
			ret = -EOPNOTSUPP;
		break;
	default:
		ret = -EOPNOTSUPP;
	}

out:
	mutex_unlock(&data->lock);
	return ret;
}

static int pac1954_read_string(struct device *dev,
			       enum hwmon_sensor_types type, u32 attr,
			       int channel, const char **str)
{
	struct pac1954_data *data = dev_get_drvdata(dev);

	channel = pac1954_chan(type, channel);
	if (channel < 0)
		return channel;
	*str = data->ch[channel].label;
	return 0;
}

static umode_t pac1954_is_visible(const void *drvdata,
				  enum hwmon_sensor_types type, u32 attr,
				  int channel)
{
	const struct pac1954_data *data = drvdata;
	const struct pac1954_channel *ch;

	channel = pac1954_chan(type, channel);
	if (channel < 0)
		return 0;
	ch = &data->ch[channel];

	if (!ch->present)
		return 0;

	if ((attr == hwmon_in_label || attr == hwmon_curr_label ||
	     attr == hwmon_power_label) && !ch->label)
		return 0;

	if ((type == hwmon_curr || type == hwmon_power) && !ch->shunt_uohm)
		return 0;

	return 0444;
}

static const struct hwmon_channel_info * const pac1954_info[] = {
	/*
	 * hwmon numbers "in" channels from 0 and everything else from 1.
	 * Add a dummy in0 (never visible) so in1..in4 line up with
	 * curr1..curr4, power1..power4 and the datasheet's CH1..CH4.
	 * The dummy must be non-zero: a 0 entry ends the config list.
	 * pac1954_is_visible() hides it (as ina3221 does).
	 */
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT | HWMON_I_AVERAGE | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_AVERAGE | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_AVERAGE | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_AVERAGE | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_AVERAGE | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_AVERAGE | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_AVERAGE | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_AVERAGE | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL),
	NULL
};

static const struct hwmon_ops pac1954_hwmon_ops = {
	.is_visible = pac1954_is_visible,
	.read = pac1954_read,
	.read_string = pac1954_read_string,
};

static const struct hwmon_chip_info pac1954_chip_info = {
	.ops = &pac1954_hwmon_ops,
	.info = pac1954_info,
};

static int pac1954_parse_channels(struct device *dev,
				  struct pac1954_data *data)
{
	unsigned int found = 0;
	int i;

	device_for_each_child_node_scoped(dev, child) {
		struct pac1954_channel *ch;
		u32 reg;
		int ret;

		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret)
			return dev_err_probe(dev, ret, "channel node without reg\n");
		if (reg < 1 || reg > PAC1954_NUM_CHANNELS)
			return dev_err_probe(dev, -EINVAL,
					     "channel reg %u out of range 1..4\n", reg);

		ch = &data->ch[reg - 1];
		if (ch->present)
			return dev_err_probe(dev, -EINVAL,
					     "channel %u defined twice\n", reg);
		ch->present = true;
		found++;

		if (!fwnode_property_read_u32(child, "shunt-resistor-micro-ohms",
					      &ch->shunt_uohm) &&
		    !ch->shunt_uohm)
			return dev_err_probe(dev, -EINVAL,
					     "channel %u: shunt of 0 uOhm\n", reg);

		/* The string lives as long as the firmware node. */
		fwnode_property_read_string(child, "label", &ch->label);
	}

	/* No channel nodes: show every channel's voltage. */
	if (!found)
		for (i = 0; i < PAC1954_NUM_CHANNELS; i++)
			data->ch[i].present = true;

	return 0;
}

static int pac1954_init_chip(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	int ret;

	ret = i2c_smbus_read_byte_data(client, PAC1954_REG_PRODUCT_ID);
	if (ret < 0)
		return dev_err_probe(dev, ret, "cannot read product ID\n");
	if (ret != PAC1954_PRODUCT_ID)
		return dev_err_probe(dev, -ENODEV,
				     "product ID 0x%02x, expected 0x%02x\n",
				     ret, PAC1954_PRODUCT_ID);

	ret = i2c_smbus_read_byte_data(client, PAC1954_REG_MANUFACTURER_ID);
	if (ret < 0)
		return dev_err_probe(dev, ret, "cannot read manufacturer ID\n");
	if (ret != PAC1954_MANUFACTURER_ID)
		return dev_err_probe(dev, -ENODEV,
				     "manufacturer ID 0x%02x, expected 0x%02x\n",
				     ret, PAC1954_MANUFACTURER_ID);

	/* No alerts: keeps the open-drain ALERT pins released. */
	ret = i2c_smbus_write_i2c_block_data(client, PAC1954_REG_ALERT_ENABLE,
					     3, (u8[]){ 0, 0, 0 });
	if (ret)
		return dev_err_probe(dev, ret, "cannot write ALERT_ENABLE\n");

	ret = pac1954_write_u16(client, PAC1954_REG_CTRL, PAC1954_CTRL_VALUE);
	if (ret)
		return dev_err_probe(dev, ret, "cannot write CTRL\n");

	ret = pac1954_write_u16(client, PAC1954_REG_NEG_PWR_FSR, 0);
	if (ret)
		return dev_err_probe(dev, ret, "cannot write NEG_PWR_FSR\n");

	/* A full REFRESH applies CTRL and NEG_PWR_FSR. */
	ret = pac1954_send_cmd(client, PAC1954_REG_REFRESH);
	if (ret)
		return dev_err_probe(dev, ret, "REFRESH failed\n");

	ret = i2c_smbus_read_byte_data(client, PAC1954_REG_REVISION_ID);
	dev_info(dev, "PAC1954 rev 0x%02x\n", ret < 0 ? 0 : ret);
	return 0;
}

static int pac1954_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct pac1954_data *data;
	struct device *hwmon_dev;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C |
				     I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_WRITE_I2C_BLOCK))
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->client = client;

	ret = devm_mutex_init(dev, &data->lock);
	if (ret)
		return ret;

	ret = pac1954_parse_channels(dev, data);
	if (ret)
		return ret;

	ret = pac1954_init_chip(client);
	if (ret)
		return ret;

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name,
							 data,
							 &pac1954_chip_info,
							 NULL);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct i2c_device_id pac1954_id[] = {
	{ "pac1954" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pac1954_id);

static const struct of_device_id pac1954_of_match[] = {
	{ .compatible = "microchip,pac1954" },
	{ }
};
MODULE_DEVICE_TABLE(of, pac1954_of_match);

static struct i2c_driver pac1954_driver = {
	.driver = {
		.name = "pac1954",
		.of_match_table = pac1954_of_match,
	},
	.probe = pac1954_probe,
	.id_table = pac1954_id,
};
module_i2c_driver(pac1954_driver);

MODULE_AUTHOR("April White");
MODULE_DESCRIPTION("Microchip PAC1954 power monitor hwmon driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(PAC1954_VERSION);
