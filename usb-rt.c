// SPDX-License-Identifier: GPL-2.0
/*
 * USB RT driver based on USB Skeleton driver - 2.2
 *
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 *
 * This driver is based on the 2.6.3 version of drivers/usb/usb-skeleton.c
 * but has been rewritten to be easier to read and use.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include "usb_rt_version.h"

MODULE_VERSION(USB_RT_VERSION_STRING);
/* Define these values to match your devices */
#define UNHUMAN_VENDOR_ID	0x3293
#define UNHUMAN_MTR_PRODUCT_ID	0x0100

/* table of devices that work with this driver */
static const struct usb_device_id usb_rt_table[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(UNHUMAN_VENDOR_ID, UNHUMAN_MTR_PRODUCT_ID, 0)},
	{} 					/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, usb_rt_table);


/* Get a minor range for your devices from the usb maintainer */
#define USB_RT_MINOR_BASE	192

/* our private defines. if this grows any larger, use your own .h file */
#define MAX_TRANSFER		(PAGE_SIZE - 512)
/*
 * MAX_TRANSFER is chosen so that the VM is not stressed by
 * allocations > PAGE_SIZE and the number of packets in a page
 * is an integer 512 is the largest possible packet on EHCI
 */
#define WRITES_IN_FLIGHT	1
/* arbitrarily chosen */

/* Structure to hold all of our device specific stuff */
struct usb_rt {
	struct usb_device	*udev;			/* the usb device for this device */
	struct usb_interface	*interface;		/* the interface for this device */
	struct semaphore	limit_sem;		/* limiting the number of writes in progress */
	struct usb_anchor	submitted;		/* in case we need to retract our submissions */
	struct urb		*bulk_in_urb;		/* the urb to read data with */
	unsigned char           *bulk_in_buffer;	/* the buffer to receive data */
	struct urb		*bulk_out_urb;		/* the urb to write data with */
	unsigned char           *bulk_out_buffer;	/* the buffer to write data */
	size_t			bulk_in_size;		/* the size of the receive buffer */
	size_t			bulk_out_size;		/* the size of the receive buffer */
	size_t			bulk_in_filled;		/* number of bytes in the buffer */
	size_t			bulk_in_copied;		/* already copied to user space */
	unsigned char	*text_api_buffer;
	__u8			bulk_in_endpointAddr;	/* the address of the bulk in endpoint */
	__u8			bulk_out_endpointAddr;	/* the address of the bulk out endpoint */
	int			errors;			/* the last request tanked */
	bool			ongoing_read;		/* a read is going on */
	spinlock_t		err_lock;		/* lock for errors */
	struct kref		kref;
	struct mutex		io_mutex;		/* synchronize I/O with disconnect */
	unsigned long		disconnected:1;
	wait_queue_head_t	bulk_in_wait;		/* to wait for an ongoing read */
	bool 			has_text_api;
};
#define to_usb_rt_dev(d) container_of(d, struct usb_rt, kref)

static struct usb_driver usb_rt_driver;
static void usb_rt_draw_down(struct usb_rt *dev);

static void usb_rt_delete(struct kref *kref)
{
	struct usb_rt *dev = to_usb_rt_dev(kref);

	kfree(dev->text_api_buffer);
	usb_free_coherent(dev->bulk_in_urb->dev, dev->bulk_in_size,
			  dev->bulk_in_buffer, dev->bulk_in_urb->transfer_dma);
	usb_free_urb(dev->bulk_in_urb);
	usb_free_coherent(dev->bulk_out_urb->dev, dev->bulk_out_size,
			  dev->bulk_out_buffer, dev->bulk_out_urb->transfer_dma);
	usb_free_urb(dev->bulk_out_urb);
	usb_put_intf(dev->interface);
	usb_put_dev(dev->udev);
	kfree(dev);
}

