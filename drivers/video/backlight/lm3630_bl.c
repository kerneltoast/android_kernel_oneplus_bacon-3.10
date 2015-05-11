/************************************************************
* Copyright (c) 2013-2013 OPPO Mobile communication Corp.ltd.,
* VENDOR_EDIT
* Description: Simple driver for Texas Instruments LM3630 Backlight driver chip.
* Version    : 1.0
* Date       : 2013-12-09
* Author     : yangxinqin
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
**************************************************************/

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <linux/platform_data/lm3630_bl.h>
#include <mach/gpio.h>
#define REG_CTRL	0x00
#define REG_CONFIG	0x01
#define REG_BRT_A	0x03
#define REG_BRT_B	0x04
#define REG_INT_STATUS	0x09
#define REG_INT_EN	0x0A
#define REG_FAULT	0x0B
#define REG_PWM_OUTLOW	0x12
#define REG_PWM_OUTHIGH	0x13
#define REG_MAX		0x1F
//yanghai add
#define REG_MAXCU_A	0x05
#define REG_MAXCU_B	0x06
//yanghai add end
/* OPPO 2013-10-24 yxq Add begin for backlight info */
#define REG_REVISION 0x1F
/* OPPO 2013-10-24 yxq Add end */
#define INT_DEBOUNCE_MSEC	10

#ifdef VENDOR_EDIT
/* Xiaori.Yuan@Mobile Phone Software Dept.Driver, 2014/03/10  Add for flicker in low backlight */
static bool pwm_flag = true;
static int backlight_level;
static int pre_brightness=0;
int set_backlight_pwm(int state);
#endif /*VENDOR_EDIT*/

static struct lm3630_chip_data *lm3630_pchip;

#ifdef CONFIG_BL_REGISTER
enum lm3630_leds {
	BLED_ALL = 0,
	BLED_1,
	BLED_2
};

static const char * const bled_name[] = {
	[BLED_ALL] = "lm3630_bled",	/*Bank1 controls all string */
	[BLED_1] = "lm3630_bled1",	/*Bank1 controls bled1 */
	[BLED_2] = "lm3630_bled2",	/*Bank1 or 2 controls bled2 */
};
#endif 

struct lm3630_chip_data {
	struct device *dev;
	struct delayed_work work;
	int irq;
	struct workqueue_struct *irqthread;
	struct lm3630_platform_data *pdata;
	struct backlight_device *bled1;
	struct backlight_device *bled2;
	struct regmap *regmap;
};

int get_pcb_version(void)
{
	return 40;
}

