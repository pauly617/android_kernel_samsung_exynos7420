/* abov_touchkey.c -- Linux driver for abov chip as touchkey
 *
 * Copyright (C) 2013 Samsung Electronics Co.Ltd
 * Author: Junkyeong Kim <jk0430.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <asm/unaligned.h>
#include <linux/regulator/consumer.h>
#include <linux/wakelock.h>
#include <linux/pinctrl/consumer.h>
#include <linux/hall.h>

#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#endif
#undef CONFIG_INPUT_BOOSTER
#ifdef CONFIG_INPUT_BOOSTER
#include <linux/input/input_booster.h>
#endif

/* registers */
#define ABOV_BTNSTATUS		0x00
#define ABOV_FW_VER		0x01
#define ABOV_PCB_VER		0x02
#define ABOV_COMMAND		0x03
#define ABOV_THRESHOLD		0x04
#define ABOV_SETIDAC		0x06
#define ABOV_DIFFDATA		0x0A
#define ABOV_KEY_VALUE		0x10
#define ABOV_GLOVE		0x12
#define ABOV_VENDORID		0x13
#define ABOV_MODELNO		0x14
#define ABOV_RAWDATA		0x15

/* command */
#define CMD_LED_ON		0x10
#define CMD_LED_OFF		0x20
#define CMD_DATA_UPDATE		0x40
#define CMD_LED_CTRL_ON		0x60
#define CMD_LED_CTRL_OFF	0x70
#define CMD_STOP_MODE		0x80
#define CMD_GLOVE_ON		0x20
#define CMD_GLOVE_OFF		0x10
#define CMD_CRC_CHECK_START	0xAA

#define ABOV_TK_NAME		"abov-touchkey"
#define FW_VERSION		0x13
#define FW_CHECKSUM_H		0x55
#define FW_CHECKSUM_L		0xA8
#define TK_FW_PATH_BIN		"abov/abov_tk_royce.fw"
#define TK_FW_PATH_SDCARD	"/sdcard/abov_fw.bin"

#define CRC_CHECK_WITHBOOTING
#define CRC_CHECK_FW_VER	0x10

#define ABOV_BOOT_DELAY		45
#ifdef CRC_CHECK_WITHBOOTING
#define ABOV_RESET_DELAY	100
#else
#define ABOV_RESET_DELAY	150
#endif

#define I2C_M_WR 0		/* for i2c */

enum {
	BUILT_IN = 0,
	SDCARD,
};

enum flip_status{
	FLIP_OPEN = 0,
	FLIP_CLOSE,
};

enum fw_update_status{
	FW_UP_SUCCESS = 0,
	FW_DOWNLOADING,
	FW_UP_FAILED,
};

#ifdef CONFIG_SAMSUNG_LPM_MODE
extern int poweroff_charging;
#endif
extern unsigned int system_rev;
extern int get_lcd_attached(char *);
static int touchkey_keycode[] = { 0,
	KEY_RECENT, KEY_BACK, KEY_HOMEPAGE, KEY_TKEY_WAKEUP,
};

struct abov_touchkey_platform_data {
	unsigned long irq_flag;
	int gpio_int;
	int gpio_sda;
	int gpio_scl;
	int gpio_rst;
	int gpio_hall;
	int gpio_tkey_led_en;
	struct regulator *vdd_io_vreg;
	struct regulator *avdd_vreg;
	const char *supply_name;
	struct regulator *vtouch_3p3;
	void (*input_event) (void *data);
	int (*power) (struct abov_touchkey_platform_data *pdata, bool on);
	int (*keyled) (bool on);
	bool first_boot;
	char *fw_path;
};

struct abov_tk_info {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct abov_touchkey_platform_data *pdata;
	struct mutex lock;
	struct pinctrl *pinctrl;
	struct device *dev;
	const struct firmware *firm_data_bin;
	const u8 *firm_data_ums;
	char phys[32];
	long firm_size;
	int irq;
	u16 menu_s;
	u16 back_s;
	u16 home_s;
	u16 menu_raw;
	u16 back_raw;
	u16 home_raw;
	int (*power) (bool on);
	void (*input_event)(void *data);
	int touchkey_count;
	u8 fw_update_state;
	u8 fw_ver;
	u8 checksum_h;
	u8 checksum_l;
	bool enabled;
	bool fw_update_possible;
	bool glovemode;
	bool wakeup_mode;
	bool wakeup_state;
	struct wake_lock report_wake_lock;
#ifdef CONFIG_DUAL_TSP
	struct delayed_work switching_work;
	struct notifier_block hall_ic_nb;
	int flip_status;
	int flip_status_current;
#endif
	bool probe_done;
#ifdef CONFIG_INPUT_BOOSTER
	struct input_booster *tkey_booster;
#endif
};
struct device *sec_device_create(void *drvdata, const char *fmt);

static int abov_touchkey_led_status;
static int abov_touchled_cmd_reserved;

static int abov_tk_suspend(struct device *dev);
static int abov_tk_input_open(struct input_dev *dev);
static void abov_tk_input_close(struct input_dev *dev);
int abov_power(struct abov_touchkey_platform_data *pdata, bool on);

#ifdef CONFIG_DUAL_TSP
static void abov_switching_tkey_work(struct work_struct *work);
static int abov_hall_ic_notify(struct notifier_block *nb,
				unsigned long flip_cover, void *v);

static struct abov_tk_info *tkey_driver = NULL;
void abov_set_tkey_info(struct abov_tk_info *info)
{
	if(info != NULL)
		tkey_driver = info;
	else
		pr_info("%s : tkey info is null\n", __func__);
}

static struct abov_tk_info *abov_get_tkey_info(void)
{
	return tkey_driver;
}
#endif

static int abov_glove_mode_enable(struct i2c_client *client, u8 cmd)
{
	return i2c_smbus_write_byte_data(client, ABOV_GLOVE, cmd);
}

static int abov_tk_i2c_read(struct i2c_client *client,
		u8 reg, u8 *val, unsigned int len)
{
	struct abov_tk_info *info = i2c_get_clientdata(client);
	struct i2c_msg msg;
	int ret;
	int retry = 3;

	mutex_lock(&info->lock);
	msg.addr = client->addr;
	msg.flags = I2C_M_WR;
	msg.len = 1;
	msg.buf = &reg;
	while (retry--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret >= 0)
			break;

		dev_err(&client->dev, "%s fail(address set)(%d)\n",
			__func__, retry);
		msleep(10);
	}
	if (ret < 0) {
		mutex_unlock(&info->lock);
		return ret;
	}
	retry = 3;
	msg.flags = 1;/*I2C_M_RD*/
	msg.len = len;
	msg.buf = val;
	while (retry--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret >= 0) {
			mutex_unlock(&info->lock);
			return 0;
		}
		dev_err(&client->dev, "%s fail(data read)(%d)\n",
			__func__, retry);
		msleep(10);
	}
	mutex_unlock(&info->lock);
	return ret;
}