static int usb_rt_open(struct inode *inode, struct file *file)
{
	struct usb_rt *dev;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	subminor = iminor(inode);

	interface = usb_find_interface(&usb_rt_driver, subminor);
	if (!interface) {
		pr_err("%s - error, can't find device for minor %d\n",
			__func__, subminor);
		retval = -ENODEV;
		goto exit;
	}

	dev = usb_get_intfdata(interface);
	if (!dev) {
		retval = -ENODEV;
		goto exit;
	}

	retval = usb_autopm_get_interface(interface);
	if (retval)
		goto exit;

	/* increment our usage count for the device */
	kref_get(&dev->kref);

	/* save our object in the file's private structure */
	file->private_data = dev;

exit:
	return retval;
}

static int usb_rt_release(struct inode *inode, struct file *file)
{
	struct usb_rt *dev;

	dev = file->private_data;
	if (dev == NULL)
		return -ENODEV;

	/* allow the device to be autosuspended */
	usb_autopm_put_interface(dev->interface);

	/* decrement the count on our device */
	kref_put(&dev->kref, usb_rt_delete);
	return 0;
}

static int usb_rt_flush(struct file *file, fl_owner_t id)
{
	struct usb_rt *dev;
	unsigned long flags;
	int res;

	dev = file->private_data;
	if (dev == NULL)
		return -ENODEV;

	/* wait for io to stop */
	mutex_lock(&dev->io_mutex);
	usb_rt_draw_down(dev);

	/* read out errors, leave subsequent opens a clean slate */
	spin_lock_irqsave(&dev->err_lock, flags);
	res = dev->errors ? (dev->errors == -EPIPE ? -EPIPE : -EIO) : 0;
	dev->errors = 0;
	spin_unlock_irqrestore(&dev->err_lock, flags);

	mutex_unlock(&dev->io_mutex);

	return res;
}

static void usb_rt_read_bulk_callback(struct urb *urb)
{
	struct usb_rt *dev;
	unsigned long flags;

	dev = urb->context;

	spin_lock_irqsave(&dev->err_lock, flags);
	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
		    urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN))
			dev_err(&dev->interface->dev,
				"%s - nonzero read bulk status received: %d\n",
				__func__, urb->status);

		dev->errors = urb->status;
	} else {
		dev->bulk_in_filled = urb->actual_length;
	}
	dev->ongoing_read = 0;
	spin_unlock_irqrestore(&dev->err_lock, flags);

	wake_up_interruptible(&dev->bulk_in_wait);
}

static int usb_rt_do_read_io(struct usb_rt *dev, size_t count)
{
	int rv;
	unsigned long flags;

	/* prepare a read */
	usb_fill_bulk_urb(dev->bulk_in_urb,
			dev->udev,
			usb_rcvbulkpipe(dev->udev,
				dev->bulk_in_endpointAddr),
			dev->bulk_in_buffer,
			min(dev->bulk_in_size, count),
			usb_rt_read_bulk_callback,
			dev);
	/* tell everybody to leave the URB alone */
	spin_lock_irqsave(&dev->err_lock, flags);
	dev->ongoing_read = 1;
	spin_unlock_irqrestore(&dev->err_lock, flags);

	/* submit bulk in urb, which means no data to deliver */
	dev->bulk_in_filled = 0;
	dev->bulk_in_copied = 0;

	/* do it */
	rv = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
	if (rv < 0) {
		dev_err(&dev->interface->dev,
			"%s - failed submitting read urb, error %d\n",
			__func__, rv);
		rv = (rv == -ENOMEM) ? rv : -EIO;
		spin_lock_irqsave(&dev->err_lock, flags);
		dev->ongoing_read = 0;
		spin_unlock_irqrestore(&dev->err_lock, flags);
	}

	return rv;
}

