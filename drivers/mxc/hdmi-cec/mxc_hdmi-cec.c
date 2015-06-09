/*
 * Copyright (C) 2012-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 */

/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

/*!
 * @file mxc_hdmi-cec.c
 *
 * @brief HDMI CEC system initialization and file operation implementation
 *
 * @ingroup HDMI
 */

//#define DEBUG

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/fsl_devices.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/sizes.h>

#include <linux/console.h>
#include <linux/types.h>
#include <linux/mfd/mxc-hdmi-core.h>
#include <linux/pinctrl/consumer.h>

#include <video/mxc_hdmi.h>

#include "mxc_hdmi-cec.h"

#define MAXCLIENTS 15

struct hdmi_cec_priv {
	u8 la;
	u8 libcec_la;
	struct list_head msg_head;
	struct list_head client_node;
	struct list_head wakeup_node;
	wait_queue_head_t hdmi_cec_qm;
	spinlock_t i_lock_cl;
};

struct hdmi_cec_event {
	u8 event_type;
	u8 msg_len;
	u8 msg[MAX_MESSAGE_LEN];
};

struct hdmi_cec_event_list {
	struct hdmi_cec_event data;
	struct list_head msg_node;
};

struct hdmi_cec_shared {
	bool write_busy;
	int  receive_error;
	int  send_error;
	struct delayed_work hdmi_cec_work, hdmi_msg_trigger;
	spinlock_t i_lock;
	struct mutex m_lock;
	struct list_head client_head;
	int nr_ff;
	u16 addresses;
	u8 latest_cec_stat;
	u32 physical_address;
};

static struct hdmi_cec_shared hdmi_cec_root;

static bool hdmi_cec_state;
static int hdmi_cec_ready = 0;
static int hdmi_cec_major;
static struct class *hdmi_cec_class;
static u8 open_count = 0;

static wait_queue_head_t hdmi_cec_qs, hdmi_cec_qw;

static inline bool la_is_local(u8 la)
{
	return (la == 0xf) ? false : hdmi_cec_root.addresses & BIT(la);
}

static irqreturn_t mxc_hdmi_cec_isr(int irq, void *data)
{
	u8 cec_stat = 0;
	unsigned long flags;
	irqreturn_t ret = IRQ_HANDLED;

	cec_stat = hdmi_readb(HDMI_IH_CEC_STAT0);
	if (!cec_stat) {
		ret = IRQ_NONE;
		goto irqnone;
	}

	spin_lock_irqsave(&hdmi_cec_root.i_lock, flags);

	hdmi_writeb(0x7f, HDMI_IH_MUTE_CEC_STAT0);
	hdmi_writeb(cec_stat, HDMI_IH_CEC_STAT0);

	if (cec_stat & HDMI_IH_CEC_STAT0_ERROR_INIT) {
		hdmi_cec_root.send_error++;
		pr_debug("%s:  error %d\n", __func__, hdmi_cec_root.send_error);
		wake_up(&hdmi_cec_qs);
	}
	if (cec_stat & (HDMI_IH_CEC_STAT0_NACK | HDMI_IH_CEC_STAT0_DONE)) {
		hdmi_cec_root.send_error = 0;
		wake_up(&hdmi_cec_qs);
	}

	hdmi_cec_root.latest_cec_stat = cec_stat;
	pr_debug("%s:  HDMI CEC interrupt received\n", __func__);
	schedule_delayed_work(&(hdmi_cec_root.hdmi_cec_work), msecs_to_jiffies(5));

	spin_unlock_irqrestore(&hdmi_cec_root.i_lock, flags);
irqnone:
	return ret;
}

static void __mxc_hdmi_cec_msg(struct hdmi_cec_event_list *event, struct hdmi_cec_priv *client, struct list_head *wakeup, u8 event_type)
{
	struct hdmi_cec_event_list *tevent = kzalloc(sizeof(struct hdmi_cec_event_list), GFP_KERNEL);
	unsigned long flags;

	if (!tevent) {
		pr_err("%s: Not enough memory!\n", __func__);
		return;
	}
	memcpy(&tevent->data, &event->data, min(sizeof(struct hdmi_cec_event), (size_t)(event->data.msg_len + 2)));
	if (event_type)
		tevent->data.event_type = event_type;

	spin_lock_irqsave(&client->i_lock_cl, flags);
	list_add_tail(&tevent->msg_node, &client->msg_head);
	spin_unlock_irqrestore(&client->i_lock_cl, flags);

	if (wakeup)
		list_add_tail(&client->wakeup_node, wakeup);
}