/* initialize chip */
static int lm3630_chip_init(struct lm3630_chip_data *pchip)
{
	int ret;
	unsigned int reg_val;
	struct lm3630_platform_data *pdata = pchip->pdata;
	/*pwm control */
	reg_val = ((pdata->pwm_active & 0x01) << 2) | (pdata->pwm_ctrl & 0x03);
	ret = regmap_update_bits(pchip->regmap, REG_CONFIG, 0x07, reg_val);
	if (ret < 0)
		goto out;
    /* For normal mode, enable pwm control by Xinqin.Yang@PhoneSW.Driver, 2013/12/20 */
    //if (get_pcb_version() < HW_VERSION__20) { /* For Find7 */
        	ret = regmap_update_bits(pchip->regmap, REG_CONFIG, 0x01, 0x01);
        	if (ret < 0)
        		goto out;
   
    //}

#ifdef CONGIF_OPPO_CMCC_OPTR
    reg_val = 0x12; /* For 13077 CMCC */
    if ((get_pcb_version() >= 20)&&(get_pcb_version() < 30)) {/*liuyan add for N3*/	
        reg_val = 0x0F; /* For 13097 cmcctest */
    } else {
        reg_val = 0x12; /* For 13077 cmcc test */
    }
#else
    if ((get_pcb_version() >= 20)&&(get_pcb_version() < 30)) {/*liuyan add for N3*/
	
        reg_val = 0x12; /* For 13097 low power version */
    } else {
   
        reg_val = 0x16; /* For 13077 pvt panel */
    }
#endif

	regmap_write(pchip->regmap, REG_MAXCU_A, reg_val);
	regmap_write(pchip->regmap, REG_MAXCU_B, reg_val);
	/* bank control */
	reg_val = ((pdata->bank_b_ctrl & 0x01) << 1) |
			(pdata->bank_a_ctrl & 0x07)|0x18;//linear
	ret = regmap_update_bits(pchip->regmap, REG_CTRL, 0x1F, reg_val);
	if (ret < 0)
		goto out;

	ret = regmap_update_bits(pchip->regmap, REG_CTRL, 0x80, 0x00);
	if (ret < 0)
		goto out;
	printk("%s: bl_initvalue=%d\n", __func__,pdata->init_brt_led1);
	/* set initial brightness */
	if (pdata->bank_a_ctrl != BANK_A_CTRL_DISABLE) {
		printk("%s: bl_initvalue=%d,222\n", __func__,pdata->init_brt_led1);
		if (ret < 0)
			goto out;
	}
	if (pdata->bank_b_ctrl != BANK_B_CTRL_DISABLE) {
		ret = regmap_write(pchip->regmap,
				   REG_BRT_B, pdata->init_brt_led2);
		if (ret < 0)
			goto out;
	}

    ret = regmap_write(pchip->regmap,
				   REG_BRT_A, pdata->init_brt_led1);
    ret = regmap_write(pchip->regmap,
				   REG_BRT_A, pdata->init_brt_led1);
	return ret;

out:
	dev_err(pchip->dev, "i2c failed to access register\n");
	return ret;
}

/* interrupt handling */
static void lm3630_delayed_func(struct work_struct *work)
{
	int ret;
	unsigned int reg_val;
	struct lm3630_chip_data *pchip;

	pchip = container_of(work, struct lm3630_chip_data, work.work);

	ret = regmap_read(pchip->regmap, REG_INT_STATUS, &reg_val);
	if (ret < 0) {
		dev_err(pchip->dev,
			"i2c failed to access REG_INT_STATUS Register\n");
		return;
	}

	dev_info(pchip->dev, "REG_INT_STATUS Register is 0x%x\n", reg_val);
}

static irqreturn_t lm3630_isr_func(int irq, void *chip)
{
	int ret;
	struct lm3630_chip_data *pchip = chip;
	unsigned long delay = msecs_to_jiffies(INT_DEBOUNCE_MSEC);

	queue_delayed_work(pchip->irqthread, &pchip->work, delay);

	ret = regmap_update_bits(pchip->regmap, REG_CTRL, 0x80, 0x00);
	if (ret < 0)
		goto out;

	return IRQ_HANDLED;
out:
	dev_err(pchip->dev, "i2c failed to access register\n");
	return IRQ_HANDLED;
}

static int lm3630_intr_config(struct lm3630_chip_data *pchip)
{
	INIT_DELAYED_WORK(&pchip->work, lm3630_delayed_func);
	pchip->irqthread = create_singlethread_workqueue("lm3630-irqthd");
	if (!pchip->irqthread) {
		dev_err(pchip->dev, "create irq thread fail...\n");
		return -1;
	}
	if (request_threaded_irq
	    (pchip->irq, NULL, lm3630_isr_func,
	     IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "lm3630_irq", pchip)) {
		dev_err(pchip->dev, "request threaded irq fail..\n");
		return -1;
	}
	return 0;
}


#ifdef CONFIG_BL_REGISTER
static bool
set_intensity(struct backlight_device *bl, struct lm3630_chip_data *pchip)
{
	if (!pchip->pdata->pwm_set_intensity)
		return false;
	pchip->pdata->pwm_set_intensity(bl->props.brightness - 1,
					pchip->pdata->pwm_period);
	return true;
}