unsigned int usb_rt_poll(struct file *file, struct poll_table_struct *wait) {
	struct usb_rt *dev;
	bool ongoing_io;
	unsigned long flags;
	unsigned int retval =  POLLWRNORM | POLLPRI | POLLOUT;	// can always write
	int rv;

	dev = file->private_data;
	
	rv = mutex_lock_interruptible(&dev->io_mutex);
	if (rv < 0) {
		return rv;
	}

	if (dev->disconnected) {		/* disconnect() was called */
		retval = -ENODEV;
		goto exit;
	}

	poll_wait(file, &dev->bulk_in_wait, wait);

	spin_lock_irqsave(&dev->err_lock, flags);
	ongoing_io = dev->ongoing_read;
	spin_unlock_irqrestore(&dev->err_lock, flags);
	if(ongoing_io) {
		// only return default retval
	} else if (dev->errors) {
		dev_info(&dev->interface->dev, "poll error: %d", dev->errors);
		retval = POLLERR;
		goto exit;
	} else {
		if (dev->bulk_in_filled - dev->bulk_in_copied) {
			// data is available
			retval |= POLLRDNORM | POLLIN;
		} else {
			// todo else poll maybe triggers a new read
			rv = usb_rt_do_read_io(dev, dev->bulk_in_size);
			if (rv) {
				retval = POLLERR;
				goto exit;
			}
		}
	}

exit:
	mutex_unlock(&dev->io_mutex);
	return retval;
}

static ssize_t usb_rt_read(struct file *file, char *buffer, size_t count,
			 loff_t *ppos)
{
	struct usb_rt *dev;
	int rv;
	bool ongoing_io;
	unsigned long flags;

	dev = file->private_data;

	/* if we cannot read at all, return EOF */
	if (!dev->bulk_in_urb || !count)
		return 0;

	/* no concurrent readers */
	rv = mutex_lock_interruptible(&dev->io_mutex);
	if (rv < 0)
		return rv;

	if (dev->disconnected) {		/* disconnect() was called */
		rv = -ENODEV;
		goto exit;
	}

	/* if IO is under way, we must not touch things */
retry:
	spin_lock_irqsave(&dev->err_lock, flags);
	ongoing_io = dev->ongoing_read;
	spin_unlock_irqrestore(&dev->err_lock, flags);

	if (ongoing_io) {
		/* nonblocking IO shall not wait */
		if (file->f_flags & O_NONBLOCK) {
			rv = -EAGAIN;
			goto exit;
		}
		/*
		 * IO may take forever
		 * hence wait in an interruptible state
		 */
		rv = wait_event_interruptible_timeout(dev->bulk_in_wait, (!dev->ongoing_read), msecs_to_jiffies(10));
		if (rv <= 0) {
			if (rv == 0) {
				rv = -ETIMEDOUT;
			}
			goto exit;
		}
	}

	/* errors must be reported */
	rv = dev->errors;
	if (rv < 0) {
		/* any error is reported once */
		dev->errors = 0;
		/* to preserve notifications about reset */
		rv = (rv == -EPIPE) ? rv : -EIO;
		/* report it */
		goto exit;
	}

	/*
	 * if the buffer is filled we may satisfy the read
	 * else we need to start IO
	 */

	if (dev->bulk_in_filled) {
		/* we had read data */
		size_t available = dev->bulk_in_filled - dev->bulk_in_copied;
		size_t chunk = min(available, count);

		if (!available) {
			/*
			 * all data has been used
			 * actual IO needs to be done
			 */
			rv = usb_rt_do_read_io(dev, count);
			if (rv < 0)
				goto exit;
			else
				goto retry;
		}
		/*
		 * data is available
		 * chunk tells us how much shall be copied
		 */

		if (copy_to_user(buffer,
				 dev->bulk_in_buffer + dev->bulk_in_copied,
				 chunk))
			rv = -EFAULT;
		else
			rv = chunk;

		dev->bulk_in_copied += chunk;

		/*
		 * if we are asked for more than we have,
		 * we start IO but don't wait
		 */
		// usb_rt only returns what it gets in one packet
		// if (available < count)
		// 	usb_rt_do_read_io(dev, count - chunk);
	} else {
		/* no data in the buffer */
		rv = usb_rt_do_read_io(dev, count);
		if (rv < 0)
			goto exit;
		else
			goto retry;
	}
exit:
	mutex_unlock(&dev->io_mutex);
	return rv;
}

