/*
 * Copyright (C) 2012 Broadcom Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/rfkill.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/poll.h>
#include <linux/version.h>

//#include <linux/platform_data/bcm2079x.h>
#include "bcm2079x.h"

/* do not change below */
#define MAX_BUFFER_SIZE		780

/* Read data */
#define PACKET_HEADER_SIZE_NCI	(4)
#define PACKET_HEADER_SIZE_HCI	(3)
#define PACKET_TYPE_NCI		(16)
#define PACKET_TYPE_HCIEV	(4)
#define MAX_PACKET_SIZE		(PACKET_HEADER_SIZE_NCI + 255)

struct bcm2079x_dev {
	wait_queue_head_t	read_wq;
	struct mutex		read_mutex;
	struct i2c_client	*client;
	struct miscdevice	bcm2079x_device;
	unsigned int		wake_gpio;
	unsigned int		en_gpio;
	unsigned int		irq_gpio;
	bool			irq_enabled;
	spinlock_t		irq_enabled_lock;
	unsigned int		count_irq;
	struct rfkill		*rfkill_dev;
	bool			rfkill_blocked;
	bool			was_enabled;
};

static void bcm2079x_init_stat(struct bcm2079x_dev *bcm2079x_dev)
{
	bcm2079x_dev->count_irq = 0;
}

static void bcm2079x_disable_irq(struct bcm2079x_dev *bcm2079x_dev)
{
	unsigned long flags;
	spin_lock_irqsave(&bcm2079x_dev->irq_enabled_lock, flags);
	if (bcm2079x_dev->irq_enabled) {
		disable_irq_nosync(bcm2079x_dev->client->irq);
		bcm2079x_dev->irq_enabled = false;
	}
	spin_unlock_irqrestore(&bcm2079x_dev->irq_enabled_lock, flags);
}

static void bcm2079x_enable_irq(struct bcm2079x_dev *bcm2079x_dev)
{
	unsigned long flags;
	spin_lock_irqsave(&bcm2079x_dev->irq_enabled_lock, flags);
	if (!bcm2079x_dev->irq_enabled) {
		bcm2079x_dev->irq_enabled = true;
		enable_irq(bcm2079x_dev->client->irq);
	}
	spin_unlock_irqrestore(&bcm2079x_dev->irq_enabled_lock, flags);
}

static void set_client_addr(struct bcm2079x_dev *bcm2079x_dev, int addr)
{
	struct i2c_client *client = bcm2079x_dev->client;
	dev_info(&client->dev,
		"Set client device address from 0x%04X flag = "\
		"%02x, to  0x%04X\n",
		client->addr, client->flags, addr);
		client->addr = addr;
		if (addr < 0x80)
			client->flags &= ~I2C_CLIENT_TEN;
		else
			client->flags |= I2C_CLIENT_TEN;
}

static irqreturn_t bcm2079x_dev_irq_handler(int irq, void *dev_id)
{
	struct bcm2079x_dev *bcm2079x_dev = dev_id;
	unsigned long flags;

	spin_lock_irqsave(&bcm2079x_dev->irq_enabled_lock, flags);
	bcm2079x_dev->count_irq++;
	spin_unlock_irqrestore(&bcm2079x_dev->irq_enabled_lock, flags);
	wake_up(&bcm2079x_dev->read_wq);

	return IRQ_HANDLED;
}

static unsigned int bcm2079x_dev_poll(struct file *filp, poll_table *wait)
{
	struct bcm2079x_dev *bcm2079x_dev = filp->private_data;
	unsigned int mask = 0;
	unsigned long flags;

	poll_wait(filp, &bcm2079x_dev->read_wq, wait);

	spin_lock_irqsave(&bcm2079x_dev->irq_enabled_lock, flags);
	if (bcm2079x_dev->count_irq > 0) {
		bcm2079x_dev->count_irq--;
		mask |= POLLIN | POLLRDNORM;
	}
	spin_unlock_irqrestore(&bcm2079x_dev->irq_enabled_lock, flags);

	return mask;
}