/* update and get brightness */
static int lm3630_bank_a_update_status(struct backlight_device *bl)
{
	int ret;
	struct lm3630_chip_data *pchip = bl_get_data(bl);
	enum lm3630_pwm_ctrl pwm_ctrl = pchip->pdata->pwm_ctrl;

	/* brightness 0 means disable */
	if (!bl->props.brightness) {

		ret = regmap_update_bits(pchip->regmap, REG_CTRL, 0x04, 0x00);
		if (ret < 0)
			goto out;
		return bl->props.brightness;
	}


	/* pwm control */
	if (pwm_ctrl == PWM_CTRL_BANK_A || pwm_ctrl == PWM_CTRL_BANK_ALL) {
		if (!set_intensity(bl, pchip))
			dev_err(pchip->dev, "No pwm control func. in plat-data\n");
	} else {


		/* i2c control */
		ret = regmap_update_bits(pchip->regmap, REG_CTRL, 0x80, 0x00);
		if (ret < 0)
			goto out;
		mdelay(1);
		ret = regmap_write(pchip->regmap,
				   REG_BRT_A, bl->props.brightness - 1);
		if (ret < 0)
			goto out;
	}
	return bl->props.brightness;
out:
	dev_err(pchip->dev, "i2c failed to access REG_CTRL\n");
	return bl->props.brightness;
}

static int lm3630_bank_a_get_brightness(struct backlight_device *bl)
{
	unsigned int reg_val;
	int brightness, ret;
	struct lm3630_chip_data *pchip = bl_get_data(bl);
	enum lm3630_pwm_ctrl pwm_ctrl = pchip->pdata->pwm_ctrl;
	if (pwm_ctrl == PWM_CTRL_BANK_A || pwm_ctrl == PWM_CTRL_BANK_ALL) {
		ret = regmap_read(pchip->regmap, REG_PWM_OUTHIGH, &reg_val);
		if (ret < 0)
			goto out;
		brightness = reg_val & 0x01;
		ret = regmap_read(pchip->regmap, REG_PWM_OUTLOW, &reg_val);
		if (ret < 0)
			goto out;
		brightness = ((brightness << 8) | reg_val) + 1;
	} else {
		ret = regmap_update_bits(pchip->regmap, REG_CTRL, 0x80, 0x00);
		if (ret < 0)
			goto out;
		mdelay(1);
		ret = regmap_read(pchip->regmap, REG_BRT_A, &reg_val);
		if (ret < 0)
			goto out;
		brightness = reg_val + 1;
	}
	bl->props.brightness = brightness;
	return bl->props.brightness;
out:
	dev_err(pchip->dev, "i2c failed to access register\n");
	return 0;
}


static const struct backlight_ops lm3630_bank_a_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = lm3630_bank_a_update_status,
	.get_brightness = lm3630_bank_a_get_brightness,
};

static int lm3630_bank_b_update_status(struct backlight_device *bl)
{
	int ret;
	struct lm3630_chip_data *pchip = bl_get_data(bl);
	enum lm3630_pwm_ctrl pwm_ctrl = pchip->pdata->pwm_ctrl;
	if (pwm_ctrl == PWM_CTRL_BANK_B || pwm_ctrl == PWM_CTRL_BANK_ALL) {
		if (!set_intensity(bl, pchip))
			dev_err(pchip->dev,
				"no pwm control func. in plat-data\n");
	} else {
		ret = regmap_update_bits(pchip->regmap, REG_CTRL, 0x80, 0x00);
		if (ret < 0)
			goto out;
		mdelay(1);
		ret = regmap_write(pchip->regmap,
				   REG_BRT_B, bl->props.brightness - 1);
	}
	return bl->props.brightness;
out:
	dev_err(pchip->dev, "i2c failed to access register\n");
	return bl->props.brightness;
}