static void usb_rt_write_bulk_callback(struct urb *urb)
{
	struct usb_rt *dev;
	unsigned long flags;

	dev = urb->context;

	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
		    urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN))
			dev_err(&dev->interface->dev,
				"%s - nonzero write bulk status received: %d\n",
				__func__, urb->status);

		spin_lock_irqsave(&dev->err_lock, flags);
		dev->errors = urb->status;
		spin_unlock_irqrestore(&dev->err_lock, flags);
	}

	up(&dev->limit_sem);
}

static ssize_t usb_rt_write(struct file *file, const char *user_buffer,
			  size_t count, loff_t *ppos)
{
	struct usb_rt *dev = file->private_data;
	int retval = 0;
	struct urb *urb = NULL;
	char *buf = NULL;
	unsigned long flags;
	size_t writesize = min(count, (size_t)dev->bulk_out_size);

	//dev_info(&dev->interface->dev, "count write: %ld", count);

	/* verify that we actually have some data to write */
	if (count == 0)
		goto exit;

	/*
	 * limit the number of URBs in flight to stop a user from using up all
	 * RAM
	 */
	if (!(file->f_flags & O_NONBLOCK)) {
		if (down_interruptible(&dev->limit_sem)) {
			retval = -ERESTARTSYS;
			goto exit;
		}
	} else {
		if (down_trylock(&dev->limit_sem)) {
			retval = -EAGAIN;
			goto exit;
		}
	}

	spin_lock_irqsave(&dev->err_lock, flags);
	retval = dev->errors;
	if (retval < 0) {
		/* any error is reported once */
		dev->errors = 0;
		/* to preserve notifications about reset */
		retval = (retval == -EPIPE) ? retval : -EIO;
	}
	spin_unlock_irqrestore(&dev->err_lock, flags);
	if (retval < 0)
		goto error;

	if (copy_from_user(dev->bulk_out_buffer, user_buffer, writesize)) {
		retval = -EFAULT;
		goto error;
	}

	/* this lock makes sure we don't submit URBs to gone devices */
	mutex_lock(&dev->io_mutex);
	if (dev->disconnected) {		/* disconnect() was called */
		mutex_unlock(&dev->io_mutex);
		retval = -ENODEV;
		goto error;
	}

	/* initialize the urb properly */
	usb_fill_bulk_urb(dev->bulk_out_urb, dev->udev,
			  usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
			  dev->bulk_out_buffer, writesize, usb_rt_write_bulk_callback, dev);
	dev->bulk_out_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	usb_anchor_urb(dev->bulk_out_urb, &dev->submitted);

	/* send the data out the bulk port */
	retval = usb_submit_urb(dev->bulk_out_urb, GFP_KERNEL);
	mutex_unlock(&dev->io_mutex);
	if (retval) {
		dev_err(&dev->interface->dev,
			"%s - failed submitting write urb, error %d\n",
			__func__, retval);
		goto error_unanchor;
	}

	return writesize;

error_unanchor:
	usb_unanchor_urb(urb);
error:
	if (urb) {
		usb_free_coherent(dev->udev, writesize, buf, urb->transfer_dma);
		usb_free_urb(urb);
	}
	up(&dev->limit_sem);

exit:
	return retval;
}

static const struct file_operations usb_rt_fops = {
	.owner =	THIS_MODULE,
	.read =		usb_rt_read,
	.write =	usb_rt_write,
	.open =		usb_rt_open,
	.release =	usb_rt_release,
	.flush =	usb_rt_flush,
	.llseek =	noop_llseek,
	.poll = 	usb_rt_poll,
};

static ssize_t text_api_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)		
{
	struct usb_interface *intf = to_usb_interface(dev);		
	struct usb_rt *usb_rt = usb_get_intfdata(intf);	
	int transfer_count = min(count, MAX_TRANSFER);
	int count_sent = 0;
	int retval;

	memcpy(usb_rt->text_api_buffer, buf, transfer_count);	 // usb_bulk_msg doesn't want a pointer to const

	/* do an immediate bulk write to the device */
	retval = usb_bulk_msg (usb_rt->udev,
						usb_sndbulkpipe (usb_rt->udev,
						0x01),
						usb_rt->text_api_buffer,
						transfer_count,
						&count_sent, HZ*10);
	if (retval)
		return retval;
	else
		return count_sent;		
}