static ssize_t bcm2079x_dev_read(struct file *filp, char __user *buf,
					size_t count, loff_t *offset)
{
	struct bcm2079x_dev *bcm2079x_dev = filp->private_data;
	unsigned char tmp[MAX_BUFFER_SIZE];
	int total, len, ret;

	total = 0;
	len = 0;

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	mutex_lock(&bcm2079x_dev->read_mutex);

	/** Read the first 4 bytes to include the length of NCI or HCI packet.
	**/
	ret = i2c_master_recv(bcm2079x_dev->client, tmp, 4);
	if (ret == 4) {
		total = ret;
		/** First byte is the packet type
		**/
		switch (tmp[0]) {
		case PACKET_TYPE_NCI:
			len = tmp[PACKET_HEADER_SIZE_NCI-1];
			break;

		case PACKET_TYPE_HCIEV:
			len = tmp[PACKET_HEADER_SIZE_HCI-1];
			/*If payload is 0, decrement total size (from 4 to 3) */
			if (len == 0)
				total--;
			/*Otherwise, discount payload length byte*/
			else
				len--;
			break;

		default:
			len = 0;/*Unknown packet byte */
			break;
		} /* switch*/

		/** make sure full packet fits in the buffer
		**/
		if (len > 0 && (len + total) <= count) {
			/** read the remainder of the packet.
			**/
			ret = i2c_master_recv(bcm2079x_dev->client, tmp+total,
				len);
			if (ret == len)
				total += len;
		} /* if */
	} /* if */

	mutex_unlock(&bcm2079x_dev->read_mutex);

	if (total > count || copy_to_user(buf, tmp, total)) {
		dev_err(&bcm2079x_dev->client->dev,
			"failed to copy to user space, total = %d\n", total);
		total = -EFAULT;
	}

	return total;
}

static ssize_t bcm2079x_dev_write(struct file *filp, const char __user *buf,
					size_t count, loff_t *offset)
{
	struct bcm2079x_dev *bcm2079x_dev = filp->private_data;
	char tmp[MAX_BUFFER_SIZE];
	int ret;

	if (count > MAX_BUFFER_SIZE) {
		dev_err(&bcm2079x_dev->client->dev, "out of memory\n");
		return -ENOMEM;
	}

	if (copy_from_user(tmp, buf, count)) {
		dev_err(&bcm2079x_dev->client->dev,
			"failed to copy from user space\n");
		return -EFAULT;
	}

	mutex_lock(&bcm2079x_dev->read_mutex);
	/* Write data */

	ret = i2c_master_send(bcm2079x_dev->client, tmp, count);
	if (ret != count) {
		dev_err(&bcm2079x_dev->client->dev,
			"failed to write %d\n", ret);
		ret = -EIO;
	}
	mutex_unlock(&bcm2079x_dev->read_mutex);

	return ret;
}

static int bcm2079x_dev_open(struct inode *inode, struct file *filp)
{
	int ret = 0;

	struct bcm2079x_dev *bcm2079x_dev = container_of(filp->private_data,
		struct bcm2079x_dev,
		bcm2079x_device);
	filp->private_data = bcm2079x_dev;
	bcm2079x_init_stat(bcm2079x_dev);
	bcm2079x_enable_irq(bcm2079x_dev);
	dev_info(&bcm2079x_dev->client->dev,
		 "%d,%d\n", imajor(inode), iminor(inode));

	return ret;
}

static long bcm2079x_dev_unlocked_ioctl(struct file *filp,
					 unsigned int cmd, unsigned long arg)
{
	struct bcm2079x_dev *bcm2079x_dev = filp->private_data;

	switch (cmd) {
	case BCMXXC_POWER_CTL:
		gpio_set_value(bcm2079x_dev->en_gpio, arg);
		break;
	case BCMXXC_WAKE_CTL:
		if (bcm2079x_dev->wake_gpio != 0)
			gpio_set_value(bcm2079x_dev->wake_gpio, arg);
		break;
	case BCMXXC_SET_ADDR:
		set_client_addr(bcm2079x_dev, arg);
		break;
	default:
		dev_err(&bcm2079x_dev->client->dev,
			"%s, unknown cmd (%x, %lx)\n", __func__, cmd, arg);
		return -ENOSYS;
	}

	return 0;
}

static const struct file_operations bcm2079x_dev_fops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.poll = bcm2079x_dev_poll,
	.read = bcm2079x_dev_read,
	.write = bcm2079x_dev_write,
	.open = bcm2079x_dev_open,
	.unlocked_ioctl = bcm2079x_dev_unlocked_ioctl
};

static int bcm2079x_i2c_acpi_config(struct bcm2079x_dev *bcm2079x_dev)
{
        struct i2c_client *client = bcm2079x_dev->client;
        struct gpio_desc *gpiod_en, *gpiod_wake;

        gpiod_en = gpiod_get_index(&client->dev, NULL, 0, GPIOD_OUT_LOW);

        if (IS_ERR(gpiod_en)) {
                dev_err(&client->dev, "No en GPIO\n");
                return -EINVAL;
        } else {
	        bcm2079x_dev->en_gpio = desc_to_gpio(gpiod_en);
	        gpiod_put(gpiod_en);
        	dev_info(&client->dev, "got en GPIO 0x%02x\n", bcm2079x_dev->en_gpio);
	}

        gpiod_wake = gpiod_get_index(&client->dev, NULL, 1, GPIOD_OUT_LOW);

        if (IS_ERR(gpiod_wake)) {
                dev_err(&client->dev, "No wake GPIO, ignoring\n");
                bcm2079x_dev->wake_gpio = 0;
                // return -EINVAL;
        } else {
	        bcm2079x_dev->wake_gpio = desc_to_gpio(gpiod_wake);
	        gpiod_put(gpiod_wake);
        	dev_info(&client->dev, "got wake GPIO 0x%02x\n", bcm2079x_dev->wake_gpio);
	}

        return 0;
}