static int lm3630_bank_b_get_brightness(struct backlight_device *bl)
{
	unsigned int reg_val;
	int brightness, ret;
	struct lm3630_chip_data *pchip = bl_get_data(bl);
	enum lm3630_pwm_ctrl pwm_ctrl = pchip->pdata->pwm_ctrl;
	if (pwm_ctrl == PWM_CTRL_BANK_B || pwm_ctrl == PWM_CTRL_BANK_ALL) {
		ret = regmap_read(pchip->regmap, REG_PWM_OUTHIGH, &reg_val);
		if (ret < 0)
			goto out;
		brightness = reg_val & 0x01;
		ret = regmap_read(pchip->regmap, REG_PWM_OUTLOW, &reg_val);
		if (ret < 0)
			goto out;
		brightness = ((brightness << 8) | reg_val) + 1;
	} else {
		ret = regmap_update_bits(pchip->regmap, REG_CTRL, 0x80, 0x00);
		if (ret < 0)
			goto out;
		mdelay(1);
		ret = regmap_read(pchip->regmap, REG_BRT_B, &reg_val);
		if (ret < 0)
			goto out;
		brightness = reg_val + 1;
	}
	bl->props.brightness = brightness;

	return bl->props.brightness;
out:
	dev_err(pchip->dev, "i2c failed to access register\n");
	return bl->props.brightness;
}


static const struct backlight_ops lm3630_bank_b_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = lm3630_bank_b_update_status,
	.get_brightness = lm3630_bank_b_get_brightness,
};


static int lm3630_backlight_register(struct lm3630_chip_data *pchip,
				     enum lm3630_leds ledno)
{
	const char *name = bled_name[ledno];
	struct backlight_properties props;
	struct lm3630_platform_data *pdata = pchip->pdata;
	props.type = BACKLIGHT_RAW;
	switch (ledno) {
	case BLED_1:
	case BLED_ALL:
		props.brightness = pdata->init_brt_led1;
		props.max_brightness = pdata->max_brt_led1;
		pchip->bled1 =
		    backlight_device_register(name, pchip->dev, pchip,
					      &lm3630_bank_a_ops, &props);
		if (IS_ERR(pchip->bled1))
			return PTR_ERR(pchip->bled1);
		break;
	case BLED_2:
		props.brightness = pdata->init_brt_led2;
		props.max_brightness = pdata->max_brt_led2;
		pchip->bled2 =
		    backlight_device_register(name, pchip->dev, pchip,
					      &lm3630_bank_b_ops, &props);
		if (IS_ERR(pchip->bled2))
			return PTR_ERR(pchip->bled2);
		break;
	}
	return 0;
}


static void lm3630_backlight_unregister(struct lm3630_chip_data *pchip)
{
	if (pchip->bled1)
		backlight_device_unregister(pchip->bled1);
	if (pchip->bled2)
		backlight_device_unregister(pchip->bled2);
}
#else