static int abov_tk_i2c_write(struct i2c_client *client,
		u8 reg, u8 *val, unsigned int len)
{
	struct abov_tk_info *info = i2c_get_clientdata(client);
	struct i2c_msg msg[1];
	unsigned char data[2];
	int ret;
	int retry = 3;

	mutex_lock(&info->lock);
	data[0] = reg;
	data[1] = *val;
	msg->addr = client->addr;
	msg->flags = I2C_M_WR;
	msg->len = 2;
	msg->buf = data;

	while (retry--) {
		ret = i2c_transfer(client->adapter, msg, 1);
		if (ret >= 0) {
			mutex_unlock(&info->lock);
			return 0;
		}
		dev_err(&client->dev, "%s fail(%d)\n",
			__func__, retry);
		msleep(10);
	}
	mutex_unlock(&info->lock);
	return ret;
}

static void release_all_fingers(struct abov_tk_info *info)
{
	struct i2c_client *client = info->client;
	int i;

	dev_dbg(&client->dev, "[TK] %s\n", __func__);

	for (i = 1; i < info->touchkey_count; i++) {
		input_report_key(info->input_dev,
			touchkey_keycode[i], 0);
	}
	input_sync(info->input_dev);
#ifdef CONFIG_INPUT_BOOSTER
	if (info->tkey_booster && info->tkey_booster->dvfs_set)
		info->tkey_booster->dvfs_set(info->tkey_booster, 2);
#endif
}

static void abov_tk_reset_for_bootmode(struct abov_tk_info *info)
{
	info->pdata->power(info->pdata, false);
	msleep(50);
	info->pdata->power(info->pdata, true);
}

static void abov_tk_reset(struct abov_tk_info *info)
{
	struct i2c_client *client = info->client;

	if (info->enabled == false)
		return;

	dev_notice(&client->dev, "%s++\n", __func__);
	disable_irq_nosync(info->irq);

	info->enabled = false;

	release_all_fingers(info);

	abov_tk_reset_for_bootmode(info);
	msleep(ABOV_RESET_DELAY);

	if (info->glovemode)
		abov_glove_mode_enable(client, CMD_GLOVE_ON);

	info->enabled = true;

	enable_irq(info->irq);
	dev_notice(&client->dev, "%s--\n", __func__);
}

static irqreturn_t abov_tk_interrupt(int irq, void *dev_id)
{
	struct abov_tk_info *info = dev_id;
	struct i2c_client *client = info->client;
	int ret, retry;
	u8 buf;
	bool press = 0;
	u8 key_data[info->touchkey_count];
	u8 key_press[info->touchkey_count];
	int i;

	ret = abov_tk_i2c_read(client, ABOV_KEY_VALUE, &buf, 1);
	if (ret < 0) {
		retry = 3;
		while (retry--) {
			dev_err(&client->dev, "%s read fail(%d)\n",
				__func__, retry);
			ret = abov_tk_i2c_read(client, ABOV_KEY_VALUE, &buf, 1);
			if (ret == 0)
				break;
			else
				msleep(10);
		}
		if (retry == 0) {
			abov_tk_reset(info);
			return IRQ_HANDLED;
		}
	}

	/* concept for L OS screen pinning (multi key)
	 *
	 * bit	7	6	5	4	3	2	1	0
	 * event	-	double	home	home	back	back	menu	menu
	 *	-	tab	press	release	press	release	press	release
	 */

	for (i = 1; i < info->touchkey_count; i++) {
		if (i == 4) {
			/* double tab event has only 1 bit */
			key_data[i] = (buf >> ((i - 1) * 2)) & 0x01;
			key_press[i] = key_data[i];
		} else {
			key_data[i] = (buf >> ((i - 1) * 2)) & 0x03;
			key_press[i] = !(key_data[i] % 2);
		}

		if (key_data[i]) {
			press |= key_press[i];
			input_report_key(info->input_dev,
					touchkey_keycode[i], key_press[i]);
#ifdef CONFIG_SAMSUNG_PRODUCT_SHIP
			dev_info(&client->dev, "key %s ver:0x%x\n",
					key_press[i] ? "P" : "R",
					info->fw_ver);
#else
			dev_info(&client->dev, "key %d %s ver:0x%x\n",
					touchkey_keycode[i],
					key_press[i] ? "P" : "R",
					info->fw_ver);
#endif
		}
	}

	input_sync(info->input_dev);

	if (key_press[4]){
		dev_info(&client->dev, "Double tab wakeup\n");
		wake_lock_timeout(&info->report_wake_lock, 3 * HZ);
	}

#ifdef CONFIG_INPUT_BOOSTER
	if (info->tkey_booster && info->tkey_booster->dvfs_set)
		info->tkey_booster->dvfs_set(info->tkey_booster, press);
#endif

	return IRQ_HANDLED;
}

#ifdef CONFIG_INPUT_BOOSTER
static ssize_t boost_level_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	int val, stage;
	struct dvfs value;

	if (!info->tkey_booster) {
		dev_err(&info->client->dev,
			"%s: booster is NULL\n", __func__);
		return count;
	}

	sscanf(buf, "%d", &val);

	stage = 1 << val;

	if (!(info->tkey_booster->dvfs_stage & stage)) {
		dev_info(&info->client->dev,
			"%s: wrong cmd %d\n", __func__, val);
		return count;
	}

	info->tkey_booster->dvfs_boost_mode = stage;
	dev_info(&info->client->dev,
			"%s: dvfs_boost_mode = 0x%X\n",
			__func__, info->tkey_booster->dvfs_boost_mode);

	if (info->tkey_booster->dvfs_boost_mode == DVFS_STAGE_NONE) {
		if (info->tkey_booster->dvfs_set)
			info->tkey_booster->dvfs_set(info->tkey_booster, 2);
	} else if (info->tkey_booster->dvfs_boost_mode == DVFS_STAGE_SINGLE) {
		input_booster_get_default_setting("head", &value);
		info->tkey_booster->dvfs_freq = value.cpu_freq;
		dev_info(&info->client->dev,
			"%s: boost_mode SINGLE, dvfs_freq = %d\n",
			__func__, info->tkey_booster->dvfs_freq);
	} else if (info->tkey_booster->dvfs_boost_mode == DVFS_STAGE_DUAL) {
		input_booster_get_default_setting("tail", &value);
		info->tkey_booster->dvfs_freq = value.cpu_freq;
		dev_info(&info->client->dev,
			"%s: boost_mode DUAL, dvfs_freq = %d\n",
			__func__, info->tkey_booster->dvfs_freq);
	}

	return count;
}
#endif

static ssize_t touchkey_led_control(struct device *dev,
		 struct device_attribute *attr, const char *buf,
		 size_t count)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int data;
	int ret;
	u8 cmd;

	ret = sscanf(buf, "%d", &data);
	if (ret != 1) {
		dev_err(&client->dev, "%s: cmd read err\n", __func__);
		return count;
	}

	if (!(data == 0 || data == 1)) {
		dev_err(&client->dev, "%s: wrong command(%d)\n",
			__func__, data);
		return count;
	}

	if (data == 1)
		cmd = CMD_LED_ON;
	else
		cmd = CMD_LED_OFF;

	if (!info->enabled){
		abov_touchled_cmd_reserved = 1;
		goto out;
	}

	ret = abov_tk_i2c_write(client, ABOV_BTNSTATUS, &cmd, 1);
	if (ret < 0){
		dev_err(&client->dev, "%s fail(%d)\n", __func__, ret);
		abov_touchled_cmd_reserved = 1;
		goto out;
	}
	abov_touchled_cmd_reserved = 0;
	dev_info(&client->dev, "%s %s\n", __func__, data ? "ON" : "OFF");