static ssize_t text_api_show(struct device *dev, struct device_attribute *attr, char *buf)		
{
	struct usb_interface *intf = to_usb_interface(dev);		
	struct usb_rt *usb_rt = usb_get_intfdata(intf);	
	int count_received = 0;
	/* do an immediate bulk read to get data from the device */
	int retval = usb_bulk_msg (usb_rt->udev,
						usb_rcvbulkpipe (usb_rt->udev,
						0x81),
						buf,
						MAX_TRANSFER,
						&count_received, 100);
	if (retval)
		return retval;
	else
		return count_received;		
}
struct device_attribute dev_attr_text_api = __ATTR_RW(text_api);

/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with the driver core
 */
// The generic class
static struct usb_class_driver usb_rt_class = {
	.name =		"usbrt%d",
	.fops =		&usb_rt_fops,
	.minor_base =	USB_RT_MINOR_BASE,
};

// A specialized name for motors
static struct usb_class_driver usb_mtr_class = {
	.name =		"mtr%d",
	.fops =		&usb_rt_fops,
	.minor_base =	USB_RT_MINOR_BASE,
};

static int usb_rt_probe(struct usb_interface *interface,
		      const struct usb_device_id *id)
{
	struct usb_rt *dev;
	struct usb_endpoint_descriptor *bulk_in=NULL, *bulk_out=NULL;
	int retval;

	/* allocate memory for our device state and initialize it */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	kref_init(&dev->kref);
	sema_init(&dev->limit_sem, WRITES_IN_FLIGHT);
	mutex_init(&dev->io_mutex);
	spin_lock_init(&dev->err_lock);
	init_usb_anchor(&dev->submitted);
	init_waitqueue_head(&dev->bulk_in_wait);

	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = usb_get_intf(interface);

	/* set up the endpoint information */
	/* use only the first bulk-in and bulk-out endpoints on interface number 0
	   is the current convention */

	retval = 0;
	if (interface->cur_altsetting->desc.bInterfaceNumber == 0 &&
		usb_endpoint_is_bulk_in(&interface->cur_altsetting->endpoint[0].desc) &&
		usb_endpoint_is_bulk_out(&interface->cur_altsetting->endpoint[1].desc)) {
		// realtime interface
		bulk_in = &interface->cur_altsetting->endpoint[0].desc;
		bulk_out = &interface->cur_altsetting->endpoint[1].desc;
		if (interface->cur_altsetting->desc.bNumEndpoints == 4 &&
			usb_endpoint_is_bulk_in(&interface->cur_altsetting->endpoint[2].desc) &&
			usb_endpoint_is_bulk_out(&interface->cur_altsetting->endpoint[3].desc)) {
			// text api interface
			retval = device_create_file(&interface->dev, &dev_attr_text_api);
			dev->has_text_api = true;
			if (retval)
				goto error;
		}
	} else {
		retval = 1;
	}
	if (retval) {
		dev_err(&interface->dev,
			"Could not find both bulk-in and bulk-out endpoints\n");
		goto error;
	}

	dev->bulk_in_size = usb_endpoint_maxp(bulk_in);
	dev->bulk_in_endpointAddr = bulk_in->bEndpointAddress;
	dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->bulk_in_urb) {
		retval = -ENOMEM;
		goto error;
	}
	dev->bulk_in_buffer = usb_alloc_coherent(dev->udev, dev->bulk_in_size, GFP_KERNEL,
				 &dev->bulk_in_urb->transfer_dma);
	if (!dev->bulk_in_buffer) {
		retval = -ENOMEM;
		goto error;
	}
	dev->bulk_in_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	dev->bulk_out_endpointAddr = bulk_out->bEndpointAddress;
	
	dev->bulk_in_size = usb_endpoint_maxp(bulk_out);
	dev->bulk_out_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->bulk_out_urb) {
		retval = -ENOMEM;
		goto error;
	}
	dev->bulk_out_buffer = usb_alloc_coherent(dev->udev, dev->bulk_out_size, GFP_KERNEL,
				 &dev->bulk_out_urb->transfer_dma);
	if (!dev->bulk_out_buffer) {
		retval = -ENOMEM;
		goto error;
	}


	dev->text_api_buffer = kmalloc(MAX_TRANSFER, GFP_KERNEL);
	if (!dev->text_api_buffer) {
		retval = -ENOMEM;
		goto error;
	}

	/* save our data pointer in this interface device */
	usb_set_intfdata(interface, dev);

	/* we can register the device now, as it is ready */
	switch (id->idProduct) {
		case UNHUMAN_MTR_PRODUCT_ID:
			retval = usb_register_dev(interface, &usb_mtr_class);
			break;
		default:
			retval = usb_register_dev(interface, &usb_rt_class);
			break;
	}
	if (retval) {
		/* something prevented us from registering this driver */
		dev_err(&interface->dev,
			"Not able to get a minor for this device.\n");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	/* let the user know what node this device is now attached to */
	dev_info(&interface->dev,
		 "USB RT device now attached to USBRT-%d",
		 interface->minor);
	return 0;

error:
	/* this frees allocated memory */
	kref_put(&dev->kref, usb_rt_delete);

	return retval;
}