/* update and get brightness */
 int lm3630_bank_a_update_status(u32 bl_level)
{
	int ret;
	struct lm3630_chip_data *pchip = lm3630_pchip;
	pr_debug("%s: bl=%d\n", __func__,bl_level);
#ifdef VENDOR_EDIT

/* Xiaori.Yuan@Mobile Phone Software Dept.Driver, 2014/04/28  Add for add log for 14001 black screen */
		if(pre_brightness == 0)
			{pr_err("%s set brightness :  %d \n",__func__,bl_level);}
		pre_brightness=bl_level;
#endif /*VENDOR_EDIT*/

	if(!pchip){
		dev_err(pchip->dev, "lm3630_bank_a_update_status pchip is null\n");
		return -ENOMEM;
		}
    if (!pchip->regmap || !lm3630_pchip->regmap) {
        pr_err("%s YXQ pchip->regmap is NULL.\n", __func__);
        return bl_level;
    }

	/* brightness 0 means disable */
	if (!bl_level) {
        ret = regmap_write(lm3630_pchip->regmap, REG_BRT_A, 0);
		ret = regmap_update_bits(pchip->regmap, REG_CTRL, 0x80, 0x80);
		if (ret < 0)
			goto out;
		return bl_level;
	}
	/* pwm control */
	//bl_level=255;
		/* i2c control */
		ret = regmap_update_bits(pchip->regmap, REG_CTRL, 0x80, 0x00);
		if (ret < 0)
			goto out;
		mdelay(1);
#ifndef VENDOR_EDIT


/* Xiaori.Yuan@Mobile Phone Software Dept.Driver, 2014/04/24  Modify for backlight flick when disable pwm */
		ret = regmap_write(pchip->regmap,
				   REG_BRT_A, bl_level);
#else /*VENDOR_EDIT*/
		if(bl_level>20)
			{
	
			ret = regmap_write(pchip->regmap,
				   REG_BRT_A, bl_level);
			}
		else if((get_pcb_version() < 20)||(get_pcb_version() >=30))/*liuyan add 30 for N3*/
			{

		
			ret = regmap_write(pchip->regmap,
				   REG_BRT_A, 2+(bl_level-1)*7/18);
			}
		else
			{
	
			ret = regmap_write(pchip->regmap,
				   REG_BRT_A, 2+(bl_level-1)*9/18);
			}
#endif /*VENDOR_EDIT*/
		if (ret < 0)
			goto out;
#ifdef VENDOR_EDIT


/* Xiaori.Yuan@Mobile Phone Software Dept.Driver, 2014/03/10  Add for flicker in low backlight */
		backlight_level =  bl_level;
		if(bl_level <= 0x14 && pwm_flag==true)
			set_backlight_pwm(0);

#endif /*VENDOR_EDIT*/

	return bl_level;
out:
	dev_err(pchip->dev, "i2c failed to access REG_CTRL\n");
	return bl_level;
}

#endif

static const struct regmap_config lm3630_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = REG_MAX,
};

#ifdef CONFIG_OF

static int lm3630_dt(struct device *dev, struct lm3630_platform_data *pdata)
{
	u32 temp_val;
	int rc;
	struct device_node *np = dev->of_node;	
//		dev_err(dev, "yanghai read \n");

		rc = of_property_read_u32(np, "ti,bank-a-ctrl", &temp_val);
		if (rc) {
			dev_err(dev, "Unable to read bank-a-ctrl\n");
			pdata->bank_a_ctrl=BANK_A_CTRL_ALL;
		} else{
			pdata->bank_a_ctrl=temp_val;
//			printk("%s: bank_a_ctrl=%d\n", __func__,pdata->bank_a_ctrl);
			}
		rc = of_property_read_u32(np, "ti,init-brt-ed1", &temp_val);
		if (rc) {
			dev_err(dev, "Unable to readinit-brt-ed1\n");
			pdata->init_brt_led1=200;
		} else{
			pdata->init_brt_led1=temp_val;
			}
		rc = of_property_read_u32(np, "ti,init-brt-led2", &temp_val);
		if (rc) {
			dev_err(dev, "Unable to read init-brt-led2\n");
			pdata->init_brt_led2=200;
		} else{
			pdata->init_brt_led2=temp_val;
			}
		rc = of_property_read_u32(np, "ti,max-brt-led1", &temp_val);
		if (rc) {
			dev_err(dev, "Unable to read max-brt-led1\n");
			pdata->max_brt_led1=255;
		} else{
			pdata->max_brt_led1=temp_val;
			}
		rc = of_property_read_u32(np, "ti,max-brt-led2", &temp_val);
		if (rc) {
			dev_err(dev, "Unable to read max-brt-led2\n");
			pdata->max_brt_led2=255;
		} else{
			pdata->max_brt_led2=temp_val;
			}
		rc = of_property_read_u32(np, "ti,pwm-active", &temp_val);
		if (rc) {
			dev_err(dev, "Unable to read pwm-active\n");
			pdata->pwm_active=PWM_ACTIVE_HIGH;
		} else{
			pdata->pwm_active=temp_val;
			}
		rc = of_property_read_u32(np, "ti,pwm-ctrl", &temp_val);
		if (rc) {
			dev_err(dev, "Unable to read pwm-ctrl\n");
			pdata->pwm_ctrl=PWM_CTRL_DISABLE;
		} else{
			pdata->pwm_ctrl=temp_val;
			}
		rc = of_property_read_u32(np, "ti,pwm-period", &temp_val);
		if (rc) {
			dev_err(dev, "Unable to read pwm-period\n");
			pdata->pwm_period=255;
		} else{
			pdata->pwm_period=temp_val;
			}
#if 0														
	pdata->bank_b_ctrl=BANK_B_CTRL_DISABLE;
	pdata->init_brt_led1=200;
	pdata->init_brt_led2=200;
	pdata->max_brt_led1=255;
	pdata->max_brt_led2=255;
	pdata->pwm_active=PWM_ACTIVE_HIGH;
	pdata->pwm_ctrl=PWM_CTRL_DISABLE;
	pdata->pwm_period=255;
#endif
	return 0;
}
#else
static int lm3630_dt(struct device *dev, struct lm3630_platform_data *pdata)
{
	return -ENODEV;
}
#endif