out:
	abov_touchkey_led_status = cmd;
	return count;
}

static ssize_t touchkey_threshold_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	u8 r_buf;
	int ret;

	ret = abov_tk_i2c_read(client, ABOV_THRESHOLD, &r_buf, 1);
	if (ret < 0) {
		dev_err(&client->dev, "%s fail(%d)\n", __func__, ret);
		r_buf = 0;
	}
	return sprintf(buf, "%d\n", r_buf);
}

static void get_diff_data(struct abov_tk_info *info)
{
	struct i2c_client *client = info->client;
	u8 r_buf[6];
	int ret;

	ret = abov_tk_i2c_read(client, ABOV_DIFFDATA, r_buf, 6);
	if (ret < 0) {
		dev_err(&client->dev, "%s fail(%d)\n", __func__, ret);
		info->menu_s = 0;
		info->back_s = 0;
		info->home_s = 0;
		return;
	}

	info->menu_s = (r_buf[0] << 8) | r_buf[1];
	info->back_s = (r_buf[2] << 8) | r_buf[3];
	info->home_s = (r_buf[4] << 8) | r_buf[5];
}

static ssize_t touchkey_menu_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);

	get_diff_data(info);

	return sprintf(buf, "%d\n", info->menu_s);
}

static ssize_t touchkey_back_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);

	get_diff_data(info);

	return sprintf(buf, "%d\n", info->back_s);
}

static ssize_t touchkey_home_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);

	get_diff_data(info);

	return sprintf(buf, "%d\n", info->home_s);
}


static void get_raw_data(struct abov_tk_info *info)
{
	struct i2c_client *client = info->client;
	u8 r_buf[6];
	int ret;

	ret = abov_tk_i2c_read(client, ABOV_RAWDATA, r_buf, 6);
	if (ret < 0) {
		dev_err(&client->dev, "%s fail(%d)\n", __func__, ret);
		info->menu_raw = 0;
		info->back_raw = 0;
		info->home_raw = 0;
		return;
	}

	info->menu_raw = (r_buf[0] << 8) | r_buf[1];
	info->back_raw = (r_buf[2] << 8) | r_buf[3];
	info->home_raw = (r_buf[4] << 8) | r_buf[5];
}

static ssize_t touchkey_menu_raw_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);

	get_raw_data(info);

	return sprintf(buf, "%d\n", info->menu_raw);
}

static ssize_t touchkey_back_raw_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);

	get_raw_data(info);

	return sprintf(buf, "%d\n", info->back_raw);
}

static ssize_t touchkey_home_raw_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);

	get_raw_data(info);

	return sprintf(buf, "%d\n", info->home_raw);
}

static ssize_t bin_fw_ver(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;

	dev_dbg(&client->dev, "fw version bin : 0x%x\n", FW_VERSION);

	return sprintf(buf, "0x%02x\n", FW_VERSION);
}

int get_tk_fw_version(struct abov_tk_info *info, bool bootmode)
{
	struct i2c_client *client = info->client;
	u8 buf;
	int ret;
	int retry = 3;

	ret = abov_tk_i2c_read(client, ABOV_FW_VER, &buf, 1);
	if (ret < 0) {
		while (retry--) {
			dev_err(&client->dev, "%s read fail(%d)\n",
				__func__, retry);
			if (!bootmode)
				abov_tk_reset(info);
			else
				return -1;
			ret = abov_tk_i2c_read(client, ABOV_FW_VER, &buf, 1);
			if (ret == 0)
				break;
		}
		if (retry == 0)
			return -1;
	}

	info->fw_ver = buf;
	dev_info(&client->dev, "%s : 0x%x\n", __func__, buf);
	return 0;
}

static ssize_t read_fw_ver(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int ret;

	if(info->flip_status == FLIP_CLOSE || !info->fw_ver){
		ret = get_tk_fw_version(info, false);
		if (ret < 0) {
			dev_err(&client->dev, "%s read fail\n", __func__);
			info->fw_ver = 0;
		}
	}

	return sprintf(buf, "0x%02x\n", info->fw_ver);
}

static int abov_load_fw(struct abov_tk_info *info, u8 cmd)
{
	struct i2c_client *client = info->client;
	struct file *fp;
	mm_segment_t old_fs;
	long fsize, nread;
	int ret = 0;

	switch(cmd) {
	case BUILT_IN:
		ret = request_firmware(&info->firm_data_bin,
			info->pdata->fw_path, &client->dev);
		if (ret) {
			dev_err(&client->dev,
				"%s request_firmware fail(%d)\n", __func__, cmd);
			return ret;
		}
		info->firm_size = info->firm_data_bin->size;
		break;

	case SDCARD:
		old_fs = get_fs();
		set_fs(get_ds());
		fp = filp_open(TK_FW_PATH_SDCARD, O_RDONLY, S_IRUSR);
		if (IS_ERR(fp)) {
			dev_err(&client->dev,
				"%s %s open error\n", __func__, TK_FW_PATH_SDCARD);
			ret = -ENOENT;
			goto fail_sdcard_open;
		}

		fsize = fp->f_path.dentry->d_inode->i_size;
		info->firm_data_ums = kzalloc((size_t)fsize, GFP_KERNEL);
		if (!info->firm_data_ums) {
			dev_err(&client->dev,
				"%s fail to kzalloc for fw\n", __func__);
			ret = -ENOMEM;
			goto fail_sdcard_kzalloc;
		}

		nread = vfs_read(fp,
			(char __user *)info->firm_data_ums, fsize, &fp->f_pos);
		if (nread != fsize) {
			dev_err(&client->dev,
				"%s fail to vfs_read file\n", __func__);
			ret = -EINVAL;
			goto fail_sdcard_size;
		}
		filp_close(fp, current->files);
		set_fs(old_fs);
		info->firm_size = nread;
		break;

	default:
		ret = -1;
		break;
	}
	dev_notice(&client->dev, "fw_size : %lu\n", info->firm_size);
	dev_notice(&client->dev, "%s success\n", __func__);
	return ret;

fail_sdcard_size:
	kfree(&info->firm_data_ums);
fail_sdcard_kzalloc:
	filp_close(fp, current->files);
fail_sdcard_open:
	set_fs(old_fs);
	return ret;
}

static void abov_release_fw(struct abov_tk_info *info, u8 cmd)
{
	switch(cmd) {
	case BUILT_IN:
		release_firmware(info->firm_data_bin);
		break;

	case SDCARD:
		kfree(info->firm_data_ums);
		break;

	default:
		break;
	}
}