void mxc_hdmi_cec_handle(u32 cec_stat)
{
	struct hdmi_cec_event_list *event = NULL;
	struct hdmi_cec_priv *client = NULL;

	if (cec_stat)
		hdmi_cec_root.physical_address = cec_stat;

	/* HDMI cable connected / HDMI cable disconnected */
	if (!hdmi_cec_ready)
		return;
	pr_debug("%s: enter\n", __func__);

	event = kzalloc(sizeof(struct hdmi_cec_event_list), GFP_KERNEL);
	if (!event) {
		pr_err("%s: Not enough memory!\n", __func__);
		return;
	}
	event->data.event_type = cec_stat ?
		MESSAGE_TYPE_CONNECTED : MESSAGE_TYPE_DISCONNECTED;

	list_for_each_entry(client, &hdmi_cec_root.client_head, client_node) {
		mutex_lock(&hdmi_cec_root.m_lock);
		__mxc_hdmi_cec_msg(event, client, NULL, 0);
		mutex_unlock(&hdmi_cec_root.m_lock);
		wake_up(&client->hdmi_cec_qm);
	}
	pr_debug("%s: exit\n", __func__);
}
EXPORT_SYMBOL(mxc_hdmi_cec_handle);

void mxc_hdmi_cec_msg(u8 event_type)
{
	struct hdmi_cec_event_list *event = NULL;
	struct hdmi_cec_priv *client = NULL;
	u8 i;
	LIST_HEAD(wakeup);

	event = kzalloc(sizeof(struct hdmi_cec_event_list), GFP_KERNEL);
	if (NULL == event) {
		pr_err("%s: Not enough memory!\n", __func__);
		goto error2;
	}
	event->data.msg_len = (event_type == MESSAGE_TYPE_RECEIVE_SUCCESS) ?
		hdmi_readb(HDMI_CEC_RX_CNT) : hdmi_readb(HDMI_CEC_TX_CNT);

	if (!event->data.msg_len || event->data.msg_len > MAX_MESSAGE_LEN) {
		pr_err("%s: Bad message size %d!\n", __func__, event->data.msg_len);
		goto error1;
	}

	for (i = 0; i < event->data.msg_len; i++)
		event->data.msg[i] = (event_type == MESSAGE_TYPE_RECEIVE_SUCCESS) ?
				hdmi_readb(HDMI_CEC_RX_DATA0+i) : hdmi_readb(HDMI_CEC_TX_DATA0+i);

	event->data.event_type = (event_type == MESSAGE_TYPE_NOACK && la_is_local(event->data.msg[0] & 0x0f)) ?
								MESSAGE_TYPE_SEND_SUCCESS : event_type;

	list_for_each_entry(client, &hdmi_cec_root.client_head, client_node) {
		if (client->la == 0xff) continue;

		if ((event_type == MESSAGE_TYPE_RECEIVE_SUCCESS && 
					(((event->data.msg[0] & 0x0f) == 15 && client->libcec_la != client->la) || (event->data.msg[0] & 0x0f) == client->la))
		    ||
		    (event_type != MESSAGE_TYPE_RECEIVE_SUCCESS &&
					(client->libcec_la == (int)client->la)) )
		{
			__mxc_hdmi_cec_msg(event, client, &wakeup, 0);
		}

		if ((event->data.event_type == MESSAGE_TYPE_SEND_SUCCESS) &&
					((la_is_local(event->data.msg[0] & 0x0f) && (event->data.msg[0] & 0x0f) == client->la)
					||
					((event->data.msg[0] & 0x0f) == 15 && !client->libcec_la)) )
		{
			__mxc_hdmi_cec_msg(event, client, &wakeup, MESSAGE_TYPE_RECEIVE_SUCCESS);
		}
		client->libcec_la = 0;
	}
	list_for_each_entry(client, &wakeup, wakeup_node)
		wake_up(&(client->hdmi_cec_qm));

error1:
	kfree(event);
error2:
	if (event_type != MESSAGE_TYPE_RECEIVE_SUCCESS) {
		mutex_lock(&hdmi_cec_root.m_lock);
		hdmi_cec_root.write_busy = false;
		hdmi_writeb(0, HDMI_CEC_TX_CNT);
		mutex_unlock(&hdmi_cec_root.m_lock);
		wake_up(&hdmi_cec_qw);
	}
}