#ifdef CONFIG_VENDOR_EDIT
/* OPPO zhanglong add 2013-08-30 for ftm test LCD backlight */
static ssize_t ftmbacklight_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
    int level;
    if (!count)
		return -EINVAL;
#if 0
    /* this function is for ftm mode, it doesn't work when normal boot */
    if(get_boot_mode() == MSM_BOOT_MODE__FACTORY) {
        level = simple_strtoul(buf, NULL, 10);

        lm3630_bank_a_update_status(level);
    }
#endif
    level = simple_strtoul(buf, NULL, 10);
    lm3630_bank_a_update_status(level);
    
    return count;
}
    DEVICE_ATTR(ftmbacklight, 0644, NULL, ftmbacklight_store);
/* OPPO zhanglong add 2013-08-30 for ftm test LCD backlight end */
#endif //CONFIG_VENDOR_EDIT

#define LM3630_ENABLE_GPIO   91
static int lm3630_probe(struct i2c_client *client,
				  const struct i2c_device_id *id)
{
	struct lm3630_platform_data *pdata = client->dev.platform_data;
	struct lm3630_chip_data *pchip;
	int ret;
/* OPPO 2013-10-24 yxq Add end */
	printk("%s:yanghai.\n", __func__);
	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
			sizeof(struct lm3630_platform_data), GFP_KERNEL);
		if (!pdata) {
			dev_err(&client->dev, "Failed to allocate memory\n");
			return -ENOMEM;
		}

		ret = lm3630_dt(&client->dev, pdata);
		if (ret)
			return ret;
	} else
		pdata = client->dev.platform_data;
	
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "fail : i2c functionality check...\n");
		return -EOPNOTSUPP;
	}

	if (pdata == NULL) {
		dev_err(&client->dev, "fail : no platform data.\n");
		return -ENODATA;
	}

	pchip = devm_kzalloc(&client->dev, sizeof(struct lm3630_chip_data),
			     GFP_KERNEL);
	if (!pchip)
		return -ENOMEM;
	lm3630_pchip=pchip;
	
	pchip->pdata = pdata;
	pchip->dev = &client->dev;
//HW enable 
	ret = gpio_request(LM3630_ENABLE_GPIO, "lm3528_enable");
	if (ret) {
		pr_err("lm3528_enable gpio_request failed: %d\n", ret);
		goto err_gpio_req;
	}
	ret = gpio_direction_output(LM3630_ENABLE_GPIO, 1);
	if (ret) {
		pr_err("%s: unable to enable!!!!!!!!!!!!\n", __func__);
		goto err_gpio_req;
	}
//HW enable
	pchip->regmap = devm_regmap_init_i2c(client, &lm3630_regmap);
	if (IS_ERR(pchip->regmap)) {
		ret = PTR_ERR(pchip->regmap);
		dev_err(&client->dev, "fail : allocate register map: %d\n",
			ret);
		return ret;
	}
	i2c_set_clientdata(client, pchip);

	/* chip initialize */
	ret = lm3630_chip_init(pchip);
/* OPPO 2013-10-24 yxq Add begin for reason */