static int abov_tk_check_busy(struct abov_tk_info *info)
{
	int ret, count = 0;
	unsigned char val = 0x00;

	do {
		ret = i2c_master_recv(info->client, &val, sizeof(val));

		if (val) {
			count++;
		} else {
			break;
		}

	} while(1);

	if (count > 1000)
		pr_err("%s: busy %d\n", __func__, count);
	return ret;
}

static int abov_tk_i2c_read_checksum(struct abov_tk_info *info)
{
	unsigned char data[6] = {0xAC, 0x9E, 0x10, 0x00, 0x3F, 0xFF};
	unsigned char checksum[5] = {0, };
	int ret;
	unsigned char reg = 0x00;

	i2c_master_send(info->client, data, 6);

	usleep_range(5 * 1000, 5 * 1000);

	abov_tk_check_busy(info);

	ret = abov_tk_i2c_read(info->client, reg, checksum, 5);

	dev_info(&info->client->dev, "%s: ret:%d [%X][%X][%X][%X][%X]\n",
			__func__, ret, checksum[0], checksum[1], checksum[2]
			, checksum[3], checksum[4]);
	info->checksum_h = checksum[3];
	info->checksum_l = checksum[4];
	return 0;
}

static int abov_tk_fw_write(struct abov_tk_info *info, unsigned char *addrH,
						unsigned char *addrL, unsigned char *val)
{
	int length = 36, ret = 0;
	unsigned char data[36];

	data[0] = 0xAC;
	data[1] = 0x7A;
	memcpy(&data[2], addrH, 1);
	memcpy(&data[3], addrL, 1);
	memcpy(&data[4], val, 32);

	ret = i2c_master_send(info->client, data, length);
	if (ret != length) {
		pr_err("%s: write fail[%x%x], %d\n", __func__, *addrH, *addrL, ret);
		return ret;
	}

	usleep_range(2 * 1000, 2 * 1000);

	abov_tk_check_busy(info);

	return 0;
}

static int abov_tk_fw_mode_enter(struct abov_tk_info *info)
{
	unsigned char data[3] = {0xAC, 0x5B, 0x2D};
	int ret = 0;

	ret = i2c_master_send(info->client, data, 3);
	if (ret != 3) {
		pr_err("%s: write fail %d\n", __func__, ret);
		return ret;
	}

	return 0;

}

static int abov_tk_fw_update(struct abov_tk_info *info, u8 cmd)
{
	int ret, ii = 0;
	int count;
	unsigned short address;
	unsigned char addrH, addrL;
	unsigned char data[32] = {0, };

	count = info->firm_size / 32;
	address = 0x1000;

	abov_power(info->pdata, 0);
	msleep(30);
	abov_power(info->pdata, 1);
	usleep_range(ABOV_BOOT_DELAY * 1000, ABOV_BOOT_DELAY * 1000);

	ret = abov_tk_fw_mode_enter(info);

	pr_info("%s: enter BL mode, ret = %d\n", __func__, ret);

	msleep(1100);

	for (ii = 0; ii < count; ii++) {

		addrH = (unsigned char)((address >> 8) & 0xFF);
		addrL = (unsigned char)(address & 0xFF);
		if (cmd == BUILT_IN)
			memcpy(data, &info->firm_data_bin->data[ii * 32], 32);
		else if(cmd == SDCARD)
			memcpy(data, &info->firm_data_ums[ii * 32], 32);

		ret = abov_tk_fw_write(info, &addrH, &addrL, data);
		if (ret < 0) {
			pr_err("%s: err, no device : %d\n", __func__, ret);
			return ret;
		}
		usleep_range(3 * 1000, 3 * 1000);

		abov_tk_check_busy(info);

		address += 0x20;

		memset(data, 0, 32);
	}

	ret = abov_tk_i2c_read_checksum(info);

	return ret;
}

static int abov_flash_fw(struct abov_tk_info *info, bool probe, u8 cmd)
{
	struct i2c_client *client = info->client;
	int retry = 3;
	int ret;

	ret = get_tk_fw_version(info, probe);
	if (ret)
		info->fw_ver = 0;

	ret = abov_load_fw(info, cmd);
	if (ret) {
		dev_err(&client->dev,
			"%s fw load fail\n", __func__);
		return ret;
	}

	while (retry--) {
		ret = abov_tk_fw_update(info, cmd);
		if (ret < 0)
			break;
		if (cmd == BUILT_IN) {
			if ((info->checksum_h != FW_CHECKSUM_H) ||
				(info->checksum_l != FW_CHECKSUM_L)) {
				dev_err(&client->dev,
					"%s checksum fail. IC(0x%x,0x%x), BN(0x%x,0x%x) retry:%d\n",
					__func__, info->checksum_h, info->checksum_l,
					FW_CHECKSUM_H, FW_CHECKSUM_L, retry);
				ret = -1;
				continue;
			}
		}
		abov_tk_reset_for_bootmode(info);
		msleep(ABOV_RESET_DELAY);
		ret = get_tk_fw_version(info, true);
		if (ret) {
			dev_err(&client->dev, "%s fw version read fail\n", __func__);
			ret = -1;
			continue;
		}

		if (info->fw_ver == 0) {
			dev_err(&client->dev, "%s fw version fail (0x%x)\n",
				__func__, info->fw_ver);
			ret = -1;
			continue;
		}

		if ((cmd == BUILT_IN) && (info->fw_ver != FW_VERSION)) {
			dev_err(&client->dev, "%s fw version fail 0x%x, 0x%x\n",
				__func__, info->fw_ver, FW_VERSION);
			ret = -1;
			continue;
		}
		ret = 0;
		break;
	}

	abov_release_fw(info, cmd);

	return ret;
}

static ssize_t touchkey_fw_update(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int ret;
	u8 cmd;

	if (info->fw_update_possible == false) {
		dev_err(&client->dev, "%s fail (no reset pin)\n", __func__);
		info->fw_update_state = FW_UP_FAILED;
		goto touchkey_fw_update_out;
	}

	switch(*buf) {
	case 's':
	case 'S':
		cmd = BUILT_IN;
		break;
	case 'i':
	case 'I':
		cmd = SDCARD;
		break;
	default:
		info->fw_update_state = FW_UP_FAILED;
		goto touchkey_fw_update_out;
	}

	info->fw_update_state = FW_DOWNLOADING;
	disable_irq(info->irq);
	info->enabled = false;
	ret = abov_flash_fw(info, false, cmd);
	if (info->glovemode)
		abov_glove_mode_enable(client, CMD_GLOVE_ON);
	info->enabled = true;
	enable_irq(info->irq);
	if (ret) {
		dev_err(&client->dev, "%s fail\n", __func__);
		info->fw_update_state = FW_UP_FAILED;
	} else {
		dev_notice(&client->dev, "%s success\n", __func__);
		info->fw_update_state = FW_UP_SUCCESS;
	}

touchkey_fw_update_out:
	dev_dbg(&client->dev, "%s : %d\n", __func__, info->fw_update_state);

	return count;
}