static void mxc_hdmi_cec_worker(struct work_struct *work)
{
	unsigned long flags;
	u8 val;

	pr_debug("%s: \n", __func__);
	if (hdmi_cec_root.latest_cec_stat && open_count) {
		/* The current transmission is successful (for initiator only).*/
		if (hdmi_cec_root.latest_cec_stat & HDMI_IH_CEC_STAT0_DONE) {
			mxc_hdmi_cec_msg(MESSAGE_TYPE_SEND_SUCCESS);
		}
		/*A frame is not acknowledged in a directly addressed message. Or a frame is negatively acknowledged in
		a broadcast message (for initiator only).*/
		if (hdmi_cec_root.latest_cec_stat & HDMI_IH_CEC_STAT0_NACK) {
			mxc_hdmi_cec_msg(MESSAGE_TYPE_NOACK);
		}
		/*EOM is detected so that the received data is ready in the receiver data buffer*/
		if (hdmi_cec_root.latest_cec_stat & HDMI_IH_CEC_STAT0_EOM) {
			mxc_hdmi_cec_msg(MESSAGE_TYPE_RECEIVE_SUCCESS);
			hdmi_writeb(0x0, HDMI_CEC_LOCK);
		}
		hdmi_cec_root.latest_cec_stat = 0;
	}

	val = HDMI_IH_CEC_STAT0_WAKEUP | HDMI_IH_CEC_STAT0_ERROR_FOLL | HDMI_IH_CEC_STAT0_ARB_LOST;
	spin_lock_irqsave(&hdmi_cec_root.i_lock, flags);
	hdmi_writeb(val, HDMI_IH_MUTE_CEC_STAT0);
	spin_unlock_irqrestore(&hdmi_cec_root.i_lock, flags);
	pr_debug("%s:  exit\n", __func__);
}

static long hdmi_cec_set_address(u8 arg, struct hdmi_cec_priv *hdmi_cec);
/*!
 * @brief open function for cec file operation
 *
 * @return  0 on success or negative error code on error
 */
static int hdmi_cec_open(struct inode *inode, struct file *filp)
{
	struct hdmi_cec_priv *hdmi_cec = NULL;

	mutex_lock(&hdmi_cec_root.m_lock);
	if (open_count == MAXCLIENTS || !hdmi_cec_ready) {
		mutex_unlock(&hdmi_cec_root.m_lock);
		return -EBUSY;
	}
	open_count++;

	hdmi_cec = kzalloc(sizeof(struct hdmi_cec_priv), GFP_KERNEL);
	hdmi_cec->la = -1;
	init_waitqueue_head(&(hdmi_cec->hdmi_cec_qm));
	spin_lock_init(&(hdmi_cec->i_lock_cl));
	INIT_LIST_HEAD(&hdmi_cec->msg_head);

	filp->private_data = (void *)(hdmi_cec);

	list_add_tail(&hdmi_cec->client_node, &hdmi_cec_root.client_head);
	hdmi_cec_set_address(15, hdmi_cec);
	mutex_unlock(&hdmi_cec_root.m_lock);

	return 0;
}

/*
 * run delayed mxc_hdmi_cec_msg() to deal with msgs to LAs registered on local CEC.
 * imx CEC is not transporting such messages through (local) interface
 * (messages with destination the same as any regstered LA)
 */
static void mxc_hdmi_cec_msg_trigger(struct work_struct *work)
{
	mxc_hdmi_cec_msg(MESSAGE_TYPE_SEND_SUCCESS);
}