/* OPPO 2013-10-24 yxq Add end */
	if (ret < 0) {
		dev_err(&client->dev, "fail : init chip\n");
		goto err_chip_init;
	}
#ifdef CONFIG_BL_REGISTER
	switch (pdata->bank_a_ctrl) {
	case BANK_A_CTRL_ALL:
		ret = lm3630_backlight_register(pchip, BLED_ALL);
		pdata->bank_b_ctrl = BANK_B_CTRL_DISABLE;
		break;
	case BANK_A_CTRL_LED1:
		ret = lm3630_backlight_register(pchip, BLED_1);
		break;
	case BANK_A_CTRL_LED2:
		ret = lm3630_backlight_register(pchip, BLED_2);
		pdata->bank_b_ctrl = BANK_B_CTRL_DISABLE;
		break;
	default:
		break;
	}

	if (ret < 0)
		goto err_bl_reg;

	if (pdata->bank_b_ctrl && pchip->bled2 == NULL) {
		ret = lm3630_backlight_register(pchip, BLED_2);
		if (ret < 0)
			goto err_bl_reg;
	}
#endif

	/* interrupt enable  : irq 0 is not allowed for lm3630 */
	pchip->irq = client->irq;
	printk("%s:yanghai client irq =%d\n", __func__,client->irq);
	pchip->irq=0;
	if (pchip->irq)
		lm3630_intr_config(pchip);
	printk("%s:yanghai----\n", __func__);
	dev_err(&client->dev, "LM3630 backlight register OK.\n");

#ifdef CONFIG_VENDOR_EDIT

/* OPPO zhanglong add 2013-08-30 for ftm test LCD backlight */
    ret = device_create_file(&client->dev, &dev_attr_ftmbacklight);
	if (ret < 0) {
		dev_err(&client->dev, "failed to create node ftmbacklight\n");
	}
/* OPPO zhanglong add 2013-08-30 for ftm test LCD backlight end */
#endif //CONFIG_VENDOR_EDIT

	return 0;
#ifdef CONFIG_BL_REGISTER
err_bl_reg:
	dev_err(&client->dev, "fail : backlight register.\n");
	lm3630_backlight_unregister(pchip);
#endif	
err_chip_init:
	return ret;

err_gpio_req:
	if (gpio_is_valid(LM3630_ENABLE_GPIO))
		gpio_free(LM3630_ENABLE_GPIO);
	return ret;
	
}

static int lm3630_remove(struct i2c_client *client)
{
	int ret;
	struct lm3630_chip_data *pchip = i2c_get_clientdata(client);
#ifdef CONFIG_VENDOR_EDIT
/* OPPO zhanglong add 2013-08-30 for ftm test LCD backlight */
    device_remove_file(&client->dev, &dev_attr_ftmbacklight);
/* OPPO zhanglong add 2013-08-30 for ftm test LCD backlight end */
#endif //CONFIG_VENDOR_EDIT

	ret = regmap_write(pchip->regmap, REG_BRT_A, 0);
	if (ret < 0)
		dev_err(pchip->dev, "i2c failed to access register\n");

	ret = regmap_write(pchip->regmap, REG_BRT_B, 0);
	if (ret < 0)
		dev_err(pchip->dev, "i2c failed to access register\n");
#ifdef CONFIG_BL_REGISTER
	lm3630_backlight_unregister(pchip);
#endif
	if (gpio_is_valid(LM3630_ENABLE_GPIO))
		gpio_free(LM3630_ENABLE_GPIO);
	
	if (pchip->irq) {
		free_irq(pchip->irq, pchip);
		flush_workqueue(pchip->irqthread);
		destroy_workqueue(pchip->irqthread);
	}
	return 0;
}