static ssize_t touchkey_fw_update_status(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int count = 0;

	dev_info(&client->dev, "%s : %d\n", __func__, info->fw_update_state);

	if (info->fw_update_state == FW_UP_SUCCESS)
		count = sprintf(buf, "PASS\n");
	else if (info->fw_update_state == FW_DOWNLOADING)
		count = sprintf(buf, "Downloading\n");
	else if (info->fw_update_state == FW_UP_FAILED)
		count = sprintf(buf, "Fail\n");

	return count;
}

static ssize_t abov_glove_mode(struct device *dev,
	 struct device_attribute *attr, const char *buf, size_t count)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int scan_buffer;
	int ret;
	u8 cmd;

	ret = sscanf(buf, "%d", &scan_buffer);
	if (ret != 1) {
		dev_err(&client->dev, "%s: cmd read err\n", __func__);
		return count;
	}

	if (!(scan_buffer == 0 || scan_buffer == 1)) {
		dev_err(&client->dev, "%s: wrong command(%d)\n",
			__func__, scan_buffer);
		return count;
	}

	if (!info->enabled)
		return count;

	if (info->glovemode == scan_buffer) {
		dev_info(&client->dev, "%s same command(%d)\n",
			__func__, scan_buffer);
		return count;
	}

	if (scan_buffer == 1) {
		dev_notice(&client->dev, "%s glove mode\n", __func__);
		cmd = CMD_GLOVE_ON;
	} else {
		dev_notice(&client->dev, "%s normal mode\n", __func__);
		cmd = CMD_GLOVE_OFF;
	}

	ret = abov_glove_mode_enable(client, cmd);
	if (ret < 0) {
		dev_err(&client->dev, "%s fail(%d)\n", __func__, ret);
		return count;
	}

	info->glovemode = scan_buffer;

	return count;
}

static ssize_t abov_glove_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", info->glovemode);
}

static ssize_t abov_wakeup_mode(struct device *dev,
	 struct device_attribute *attr, const char *buf, size_t count)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	int scan_buffer;
	int ret;

	ret = sscanf(buf, "%d", &scan_buffer);
	if (ret != 1)
		goto err;

	if (!(scan_buffer == 0 || scan_buffer == 1))
		goto err;

	if(scan_buffer == 1)
		info->wakeup_mode = true;
	else
		info->wakeup_mode = false;

	dev_info(dev, "%s : Set to %s mode\n",
		__func__, info->wakeup_mode ? "wakeup" : "normal");
err:
	return count;
}

static ssize_t abov_wakeup_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", info->wakeup_mode);
}


static DEVICE_ATTR(touchkey_threshold, S_IRUGO, touchkey_threshold_show, NULL);
static DEVICE_ATTR(brightness, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
			touchkey_led_control);
static DEVICE_ATTR(touchkey_recent, S_IRUGO, touchkey_menu_show, NULL);
static DEVICE_ATTR(touchkey_back, S_IRUGO, touchkey_back_show, NULL);
static DEVICE_ATTR(touchkey_home, S_IRUGO, touchkey_home_show, NULL);
static DEVICE_ATTR(touchkey_recent_raw, S_IRUGO, touchkey_menu_raw_show, NULL);
static DEVICE_ATTR(touchkey_back_raw, S_IRUGO, touchkey_back_raw_show, NULL);
static DEVICE_ATTR(touchkey_home_raw, S_IRUGO, touchkey_home_raw_show, NULL);
static DEVICE_ATTR(touchkey_firm_version_phone, S_IRUGO, bin_fw_ver, NULL);
static DEVICE_ATTR(touchkey_firm_version_panel, S_IRUGO, read_fw_ver, NULL);
static DEVICE_ATTR(touchkey_firm_update, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
			touchkey_fw_update);
static DEVICE_ATTR(touchkey_firm_update_status, S_IRUGO | S_IWUSR | S_IWGRP,
			touchkey_fw_update_status, NULL);
static DEVICE_ATTR(glove_mode, S_IRUGO | S_IWUSR | S_IWGRP,
			abov_glove_mode_show, abov_glove_mode);
static DEVICE_ATTR(two_touch_wakeup_mode, S_IRUGO | S_IWUSR | S_IWGRP,
			abov_wakeup_mode_show, abov_wakeup_mode);
#ifdef CONFIG_INPUT_BOOSTER
static DEVICE_ATTR(boost_level,
		   S_IWUSR | S_IWGRP, NULL, boost_level_store);
#endif

static struct attribute *sec_touchkey_attributes[] = {
	&dev_attr_touchkey_threshold.attr,
	&dev_attr_brightness.attr,
	&dev_attr_touchkey_recent.attr,
	&dev_attr_touchkey_back.attr,
	&dev_attr_touchkey_home.attr,
	&dev_attr_touchkey_recent_raw.attr,
	&dev_attr_touchkey_back_raw.attr,
	&dev_attr_touchkey_home_raw.attr,
	&dev_attr_touchkey_firm_version_phone.attr,
	&dev_attr_touchkey_firm_version_panel.attr,
	&dev_attr_touchkey_firm_update.attr,
	&dev_attr_touchkey_firm_update_status.attr,
	&dev_attr_glove_mode.attr,
	&dev_attr_two_touch_wakeup_mode.attr,
#ifdef CONFIG_INPUT_BOOSTER
	&dev_attr_boost_level.attr,
#endif
	NULL,
};
static struct attribute_group sec_touchkey_attr_group = {
	.attrs = sec_touchkey_attributes,
};


static int abov_tk_fw_check(struct abov_tk_info *info)
{
	struct i2c_client *client = info->client;
	int ret;

#ifdef CRC_CHECK_WITHBOOTING
	u8 checksum_cmd;
	int i;

	ret = abov_tk_i2c_read(client, ABOV_FW_VER, &checksum_cmd, 1);
	if (ret < 0) {
		dev_err(&client->dev, "%s fail(%d)\n", __func__, ret);
		checksum_cmd = 0;
	}

	dev_info(&client->dev, "%s FW ver = 0x%x\n", __func__, checksum_cmd);

	/* CRC Check */
	if(checksum_cmd >= CRC_CHECK_FW_VER) {
		checksum_cmd = CMD_CRC_CHECK_START;
		ret = abov_tk_i2c_write(client, ABOV_FW_VER, &checksum_cmd, 1);
		if (ret < 0) {
			dev_err(&client->dev,
				"%s %d fail(%d)\n", __func__, __LINE__, ret);
			checksum_cmd = 0;
		}

		msleep(40);

		for (i = 0; i < 30; i++) {
			/* Read 0x01 to check pass */
			ret = abov_tk_i2c_read(client, ABOV_FW_VER, &checksum_cmd, 1);
			if (ret < 0) {
				dev_err(&client->dev,
					"%s %d fail(%d)\n", __func__, __LINE__, ret);
				checksum_cmd = 0;
			}

			if (checksum_cmd != CMD_CRC_CHECK_START) {
				dev_info(&client->dev,
					"%s checksum result = 0x%x, %d\n",
					__func__, checksum_cmd, i);
				break;
			}
			usleep_range(5000, 5000);
		}
	} else {
		msleep(80);
	}
#endif

	ret = get_tk_fw_version(info, true);
	if (ret) {
		dev_err(&client->dev,
			"%s: i2c fail...[%d], addr[%d]\n",
			__func__, ret, info->client->addr);
		}

	if (!info->fw_update_possible)
		return ret;

	if (ret || info->fw_ver < FW_VERSION || info->fw_ver > 0xf0) {
		dev_err(&client->dev, "excute tk firmware update (0x%x -> 0x%x\n",
			info->fw_ver, FW_VERSION);
		ret = abov_flash_fw(info, true, BUILT_IN);
		if (ret) {
			dev_err(&client->dev,
				"failed to abov_flash_fw (%d)\n", ret);
		} else {
			dev_info(&client->dev,
				"fw update success\n");
		}
	}

	return ret;
}