static ssize_t hdmi_cec_read(struct file *file, char __user *buf, size_t count,
			    loff_t *ppos)
{
	struct hdmi_cec_priv *hdmi_cec = file->private_data;
	int ret = 0;

	if (!open_count || hdmi_cec->la == 0xff)
		return -ENODEV;

	pr_debug("%s:  client la %x, (addr %x)\n", __func__, hdmi_cec->la, (unsigned int)hdmi_cec);

	count = min(count, sizeof(struct hdmi_cec_event));
	do {
		unsigned long flags;
		struct hdmi_cec_event_list *event = NULL;

		spin_lock_irqsave(&(hdmi_cec->i_lock_cl), flags);
		if (!list_empty(&hdmi_cec->msg_head)) {
			event = list_first_entry_or_null(&hdmi_cec->msg_head, struct hdmi_cec_event_list, msg_node);
			list_del(&event->msg_node);
		}
		spin_unlock_irqrestore(&(hdmi_cec->i_lock_cl), flags);

		if (event) {
			ret = copy_to_user(buf, &event->data, count) ? -EFAULT : count;
			kfree(event);
		}
		else if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
		}
		else if (wait_event_interruptible(hdmi_cec->hdmi_cec_qm, (!list_empty(&hdmi_cec->msg_head)))) {
			ret = -ERESTARTSYS;
		}
	} while(!ret);

	pr_debug("%s:  exit %d\n", __func__, ret);
	return ret;
}

static ssize_t hdmi_cec_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct hdmi_cec_priv *hdmi_cec = file->private_data;
	int ret = 0 , i = 0;
	u8 msg[MAX_MESSAGE_LEN];
	u8 val = 0;
	int timeout = 1500;

	if (!open_count || hdmi_cec->la == 0xff)
		return -ENODEV;

	if (count > MAX_MESSAGE_LEN)
		return -E2BIG;

	memset(&msg, 0, MAX_MESSAGE_LEN);
	if (copy_from_user(&msg, buf, count))
		return -EFAULT;

	if (file->f_flags & O_NONBLOCK && hdmi_cec_root.write_busy)
		return -EAGAIN;
	else if (wait_event_interruptible(hdmi_cec_qw, (!hdmi_cec_root.write_busy)))
		return -ERESTARTSYS;

	mutex_lock(&hdmi_cec_root.m_lock);
	pr_debug("%s: \n", __func__);
	hdmi_cec_root.write_busy = true;

	hdmi_writeb(count, HDMI_CEC_TX_CNT);
	for (i = 0; i < count; i++)
		hdmi_writeb(msg[i], HDMI_CEC_TX_DATA0+i);

	hdmi_cec->libcec_la = hdmi_cec->la;
	if (la_is_local(msg[0] & 0x0f)) {
		ret = count;
		schedule_delayed_work(&(hdmi_cec_root.hdmi_msg_trigger), msecs_to_jiffies(5));
		pr_debug("%s:  wait_event la_is_local\n", __func__);
	} else

	do {
		val = hdmi_readb(HDMI_CEC_CTRL); val |= 0x01; val &= ~0x6;
		val |= hdmi_cec_root.send_error ? SIGNAL_FREE_TIME_RESEND : SIGNAL_FREE_TIME_NORMAL;
		hdmi_writeb(val, HDMI_CEC_CTRL);

		ret = wait_event_timeout(hdmi_cec_qs, !((val = hdmi_readb(HDMI_CEC_CTRL)) & 0x01), msecs_to_jiffies(timeout));
		pr_debug("%s:  wait_event ret %d\n", __func__, ret);
		if (hdmi_cec_root.send_error > 5 || ret < 2) {
			hdmi_writeb(0, HDMI_CEC_TX_CNT);
			hdmi_cec_root.write_busy = false;
			wake_up(&hdmi_cec_qw);
			ret = -EIO;
		} else if (hdmi_cec_root.send_error && ret > 1) {
			pr_debug("%s: --- resending msg\n", __func__);
			timeout = jiffies_to_msecs(ret);
			ret = 0;
		} else if (ret > 1) {
			ret = count;
		}
	} while(!ret);

	mutex_unlock(&hdmi_cec_root.m_lock);
	return ret;
}