static int rfkill_bcm2079x_set_power(void *data, bool blocked)
{
	struct bcm2079x_dev *bcm2079x_dev = (struct bcm2079x_dev *)data;

	if (blocked) {
		if (!bcm2079x_dev->rfkill_blocked) {
			bcm2079x_dev->was_enabled = gpio_get_value(bcm2079x_dev->en_gpio);
			gpio_set_value(bcm2079x_dev->en_gpio, 0);
		}
		bcm2079x_dev->rfkill_blocked = blocked;
	} else {
		if (bcm2079x_dev->rfkill_blocked) {
			if (bcm2079x_dev->was_enabled) {
				gpio_set_value(bcm2079x_dev->en_gpio, 1);
				bcm2079x_dev->was_enabled = 0;
			}
		}
		bcm2079x_dev->rfkill_blocked = blocked;
	}

	return 0;
}

static const struct rfkill_ops rfkill_bcm2079x_ops = {
	.set_block = rfkill_bcm2079x_set_power,
};

static int bcm2079x_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	int ret;
	struct bcm2079x_platform_data *platform_data;
	struct bcm2079x_dev *bcm2079x_dev;

	platform_data = client->dev.platform_data;

	dev_info(&client->dev, "%s, probing bcm2079x driver flags = %x irq=0x%02x\n",
		__func__, client->flags, client->irq);

	bcm2079x_dev = kzalloc(sizeof(*bcm2079x_dev), GFP_KERNEL);
	if (bcm2079x_dev == NULL) {
		dev_err(&client->dev,
			"failed to allocate memory for module data\n");
		ret = -ENOMEM;
		goto err_exit;
	}
	bcm2079x_dev->client = client;

	if (platform_data == NULL) {
		dev_err(&client->dev, "pdata==NULL, trying devm\n");
		if (bcm2079x_i2c_acpi_config(bcm2079x_dev)) {
			return -ENODEV;
		}
	} else {
		bcm2079x_dev->wake_gpio = platform_data->wake_gpio;
		bcm2079x_dev->irq_gpio = platform_data->irq_gpio;
		bcm2079x_dev->en_gpio = platform_data->en_gpio;

		ret = gpio_request_one(bcm2079x_dev->en_gpio,
			GPIOF_OUT_INIT_LOW, "xxc_en");
		if (ret) {
			dev_err(&client->dev, "request EN GPIO failed\n");
			goto err_en;
		}
		ret = gpio_request_one(bcm2079x_dev->wake_gpio,
			GPIOF_OUT_INIT_LOW, "xxc_wake");
		if (ret) {
			dev_err(&client->dev, "request WAKE GPIO failed\n");
			goto err_wake;
		}
		ret = gpio_request_one(bcm2079x_dev->irq_gpio, GPIOF_IN, "xxc_irq");
		if (ret) {
			dev_err(&client->dev, "request irq GPIO failed\n");
			return -ENODEV;
		}
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "need I2C_FUNC_I2C\n");
		return -ENODEV;
	}

	ret = gpio_request_one(bcm2079x_dev->en_gpio,
		GPIOF_OUT_INIT_LOW, "xxc_en");
	if (ret) {
		dev_err(&client->dev, "request EN GPIO failed\n");
		goto err_en;
	}
	gpio_set_value(bcm2079x_dev->en_gpio, 0);

	if (bcm2079x_dev->wake_gpio != 0) {
		ret = gpio_request_one(bcm2079x_dev->wake_gpio,
			GPIOF_OUT_INIT_LOW, "xxc_wake");
		if (ret) {
			dev_err(&client->dev, "request WAKE GPIO failed\n");
			goto err_wake;
		}
		gpio_set_value(bcm2079x_dev->wake_gpio, 0);
	}

	/* init mutex and queues */
	init_waitqueue_head(&bcm2079x_dev->read_wq);
	mutex_init(&bcm2079x_dev->read_mutex);
	spin_lock_init(&bcm2079x_dev->irq_enabled_lock);

	bcm2079x_dev->bcm2079x_device.minor = MISC_DYNAMIC_MINOR;
	bcm2079x_dev->bcm2079x_device.name = "bcm2079x-i2c";
	bcm2079x_dev->bcm2079x_device.fops = &bcm2079x_dev_fops;

	ret = misc_register(&bcm2079x_dev->bcm2079x_device);
	if (ret) {
		dev_err(&client->dev, "misc_register failed\n");
		goto err_misc_register;
	}

	/* request irq.  the irq is set whenever the chip has data available
	 * for reading.  it is cleared when all data has been read.
	 */
	dev_info(&client->dev, "requesting IRQ %d\n", client->irq);
	bcm2079x_dev->irq_enabled = true;
	ret = request_irq(client->irq, bcm2079x_dev_irq_handler,
			IRQF_TRIGGER_RISING, client->name, bcm2079x_dev);
	if (ret) {
		dev_err(&client->dev, "request_irq failed\n");
		goto err_request_irq_failed;
	}
	bcm2079x_disable_irq(bcm2079x_dev);

	i2c_set_clientdata(client, bcm2079x_dev);
	dev_info(&client->dev, "bcm2079x on I2C %d @ 0x%02x\n", 0, client->addr);
	dev_info(&client->dev,
		 "%s, probing bcm2079x driver exited successfully\n",
		 __func__);

	// create RFKILL device
	bcm2079x_dev->rfkill_dev = rfkill_alloc("BCM2079x", &client->dev, RFKILL_TYPE_NFC, &rfkill_bcm2079x_ops, bcm2079x_dev);
	if (!bcm2079x_dev->rfkill_dev)
		dev_err(&client->dev, "RFKILL dev alloc failed\n");
	else {
		ret = rfkill_register(bcm2079x_dev->rfkill_dev);
		if (ret < 0) {
			dev_err(&client->dev, "RFKILL dev register failed\n");
			rfkill_destroy(bcm2079x_dev->rfkill_dev);
			bcm2079x_dev->rfkill_dev = NULL;
		}
	}

	return 0;

