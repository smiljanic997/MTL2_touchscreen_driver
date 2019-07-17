/*
 * MTL2 touchscreen driver
 *
 * Copyright (c) 2019 Slaven Smiljanic
 * 
 * 
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/ioport.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/gpio.h>
#include <linux/input/touchscreen.h>


// Custom bit manipulation macros
#define BIT_SET(a, b) ((a) |= (1UL << (b)))
#define BIT_CHECK(a, b) (!!((a) & (1UL << (b))))

#define DEVICE_NAME		"mtl2_touchscreen"
#define DEVICE_ADDRESS	0x38		// address obtained with i2cdetect tool

#define INT_GPIO_PIN 	4  // GPIO_04 will be used for interrupts

#define MTL2_MAX_X 		480
#define MTL2_MAX_Y 		800

// MTL2 Touchscreen registers:
#define DEVICE_MODE		0x00
#define GEST_ID			0x01
#define TD_STATUS		0x02
#define TOUCH1_XH		0x03
#define TOUCH1_XL		0x04
#define TOUCH1_YH		0x05
#define TOUCH1_YL		0x06
#define TOUCH2_XH		0x09
#define TOUCH2_XL		0x0A
#define TOUCH2_YH		0x0B
#define TOUCH2_YL		0x0C
#define TOUCH3_XH		0x0F
#define TOUCH3_XL		0x10
#define TOUCH3_YH		0x11
#define TOUCH3_YL		0x12
#define TOUCH4_XH		0x15
#define TOUCH4_XL		0x16
#define TOUCH4_YH		0x17
#define TOUCH4_YL		0x18
#define TOUCH5_XH		0x1B
#define TOUCH5_XL		0x1C
#define TOUCH5_YH		0x1D
#define TOUCH5_YL		0x1E


struct mtl2_touchscreen_data {
    struct i2c_client *client;
    struct input_dev *input;
};

/*
 * Utility function used to create touch coordinate, in a way that's
 * specified in datasheet.
 * 
 * */
static u16 create_coord(u8 msb, u8 lsb)
{	
	u16 coord = 0;
	int i;
	for(i = 0; i < 8; i++)
		if(BIT_CHECK(lsb, i))
			BIT_SET(coord, i);
	for(i = 8; i < 12; i++)
		if(BIT_CHECK(msb, i - 8))
			BIT_SET(coord, i);
	return coord;
}

static const struct i2c_device_id mtl2_touchscreen_id[] = {
    {DEVICE_NAME, DEVICE_ADDRESS},
    {},
};
MODULE_DEVICE_TABLE(i2c, mtl2_touchscreen_id);

static const struct i2c_board_info mtl2_touchscreen_board_info = {
	I2C_BOARD_INFO(DEVICE_NAME, DEVICE_ADDRESS)
};

struct mtl2_touchscreen_data data;

static int mtl2_irq = -1;

static irqreturn_t mtl2_touchscreen_irq(int irq, void* dev_id)
{		
	u16 x, y;
	
	u8 x_h, x_l;
	x_h = i2c_smbus_read_byte_data(data.client, TOUCH1_XH);
	x_l = i2c_smbus_read_byte_data(data.client, TOUCH1_XL);
	x = create_coord(x_h, x_l);

	u8 y_h, y_l;
	y_h = i2c_smbus_read_byte_data(data.client, TOUCH1_YH);
	y_l = i2c_smbus_read_byte_data(data.client, TOUCH1_YL);
	y = create_coord(y_h, y_l);	
	
	input_report_abs(data.input, ABS_X, x);
	input_report_abs(data.input, ABS_Y, y);	
	input_report_key(data.input, BTN_TOUCH, 1);
	input_sync(data.input);
	
	printk(KERN_CONT "%d, %d\n", x, y);
	
	return IRQ_HANDLED;
}


static int mtl2_touchscreen_probe(struct i2c_client *client,
				  const struct i2c_device_id *id)
{
    int error;
    
    struct input_dev *input;
        
	if(gpio_request(INT_GPIO_PIN, NULL))
	{
		dev_err(&client->dev, "GPIO request failure.");
		return -1;
	}
	
	if((mtl2_irq = gpio_to_irq(INT_GPIO_PIN)) < 0)
	{
		dev_err(&client->dev, "GPIO mapping to IQR failure.");
		return -1;
	}
	
	printk(KERN_INFO "Mapped int %d\n", mtl2_irq);
		
	if((devm_request_irq(&client->dev, mtl2_irq, mtl2_touchscreen_irq, 
		IRQF_TRIGGER_RISING, "mtl2_irq", (void*)&data)))
	{
		dev_err(&client->dev, "IRQ request failure.\n");
		return -ENODEV;
	}

    if(!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
    {
		dev_err(&client->dev, "i2c_check_functionality error\n");
		return -ENXIO;
    }	

    input = devm_input_allocate_device(&client->dev);
    if(!input)
    {
		return -ENOMEM;
	}
	data.input = input;

    input->name = "MTL2 Touchscreen";
    input->id.bustype = BUS_I2C;
    input->dev.parent = &client->dev;

    __set_bit(INPUT_PROP_DIRECT, input->propbit);
    input_set_capability(input, EV_KEY, BTN_TOUCH);
    input_set_abs_params(input, ABS_X, 0, MTL2_MAX_X, 0, 0);
    input_set_abs_params(input, ABS_Y, 0, MTL2_MAX_Y, 0, 0);
    
    input_set_drvdata(input, &data);
    
    error = input_register_device(data.input);
    if(error)
    {
		dev_err(&client->dev, "Failed to register input device. Error code: %d\n", error);
		return error;
	}
}

static struct i2c_driver mtl2_touchscreen_driver = {
    .driver = {
		.name = DEVICE_NAME
    },
    .probe    = mtl2_touchscreen_probe,
    .id_table = mtl2_touchscreen_id
};

static int __init mtl2_touchscreen_init(void)
{
	printk(KERN_CONT "Inserting module.");
	
	struct i2c_adapter *i2c_adapt;
	i2c_adapt = i2c_get_adapter(1); // i2c-1
	data.client = i2c_new_device(i2c_adapt, &mtl2_touchscreen_board_info);
	
	i2c_add_driver(&mtl2_touchscreen_driver);
	
	return 0;
}

static int __exit mtl2_touchscreen_exit(void)
{
	printk(KERN_CONT "Removing module.");
	
	disable_irq(mtl2_irq);
    	gpio_free(INT_GPIO_PIN);
	input_unregister_device(data.input);
	i2c_unregister_device(data.client);
	i2c_del_driver(&mtl2_touchscreen_driver);
}

module_init(mtl2_touchscreen_init);
module_exit(mtl2_touchscreen_exit);
MODULE_AUTHOR("Slaven Smiljanic");
MODULE_LICENSE("GPL");