static void hdmi_cec_hwenable(void)
{
	u8 val;

	pr_debug("%s: \n", __func__);
	hdmi_cec_state = true;

	val = hdmi_readb(HDMI_MC_CLKDIS);
	val &= ~HDMI_MC_CLKDIS_CECCLK_DISABLE;
	hdmi_writeb(val, HDMI_MC_CLKDIS);

	val = HDMI_IH_CEC_STAT0_ERROR_INIT | HDMI_IH_CEC_STAT0_NACK |
		HDMI_IH_CEC_STAT0_EOM | HDMI_IH_CEC_STAT0_DONE;
	hdmi_writeb(val, HDMI_CEC_POLARITY);

	val = HDMI_IH_CEC_STAT0_WAKEUP | HDMI_IH_CEC_STAT0_ERROR_FOLL |
		HDMI_IH_CEC_STAT0_ARB_LOST;
	hdmi_writeb(val, HDMI_CEC_MASK);
	hdmi_writeb(val, HDMI_IH_MUTE_CEC_STAT0);
	hdmi_writeb(0x0, HDMI_CEC_LOCK);
	hdmi_writeb(0x2, HDMI_CEC_CTRL);
}

static void hdmi_cec_hwdisable(void)
{
	u8 val;

	pr_debug("%s: \n", __func__);
	hdmi_cec_state = false;

	hdmi_writeb(0x10, HDMI_CEC_CTRL);

	val = HDMI_IH_CEC_STAT0_WAKEUP | HDMI_IH_CEC_STAT0_ERROR_FOLL |
		HDMI_IH_CEC_STAT0_ERROR_INIT | HDMI_IH_CEC_STAT0_ARB_LOST |
		HDMI_IH_CEC_STAT0_NACK | HDMI_IH_CEC_STAT0_EOM |
		HDMI_IH_CEC_STAT0_DONE;
	hdmi_writeb(val, HDMI_CEC_MASK);
	hdmi_writeb(val, HDMI_IH_MUTE_CEC_STAT0);

	hdmi_writeb(0x0, HDMI_CEC_POLARITY);

	val = hdmi_readb(HDMI_MC_CLKDIS);
	val |= HDMI_MC_CLKDIS_CECCLK_DISABLE;
	hdmi_writeb(val, HDMI_MC_CLKDIS);
}

static long hdmi_cec_set_address(u8 arg, struct hdmi_cec_priv *hdmi_cec)
{

	if (hdmi_cec->la == arg)
		return 0;

	pr_debug("%s: to %d\n", __func__, arg);

	if (arg == 15)
		hdmi_cec_root.nr_ff++;
	if (hdmi_cec->la == 15)
		hdmi_cec_root.nr_ff--;
	else
		hdmi_cec_root.addresses &= ~BIT(hdmi_cec->la);

	hdmi_cec->la = arg;
	if ((u8)arg != 0xff)
		hdmi_cec_root.addresses |= BIT(arg);
	else
		wake_up(&hdmi_cec->hdmi_cec_qm);

	if (!hdmi_cec_root.nr_ff)
		hdmi_cec_root.addresses &= ~BIT(15);

	hdmi_writeb(hdmi_cec_root.addresses & 0xff, HDMI_CEC_ADDR_L);
	/* 
	 * Don't register LA = 15 to with hardware. with it set, broadcast messages are never
	 * sent (considered local by CEC controler)
	 */
	hdmi_writeb((hdmi_cec_root.addresses & 0x7f00) >> 8, HDMI_CEC_ADDR_H);
	return 0;
}

/*!
 * @brief IO ctrl function for vpu file operation
 * @param cmd IO ctrl command
 * @return  0 on success or negative error code on error
 */