int abov_power(struct abov_touchkey_platform_data *pdata, bool on)
{
	int ret = 0;

	if (pdata->vtouch_3p3) {
		if (on){
			if (!pdata->first_boot && regulator_is_enabled(pdata->vtouch_3p3))
				pr_err("%s: vtouch_3p3 is already enabled.\n",__func__);
			else {
				ret = regulator_enable(pdata->vtouch_3p3);
				if (ret){
					pr_err("%s: failed to enable vtouch_3p3. %d\n",
						__func__, ret);
					goto err;
				}
			}
		} else {
			if (regulator_is_enabled(pdata->vtouch_3p3)){
				ret = regulator_disable(pdata->vtouch_3p3);
				if (ret){
					pr_err("%s: failed to disable vtouch_3p3. %d\n",
						__func__, ret);
					goto err;
				}
			} else {
				pr_err("%s: vtouch_3p3 is already disabled.\n",__func__);
			}
		}
		pr_info("%s: vtouch_3p3 is %s\n", __func__,
			regulator_is_enabled(pdata->vtouch_3p3) ? "enabled" : "disabled");
	}

err:
	return ret;
}

static int abov_pinctrl_configure(struct abov_tk_info *info,
							bool active)
{
	struct pinctrl_state *set_state;
	int retval;

	if (active) {
		set_state =
			pinctrl_lookup_state(info->pinctrl,
						"on_irq");
		if (IS_ERR(set_state)) {
			pr_err("%s: cannot get ts pinctrl active state\n", __func__);
			return PTR_ERR(set_state);
		}
	} else {
		set_state =
			pinctrl_lookup_state(info->pinctrl,
						"off_irq");
		if (IS_ERR(set_state)) {
			pr_err("%s: cannot get gpiokey pinctrl sleep state\n", __func__);
			return PTR_ERR(set_state);
		}
	}
	retval = pinctrl_select_state(info->pinctrl, set_state);
	if (retval) {
		pr_err("%s: cannot set ts pinctrl active state\n", __func__);
		return retval;
	}

	return 0;
}

int abov_gpio_reg_init(struct device *dev,
			struct abov_touchkey_platform_data *pdata)
{
	int ret = 0;
	ret = gpio_request(pdata->gpio_int, "tkey_gpio_int");
	if(ret < 0){
		dev_err(dev, "unable to request gpio_int\n");
		return ret;
	}

	pdata->vtouch_3p3 = regulator_get(dev, pdata->supply_name);
	if (IS_ERR(pdata->vtouch_3p3)) {
		pdata->vtouch_3p3 = NULL;
		dev_err(dev, "pdata->vtouch_3p3 get error\n");
		return -1;
	}

	pdata->power = abov_power;

	return ret;
}

#ifdef CONFIG_OF
static int abov_parse_dt(struct device *dev,
			struct abov_touchkey_platform_data *pdata)
{
	struct device_node *np = dev->of_node;
	int retval = 0;

	pdata->gpio_int = of_get_named_gpio(np, "abov,irq-gpio", 0);
	if(pdata->gpio_int < 0){
		dev_err(dev, "unable to get gpio_int\n");
		return pdata->gpio_int;
	}

	pdata->gpio_scl = of_get_named_gpio(np, "abov,scl-gpio", 0);
	if(pdata->gpio_scl < 0){
		dev_err(dev, "unable to get gpio_scl\n");
		return pdata->gpio_scl;
	}

	pdata->gpio_sda = of_get_named_gpio(np, "abov,sda-gpio", 0);
	if(pdata->gpio_sda < 0){
		dev_err(dev, "unable to get gpio_sda\n");
		return pdata->gpio_sda;
	}

	pdata->gpio_hall = of_get_named_gpio(np, "abov,hall_flip-gpio", 0);
	if(pdata->gpio_hall < 0){
		dev_err(dev, "unable to get gpio_hall\n");
	}

	retval = of_property_read_string(np, "abov,fw_path", (const char **)&pdata->fw_path);
	if (retval) {
		dev_err(dev, "touchkey:failed to read fw_path %d\n", retval);
		pdata->fw_path = TK_FW_PATH_BIN;
	}
	dev_info(dev, "%s: fw path %s\n", __func__, pdata->fw_path);

	retval = of_property_read_string(np, "abov,vtouch_3p3", &pdata->supply_name);
	if (retval)
		dev_err(dev, "unable to get name of vtouch_3p3, %d\n", retval);

	dev_info(dev, "%s: gpio_int:%d, gpio_scl:%d, gpio_sda:%d\n",
			__func__, pdata->gpio_int, pdata->gpio_scl, pdata->gpio_sda);

	return retval;
}
#else
static int abov_parse_dt(struct device *dev,
			struct abov_touchkey_platform_data *pdata)
{
	return -ENODEV;
}
#endif

static int abov_tk_probe(struct i2c_client *client,
				  const struct i2c_device_id *id)
{
	struct abov_tk_info *info;
	struct input_dev *input_dev;
	int ret = 0;

	pr_info("%s\n",__func__);
#if 0
	if (get_lcd_attached("GET") == 0) {
			dev_err(&client->dev, "%s : get_lcd_attached()=0 \n", __func__);
			return -EIO;
		}
#endif
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev,
			"i2c_check_functionality fail\n");
		return -EIO;
	}

	info = kzalloc(sizeof(struct abov_tk_info), GFP_KERNEL);
	if (!info) {
		dev_err(&client->dev, "Failed to allocate memory\n");
		ret = -ENOMEM;
		goto err_alloc;
	}

	input_dev = input_allocate_device();
	if (!input_dev) {
		dev_err(&client->dev,
			"Failed to allocate memory for input device\n");
		ret = -ENOMEM;
		goto err_input_alloc;
	}

	info->probe_done = false;
#ifdef CONFIG_DUAL_TSP
	abov_set_tkey_info(info);
	info->flip_status = -1;
	info->flip_status_current = -1;