static void usb_rt_disconnect(struct usb_interface *interface)
{
	struct usb_rt *dev;
	int minor = interface->minor;

	dev = usb_get_intfdata(interface);
	if (dev->has_text_api == true)
		device_remove_file(&interface->dev, &dev_attr_text_api);
	usb_set_intfdata(interface, NULL);

	/* give back our minor */
	usb_deregister_dev(interface, &usb_rt_class);

	/* prevent more I/O from starting */
	mutex_lock(&dev->io_mutex);
	dev->disconnected = 1;
	mutex_unlock(&dev->io_mutex);

	usb_kill_urb(dev->bulk_in_urb);
	usb_kill_anchored_urbs(&dev->submitted);

	dev_info(&interface->dev, "USB RT #%d disconnected", minor);

	/* decrement our usage count */
	kref_put(&dev->kref, usb_rt_delete);
}

static void usb_rt_draw_down(struct usb_rt *dev)
{
	int time;

	time = usb_wait_anchor_empty_timeout(&dev->submitted, 1000);
	if (!time)
		usb_kill_anchored_urbs(&dev->submitted);
	usb_kill_urb(dev->bulk_in_urb);
}

static int usb_rt_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct usb_rt *dev = usb_get_intfdata(intf);

	if (!dev)
		return 0;
	usb_rt_draw_down(dev);
	return 0;
}

static int usb_rt_resume(struct usb_interface *intf)
{
	return 0;
}

static int usb_rt_pre_reset(struct usb_interface *intf)
{
	struct usb_rt *dev = usb_get_intfdata(intf);

	mutex_lock(&dev->io_mutex);
	usb_rt_draw_down(dev);

	return 0;
}

static int usb_rt_post_reset(struct usb_interface *intf)
{
	struct usb_rt *dev = usb_get_intfdata(intf);

	/* we are sure no URBs are active - no locking needed */
	dev->errors = -EPIPE;
	mutex_unlock(&dev->io_mutex);

	return 0;
}

static struct usb_driver usb_rt_driver = {
	.name =		"usb_rt",
	.probe =	usb_rt_probe,
	.disconnect =	usb_rt_disconnect,
	.suspend =	usb_rt_suspend,
	.resume =	usb_rt_resume,
	.pre_reset =	usb_rt_pre_reset,
	.post_reset =	usb_rt_post_reset,
	.id_table =	usb_rt_table,
	.supports_autosuspend = 1,
};

module_usb_driver(usb_rt_driver);

MODULE_LICENSE("GPL v2");