static const struct i2c_device_id lm3630_id[] = {
	{LM3630_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, lm3630_id);

static int lm3630_suspend(struct i2c_client *client, pm_message_t mesg)
{
	int rc ;
	pr_err("%s:backlight suspend.\n", __func__);
    rc = regmap_write(lm3630_pchip->regmap, REG_BRT_A, 0);
	rc  = regmap_update_bits(lm3630_pchip->regmap, REG_CTRL, 0x80, 0x80);
	if (rc  < 0)
	{
		pr_err("%s: unable to shotdown !!!!!!!!!!!!\n", __func__);
	}
//	rc = gpio_direction_output(LM3630_ENABLE_GPIO, 0);
//	if (rc) {
//		pr_err("%s: unable to enable!!!!!!!!!!!!\n", __func__);
//		return rc;
//	}

	return 0;
}
static int lm3630_resume(struct i2c_client *client)
{
	int rc ;
	pr_err("%s: backlight resume.\n", __func__);
    rc = regmap_write(lm3630_pchip->regmap, REG_BRT_A, 0);
	regmap_update_bits(lm3630_pchip->regmap, REG_CONFIG, 0x04, 0x00);
	rc  = regmap_update_bits(lm3630_pchip->regmap, REG_CTRL, 0x80, 0x00);
	if (rc  < 0)
	{
		pr_err("%s: unable to shotdown !!!!!!!!!!!!\n", __func__);
	}	
//	rc = gpio_direction_output(LM3630_ENABLE_GPIO, 1);
//	if (rc) {
//		pr_err("%s: unable to enable!!!!!!!!!!!!\n", __func__);
//		return rc;
//	}

	return 0;
}

#ifdef VENDOR_EDIT
/* Xiaori.Yuan@Mobile Phone Software Dept.Driver, 2014/05/22  Add for reduce current when charge */
void lm3630_reduce_max_current(void)
{
	if(lm3630_pchip == NULL) return;
	regmap_write(lm3630_pchip->regmap, REG_MAXCU_A, 0x0D);
	regmap_write(lm3630_pchip->regmap, REG_MAXCU_B, 0x0D);
}

void lm3630_recover_max_current(void)
{

	if(lm3630_pchip == NULL) return;
	regmap_write(lm3630_pchip->regmap, REG_MAXCU_A, 0x12);
	regmap_write(lm3630_pchip->regmap, REG_MAXCU_B, 0x12);
}
#endif /*VENDOR_EDIT*/



#ifdef VENDOR_EDIT
/* Xiaori.Yuan@Mobile Phone Software Dept.Driver, 2014/02/17  Add for set cabc */
int set_backlight_pwm(int state)
{
    int rc = 0;
	//if (get_pcb_version() < HW_VERSION__20) { /* For Find7 */
			if( state == 1 && backlight_level <= 0x14 ) return rc;
        	if(state == 1)
    		{
    	
       			 rc = regmap_update_bits(lm3630_pchip->regmap, REG_CONFIG, 0x01, 0x01);
				 pwm_flag = true;
   		    }
   			else
   			{
   			
    		     rc = regmap_update_bits(lm3630_pchip->regmap, REG_CONFIG, 0x01, 0x00);
				 pwm_flag = false;
  			}
    //}
    return rc;
}
#endif /*VENDOR_EDIT*/


#ifdef CONFIG_OF
static struct of_device_id lm3630_table[] = {
	{ .compatible = "ti,lm3630",},
	{ },
};
#else
#define lm3630_table NULL
#endif

static struct i2c_driver lm3630_i2c_driver = {
	.driver = {
		  .name = LM3630_NAME,
		  .owner	= THIS_MODULE,
		  .of_match_table = lm3630_table,		   	
		   },
	.probe = lm3630_probe,
	.remove = lm3630_remove,
	.suspend = lm3630_suspend,
	.resume = lm3630_resume,
	.id_table = lm3630_id,
};

module_i2c_driver(lm3630_i2c_driver);

MODULE_DESCRIPTION("Texas Instruments Backlight driver for LM3630");
MODULE_AUTHOR("G.Shark Jeong <gshark.jeong@gmail.com>");
MODULE_AUTHOR("Daniel Jeong <daniel.jeong@ti.com>");
MODULE_LICENSE("GPL v2");