#endif
	info->client = client;
	info->input_dev = input_dev;
	info->wakeup_mode = false;

	if (client->dev.of_node) {
		struct abov_touchkey_platform_data *pdata;
		pdata = devm_kzalloc(&client->dev,
			sizeof(struct abov_touchkey_platform_data), GFP_KERNEL);
		if (!pdata) {
			dev_err(&client->dev, "Failed to allocate memory\n");
			return -ENOMEM;
		}

		ret = abov_parse_dt(&client->dev, pdata);
		if (ret)
			return ret;

		info->pdata = pdata;
	} else
		info->pdata = client->dev.platform_data;

	if (info->pdata == NULL) {
		pr_err("failed to get platform data\n");
		goto err_config;
	}

	/* Get pinctrl if target uses pinctrl */
	info->pinctrl = devm_pinctrl_get(&client->dev);
	if (IS_ERR(info->pinctrl)) {
		if (PTR_ERR(info->pinctrl) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		pr_err("%s: Target does not use pinctrl\n", __func__);
		info->pinctrl = NULL;
	}

	if (info->pinctrl) {
		ret = abov_pinctrl_configure(info, true);
		if (ret)
			pr_err("%s: cannot set ts pinctrl active state\n", __func__);
	}

	ret = abov_gpio_reg_init(&client->dev, info->pdata);
	if(ret){
		dev_err(&client->dev, "failed to init reg\n");
		goto pwr_config;
	}

	info->pdata->first_boot = true;
	info->pdata->power(info->pdata, true);
	info->pdata->first_boot = false;

	client->irq = gpio_to_irq(info->pdata->gpio_int);

	info->irq = -1;
	mutex_init(&info->lock);
	wake_lock_init(&info->report_wake_lock, WAKE_LOCK_SUSPEND, "report_wake_lock");

	info->fw_update_possible = true;

	info->input_event = info->pdata->input_event;
	info->touchkey_count = sizeof(touchkey_keycode) / sizeof(int);
	i2c_set_clientdata(client, info);
	ret = abov_tk_fw_check(info);
	if (ret) {
		dev_err(&client->dev,
			"failed to firmware check (%d)\n", ret);
		goto err_reg_input_dev;
	}
	snprintf(info->phys, sizeof(info->phys),
		 "%s/input0", dev_name(&client->dev));
	input_dev->name = "sec_touchkey";
	input_dev->phys = info->phys;
	input_dev->id.bustype = BUS_HOST;
	input_dev->dev.parent = &client->dev;
	input_dev->open = abov_tk_input_open;
	input_dev->close = abov_tk_input_close;
	set_bit(EV_KEY, input_dev->evbit);
	set_bit(KEY_RECENT, input_dev->keybit);
	set_bit(KEY_BACK, input_dev->keybit);
	set_bit(KEY_HOMEPAGE, input_dev->keybit);
	set_bit(KEY_TKEY_WAKEUP, input_dev->keybit);
	set_bit(EV_LED, input_dev->evbit);
	set_bit(LED_MISC, input_dev->ledbit);
	input_set_drvdata(input_dev, info);

	ret = input_register_device(input_dev);
	if (ret) {
		dev_err(&client->dev, "failed to register input dev (%d)\n",
			ret);
		goto err_reg_input_dev;
	}

	info->enabled = true;

	if (!info->pdata->irq_flag) {
		ret = request_threaded_irq(client->irq, NULL, abov_tk_interrupt,
			IRQF_TRIGGER_LOW | IRQF_ONESHOT, ABOV_TK_NAME, info);
	} else {
		ret = request_threaded_irq(client->irq, NULL, abov_tk_interrupt,
			info->pdata->irq_flag, ABOV_TK_NAME, info);
	}
	if (ret < 0) {
		dev_err(&client->dev, "Failed to register interrupt %d\n", ret);
		goto err_req_irq;
	}
	info->irq = client->irq;

	info->dev = sec_device_create(info, "sec_touchkey");

	if (IS_ERR(info->dev))
		dev_err(&client->dev,
		"Failed to create device for the touchkey sysfs\n");


	ret = sysfs_create_group(&info->dev->kobj,
		&sec_touchkey_attr_group);
	if (ret)
		dev_err(&client->dev, "Failed to create sysfs group\n");

	ret = sysfs_create_link(&info->dev->kobj,
		&info->input_dev->dev.kobj, "input");
	if (ret < 0) {
		dev_err(&info->client->dev,
			"%s: Failed to create input symbolic link\n",
			__func__);
	}

#ifdef CONFIG_INPUT_BOOSTER
	info->tkey_booster = input_booster_allocate(INPUT_BOOSTER_ID_TKEY);
	if (!info->tkey_booster) {
		dev_err(&client->dev,
			"%s: Failed to alloc mem for tsp_booster\n", __func__);
		goto err_get_tkey_booster;
	}
#endif
#ifdef CONFIG_DUAL_TSP
	if(info->pdata->gpio_hall < 0)
		/* default set : tkey enable */
		info->flip_status = FLIP_CLOSE;
	else
		info->flip_status = !(gpio_get_value(info->pdata->gpio_hall));
	dev_info(&client->dev, "%s: Folder is %sed now.\n",
		__func__, info->flip_status ? "clos":"open");

	if (!info->flip_status)
		abov_tk_suspend(&client->dev);
#endif
#ifdef CONFIG_DUAL_TSP
	INIT_DELAYED_WORK(&info->switching_work, abov_switching_tkey_work);
	/* Hall IC notify priority -> ftn -> register */
	info->hall_ic_nb.priority = 2;
	info->hall_ic_nb.notifier_call = abov_hall_ic_notify;
	hall_ic_register_notify(&info->hall_ic_nb);
	dev_info(&client->dev, "%s,touchkey:hall ic register",__func__);
#endif


	info->probe_done = true;
	pr_info("%s done\n", __func__);
	return 0;

#ifdef CONFIG_INPUT_BOOSTER
/* Delete annotation when use this code later */
	//input_booster_free(info->tkey_booster);
	//info->tkey_booster = NULL;
err_get_tkey_booster:
#endif
err_req_irq:
	input_unregister_device(input_dev);
err_reg_input_dev:
	wake_lock_destroy(&info->report_wake_lock);
	mutex_destroy(&info->lock);
pwr_config:
err_config:
#ifdef CONFIG_DUAL_TSP
	tkey_driver = NULL;
#endif
	input_free_device(input_dev);
err_input_alloc:
	kfree(info);
err_alloc:
	return ret;

}

static int abov_tk_remove(struct i2c_client *client)
{
	struct abov_tk_info *info = i2c_get_clientdata(client);

/*	if (info->enabled)
		info->pdata->power(0);
*/
	info->enabled = false;
	if (info->irq >= 0)
		free_irq(info->irq, info);
	input_unregister_device(info->input_dev);
	input_free_device(info->input_dev);
#ifdef CONFIG_INPUT_BOOSTER
	input_booster_free(info->tkey_booster);
	info->tkey_booster = NULL;
#endif
	kfree(info);

	return 0;
}

static void abov_tk_shutdown(struct i2c_client *client)
{
	struct abov_tk_info *info = i2c_get_clientdata(client);
	u8 cmd = CMD_LED_OFF;

	info->enabled = false;

	abov_tk_i2c_write(client, ABOV_BTNSTATUS, &cmd, 1);
	dev_err(&info->client->dev, "%s: done \n", __func__);
}