err_request_irq_failed:
	misc_deregister(&bcm2079x_dev->bcm2079x_device);
err_misc_register:
	mutex_destroy(&bcm2079x_dev->read_mutex);
	kfree(bcm2079x_dev);
err_exit:
	if (bcm2079x_dev->wake_gpio)
		gpio_free(bcm2079x_dev->wake_gpio);
err_wake:
	if (bcm2079x_dev->wake_gpio)
		gpio_free(bcm2079x_dev->wake_gpio);
err_en:
	gpio_free(bcm2079x_dev->irq_gpio);
	return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
static int bcm2079x_remove(struct i2c_client *client)
#else
static void bcm2079x_remove(struct i2c_client *client)
#endif
{
	struct bcm2079x_dev *bcm2079x_dev;

	bcm2079x_dev = i2c_get_clientdata(client);
	free_irq(client->irq, bcm2079x_dev);
	misc_deregister(&bcm2079x_dev->bcm2079x_device);
	mutex_destroy(&bcm2079x_dev->read_mutex);
	gpio_free(bcm2079x_dev->irq_gpio);
	gpio_free(bcm2079x_dev->en_gpio);
	if (bcm2079x_dev->wake_gpio)
		gpio_free(bcm2079x_dev->wake_gpio);
	if (bcm2079x_dev->rfkill_dev) {
		rfkill_unregister(bcm2079x_dev->rfkill_dev);
		rfkill_destroy(bcm2079x_dev->rfkill_dev);
	}		
	kfree(bcm2079x_dev);

	dev_info(&client->dev, "removed\n");

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	return 0;
#else
	return;
#endif
}

static const struct of_device_id of_bcm2079x_i2c_match[] = {
	{ .compatible = "bcm,bcm2079x-i2c", },
	{},
};
MODULE_DEVICE_TABLE(of, of_bcm2079x_i2c_match);


#ifdef CONFIG_ACPI
static struct acpi_device_id acpi_id[] = {
	{ "BCM2F1F" },
	{ },
};
MODULE_DEVICE_TABLE(acpi, acpi_id);
#endif


static const struct i2c_device_id bcm2079x_id[] = {
	{"bcm2079x-i2c", 0},
	{}
};

static struct i2c_driver bcm2079x_driver = {
	.id_table = bcm2079x_id,
	.probe = bcm2079x_probe,
	.remove = bcm2079x_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "bcm2079x-i2c",
#ifdef CONFIG_ACPI
		.acpi_match_table = ACPI_PTR(acpi_id),
#endif
		.of_match_table = of_match_ptr(of_bcm2079x_i2c_match),
	},
};

/*
 * module load/unload record keeping
 */

static int __init bcm2079x_dev_init(void)
{
	return i2c_add_driver(&bcm2079x_driver);
}
module_init(bcm2079x_dev_init);

static void __exit bcm2079x_dev_exit(void)
{
	i2c_del_driver(&bcm2079x_driver);
}
module_exit(bcm2079x_dev_exit);

MODULE_AUTHOR("Broadcom");
MODULE_AUTHOR("nicole.faerber@id3p.com");
MODULE_DESCRIPTION("bcm2079x contrller driver");
MODULE_LICENSE("GPL");