static long hdmi_cec_ioctl(struct file *filp, u_int cmd,
		     u_long arg)
{
	int ret = 0, i;
	struct hdmi_cec_priv *hdmi_cec = filp->private_data;

	pr_debug("%s: \n", __func__);

	if (!open_count)
		return -ENODEV;

	switch (cmd) {
	case HDMICEC_IOC_SETLOGICALADDRESS:
		mutex_lock(&hdmi_cec_root.m_lock);
		ret = hdmi_cec_set_address(arg, hdmi_cec);
		mutex_unlock(&hdmi_cec_root.m_lock);
		/*
		 * in case we have more clients, inform them about PA change.
		 * (if libCEC is not in monitoring mode, it won't allow more
		 * clients with same PA - it changes all previous holders of
		 * that 'taken' PA to 1000.
		 * to avoid that, we expand PA further by replacing first empty
		 * dimension with LA (for instance if our PA is 2.2.0.0, we 
		 * change to 2.2.X.0 all concurent clients, where X is actual LA
		 */
		if (open_count > 1 && arg != 15)
			mxc_hdmi_cec_handle(hdmi_cec_root.physical_address);
		break;

	case HDMICEC_IOC_STARTDEVICE:
		if (!hdmi_cec_state)
			hdmi_cec_hwenable();
		break;

	case HDMICEC_IOC_STOPDEVICE:
		hdmi_cec_set_address(-1, hdmi_cec);
		if (hdmi_cec_state && open_count < 2)
			hdmi_cec_hwdisable();
		break;

	case HDMICEC_IOC_GETPHYADDRESS:
		ret = copy_to_user((void __user *)arg, &hdmi_cec_root.physical_address,
					4*sizeof(u8))?-EFAULT:0;
		if (open_count > 1) {
			for (i = 0; i < 4 && ((u8*)arg)[i] != 0x0; i++);
			((u8*)arg)[i] = hdmi_cec->la;
		}
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

void hdmi_cec_start_device(void)
{
	if(open_count && hdmi_cec_ready && !hdmi_cec_state)
		hdmi_cec_hwenable();
}
EXPORT_SYMBOL(hdmi_cec_start_device);

void hdmi_cec_stop_device(void)
{
	if(hdmi_cec_ready && hdmi_cec_state)
		hdmi_cec_hwdisable();
}
EXPORT_SYMBOL(hdmi_cec_stop_device);

/*!
* @brief Release function for vpu file operation
* @return  0 on success or negative error code on error
*/
static int hdmi_cec_release(struct inode *inode, struct file *filp)
{
	struct hdmi_cec_priv *hdmi_cec = filp->private_data;
	struct hdmi_cec_priv *client, *t;
	unsigned long flags;

	pr_debug("%s: \n", __func__);

	mutex_lock(&hdmi_cec_root.m_lock);
	spin_lock_irqsave(&hdmi_cec->i_lock_cl, flags);

	hdmi_cec_set_address(-1, hdmi_cec);
	if (!--open_count)
		hdmi_cec_hwdisable();
	while (!list_empty(&hdmi_cec->msg_head)) {
		struct hdmi_cec_event_list *event = NULL;

		event = list_first_entry(&hdmi_cec->msg_head, struct hdmi_cec_event_list, msg_node);
		list_del(&event->msg_node);
		kfree(event);
	}
	spin_unlock_irqrestore(&hdmi_cec->i_lock_cl, flags);

	list_for_each_entry_safe(client, t, &hdmi_cec_root.client_head, client_node) {
		if (client == hdmi_cec) {
			list_del(&client->client_node);
			filp->private_data = NULL;
			kfree(client);
		}
	}

	mutex_unlock(&hdmi_cec_root.m_lock);
	return 0;
}

static unsigned int hdmi_cec_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;
	struct hdmi_cec_priv *hdmi_cec = file->private_data;

	pr_debug("%s: poll client %lx, la %d\n", __func__, (unsigned long)hdmi_cec, hdmi_cec->la);

	if (hdmi_cec->la == 0xff)
		return POLLHUP;

	poll_wait(file, &hdmi_cec->hdmi_cec_qm, wait);
	poll_wait(file, &hdmi_cec_qw, wait);

	if (!hdmi_cec_root.write_busy)
		mask = (POLLOUT | POLLWRNORM);
	if (!list_empty(&hdmi_cec->msg_head))
		mask |= (POLLIN | POLLRDNORM);

	return mask;
}

const struct file_operations hdmi_cec_fops = {
	.owner = THIS_MODULE,
	.read = hdmi_cec_read,
	.write = hdmi_cec_write,
	.open = hdmi_cec_open,
	.unlocked_ioctl = hdmi_cec_ioctl,
	.release = hdmi_cec_release,
	.poll = hdmi_cec_poll,
};

static int hdmi_cec_dev_probe(struct platform_device *pdev)
{
	int err = 0;
	struct device *temp_class;
	struct resource *res;
	struct pinctrl *pinctrl;
	int irq = platform_get_irq(pdev, 0);

	hdmi_cec_major = register_chrdev(hdmi_cec_major, "mxc_hdmi_cec", &hdmi_cec_fops);
	if (hdmi_cec_major < 0) {
		dev_err(&pdev->dev, "%s: unable to get a major for HDMI CEC\n", __func__);
		err = -EBUSY;
		goto out;
	}

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (unlikely(res == NULL)) {
		dev_err(&pdev->dev, "%s: No HDMI irq line provided\n", __func__);
		goto err_out_chrdev;
	}
	spin_lock_init(&hdmi_cec_root.i_lock);

	err = devm_request_irq(&pdev->dev, irq, mxc_hdmi_cec_isr, IRQF_SHARED,
			dev_name(&pdev->dev), &hdmi_cec_root);
	if (err < 0) {
		dev_err(&pdev->dev, "%s: Unable to request irq: %d\n", __func__, err);
		goto err_out_chrdev;
	}

	hdmi_cec_class = class_create(THIS_MODULE, "mxc_hdmi_cec");
	if (IS_ERR(hdmi_cec_class)) {
		err = PTR_ERR(hdmi_cec_class);
		goto err_out_chrdev;
	}

	temp_class = device_create(hdmi_cec_class, NULL,
			MKDEV(hdmi_cec_major, 0), NULL, "mxc_hdmi_cec");
	if (IS_ERR(temp_class)) {
		err = PTR_ERR(temp_class);
		goto err_out_class;
	}

	pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
	if (IS_ERR(pinctrl)) {
		dev_err(&pdev->dev, "%s: can't get/select CEC pinctrl\n", __func__);
		goto err_out_class;
	}

	init_waitqueue_head(&hdmi_cec_qs);
	init_waitqueue_head(&hdmi_cec_qw);

	INIT_LIST_HEAD(&hdmi_cec_root.client_head);

	mutex_init(&hdmi_cec_root.m_lock);
	hdmi_cec_root.addresses = 0;
	platform_set_drvdata(pdev, &hdmi_cec_root);
	INIT_DELAYED_WORK(&hdmi_cec_root.hdmi_cec_work, mxc_hdmi_cec_worker);
	INIT_DELAYED_WORK(&hdmi_cec_root.hdmi_msg_trigger, mxc_hdmi_cec_msg_trigger);

	dev_info(&pdev->dev, "%s: HDMI CEC initialized\n", __func__);
	hdmi_cec_ready = 1;
	goto out;

err_out_class:
	device_destroy(hdmi_cec_class, MKDEV(hdmi_cec_major, 0));
	class_destroy(hdmi_cec_class);
err_out_chrdev:
	unregister_chrdev(hdmi_cec_major, "mxc_hdmi_cec");
out:
	return err;
}

static int hdmi_cec_dev_remove(struct platform_device *pdev)
{
	if (hdmi_cec_major > 0) {
		flush_scheduled_work();
		device_destroy(hdmi_cec_class, MKDEV(hdmi_cec_major, 0));
		class_destroy(hdmi_cec_class);
		unregister_chrdev(hdmi_cec_major, "mxc_hdmi_cec");
		hdmi_cec_major = 0;
	}
	return 0;
}

static const struct of_device_id imx_hdmi_cec_match[] = {
	{ .compatible = "fsl,imx6q-hdmi-cec", },
	{ .compatible = "fsl,imx6dl-hdmi-cec", },
	{ /* sentinel */ }
};

static struct platform_driver mxc_hdmi_cec_driver = {
	.probe = hdmi_cec_dev_probe,
	.remove = hdmi_cec_dev_remove,
	.driver = {
		.name = "mxc_hdmi_cec",
		.of_match_table	= imx_hdmi_cec_match,
	},
};

module_platform_driver(mxc_hdmi_cec_driver);

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("Linux HDMI CEC driver for Freescale i.MX/MXC");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:mxc_hdmi_cec");