static int abov_tk_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct abov_tk_info *info = i2c_get_clientdata(client);
	u8 cmd;
	int ret;

	if (!info->wakeup_state)
		if (!info->enabled)
			return 0;

	printk("Inside abov_tk_suspend \n");
	dev_dbg(&info->client->dev, "%s: users=%d\n", __func__,
		   info->input_dev->users);

	if (info->wakeup_mode && info->flip_status == FLIP_CLOSE){
		/*Enter WAKEUP mode*/
		cmd = CMD_STOP_MODE;
		ret = abov_tk_i2c_write(client, ABOV_BTNSTATUS, &cmd, 1);
		if (ret < 0){
			dev_err(dev,
				"%s : failed to write wakeup mode(%d)\n",
				__func__, ret);
			return 0;
		}
		dev_info(dev,
			"%s : success to enter tkey wakeup state\n",
			__func__);
		release_all_fingers(info);
		enable_irq_wake(info->irq);
		info->enabled = false;
		info->wakeup_state = true;
	}
	else {
		disable_irq(info->irq);
		info->enabled = false;
		info->wakeup_state = false;
		release_all_fingers(info);

		info->pdata->power(info->pdata, false);
	}
	return 0;
}

static int abov_tk_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct abov_tk_info *info = i2c_get_clientdata(client);
	u8 led_data;
	int ret;
	if (info->enabled)
		return 0;
#ifdef CONFIG_DUAL_TSP
	if (info->flip_status == FLIP_OPEN){
		dev_err(&info->client->dev,
			"%s: flip is opened, so resume is ignored.\n", __func__);
		return 0;
	}
#endif
	printk("Inside abov_tk_resume \n");

	dev_dbg(&info->client->dev, "%s: users=%d\n", __func__,
		   info->input_dev->users);

	if (info->wakeup_state){
		dev_info(dev, "%s: tkey wakeup\n", __func__);
		disable_irq_wake(info->irq);
		info->wakeup_state = false;
		disable_irq(info->irq);
		release_all_fingers(info);

		info->pdata->power(info->pdata, false);
		usleep_range(5 * 1000, 5 * 1000);
	}

	info->pdata->power(info->pdata, true);
	msleep(ABOV_RESET_DELAY);
	info->enabled = true;

	if (info->glovemode)
		{
			printk("********00:%d \n",abov_glove_mode_enable(client, CMD_GLOVE_ON));
		}

	abov_touchkey_led_status = CMD_LED_ON;

	printk("abov_touchled_cmd_reserved:%d,abov_touchkey_led_status=%d \n",
		abov_touchled_cmd_reserved,abov_touchkey_led_status);

	if (abov_touchled_cmd_reserved && \
		abov_touchkey_led_status == CMD_LED_ON) {
		abov_touchled_cmd_reserved = 0;
		led_data = abov_touchkey_led_status;

		ret = abov_tk_i2c_write(client, ABOV_BTNSTATUS, &led_data, 1);

		dev_info(&info->client->dev, "%s: LED reserved on\n", __func__);
	}

	enable_irq(info->irq);

	return 0;
}

#ifdef CONFIG_DUAL_TSP
static void abov_switching_tkey_work(struct work_struct *work)
{
	struct abov_tk_info *info =
			container_of(work, struct abov_tk_info,
			switching_work.work);

	if (info == NULL){
		pr_err("[tkey] %s: tkey info is null\n", __func__);
		return;
	}

	if (!info->probe_done){
		dev_err(&info->client->dev,
			"%s: touchkey probe is not done yet\n",
			__func__);
		return;
	}

	if(info->fw_update_state == FW_DOWNLOADING){
		dev_err(&info->client->dev,
			"%s: tk fw update is running. switching is ignored.\n",
			__func__);
		return;
	}

	dev_info(&info->client->dev,
		"%s : flip: %d(now) change to %d, tk_enabled : %d\n",
		__func__,info->flip_status, info->flip_status_current, info->enabled);

	if (info->flip_status != info->flip_status_current)
	{
		info->flip_status = info->flip_status_current;
		if (info->flip_status_current == FLIP_CLOSE) {
			dev_info(&info->client->dev,
				"%s : flip closed. tkey must be resumed.\n",
				__func__);
			abov_tk_resume(&info->client->dev);
		}
		else {
			dev_info(&info->client->dev,
				"%s : flip opened. tkey must be suspended\n",
				__func__);
			abov_tk_suspend(&info->client->dev);
		}
	}
}

static int abov_hall_ic_notify(struct notifier_block *nb,
				unsigned long flip_cover, void *v)
{
	struct abov_tk_info *info = abov_get_tkey_info();

	pr_info("%s %s\n", __func__, flip_cover ? "close" : "open");

	info->flip_status_current = flip_cover;
	schedule_delayed_work(&info->switching_work,
				msecs_to_jiffies(1));

	return 0;
}
#endif

static int abov_tk_input_open(struct input_dev *dev)
{
	struct abov_tk_info *info = input_get_drvdata(dev);
	dev_info(&info->client->dev, "%s: users=%d\n", __func__,
		   info->input_dev->users);

	abov_tk_resume(&info->client->dev);

	if (info->pinctrl) {
		int retval;
		retval = abov_pinctrl_configure(info, true);
		dev_info(&info->client->dev, "failed to put the pin in active state\n");
	}

	return 0;
}

static void abov_tk_input_close(struct input_dev *dev)
{
	struct abov_tk_info *info = input_get_drvdata(dev);
	dev_info(&info->client->dev, "%s: users=%d\n", __func__,
		   info->input_dev->users);

	abov_tk_suspend(&info->client->dev);

	if (info->pinctrl) {
		int retval;
		retval = abov_pinctrl_configure(info, false);
		dev_info(&info->client->dev, "failed to put the pin in suspend state\n");
	}

}

static const struct i2c_device_id abov_tk_id[] = {
	{ABOV_TK_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, abov_tk_id);

#ifdef CONFIG_OF
static struct of_device_id abov_match_table[] = {
	{ .compatible = "abov,abov_touchkey",},
	{ },
};
#else
#define abov_match_table NULL
#endif

static struct i2c_driver abov_tk_driver = {
	.probe = abov_tk_probe,
	.remove = abov_tk_remove,
	.shutdown = abov_tk_shutdown,
	.driver = {
		   .name = ABOV_TK_NAME,
		   .of_match_table = abov_match_table,
	},
	.id_table = abov_tk_id,
};

static int __init touchkey_init(void)
{
#ifdef CONFIG_SAMSUNG_LPM_MODE
//	if (poweroff_charging) {
//		pr_notice("%s : LPM Charging Mode!!\n", __func__);
//		return 0;
//	}
#endif

	return i2c_add_driver(&abov_tk_driver);
}

static void __exit touchkey_exit(void)
{
	i2c_del_driver(&abov_tk_driver);
}

module_init(touchkey_init);
module_exit(touchkey_exit);

/* Module information */
MODULE_AUTHOR("Samsung Electronics");
MODULE_DESCRIPTION("Touchkey driver for Abov MF16xx chip");
MODULE_LICENSE("GPL");
