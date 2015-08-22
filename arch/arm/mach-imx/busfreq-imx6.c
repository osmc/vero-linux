/*
 * Copyright (C) 2011-2015 Freescale Semiconductor, Inc. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*!
 * @file busfreq-imx6.c
 *
 * @brief A common API for the Freescale Semiconductor iMX6 Busfreq API
 *
 * The APIs are for setting bus frequency to different values based on the
 * highest freqeuncy requested.
 *
 * @ingroup PM
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <asm/cacheflush.h>
#include <asm/fncpy.h>
#include <asm/io.h>
#include <asm/mach/map.h>
#include <asm/mach-types.h>
#include <asm/tlb.h>
#include <linux/busfreq-imx6.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/reboot.h>
#include <linux/regulator/consumer.h>
#include <linux/sched.h>
#include <linux/suspend.h>
#include "../../../drivers/clk/imx/clk.h"
#include "hardware.h"
#include "common.h"

int vpu352 = 1;

static RAW_NOTIFIER_HEAD(busfreq_notifier_chain);

int register_busfreq_notifier(struct notifier_block *nb)
{
	return raw_notifier_chain_register(&busfreq_notifier_chain, nb);
}
EXPORT_SYMBOL(register_busfreq_notifier);

int unregister_busfreq_notifier(struct notifier_block *nb)
{
	return raw_notifier_chain_unregister(&busfreq_notifier_chain, nb);
}
EXPORT_SYMBOL(unregister_busfreq_notifier);

void request_bus_freq(enum bus_freq_mode mode)
{
	return;
}
EXPORT_SYMBOL(request_bus_freq);

void release_bus_freq(enum bus_freq_mode mode)
{
	return;
}
EXPORT_SYMBOL(release_bus_freq);

static int vpu352_setup(char *options)
{
	return kstrtol(options, 0, (long int *)&vpu352);
}

/*!
 * This is the probe routine for the bus frequency driver.
 *
 * @param   pdev   The platform device structure
 *
 * @return         The function returns 0 on success
 *
 */

static int busfreq_probe(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id imx6_busfreq_ids[] = {
	{ .compatible = "fsl,imx6_busfreq", },
	{ /* sentinel */ }
};

static struct platform_driver busfreq_driver = {
	.driver = {
		.name = "imx6_busfreq",
		.owner  = THIS_MODULE,
		.of_match_table = imx6_busfreq_ids,
		},
	.probe = busfreq_probe,
};

/*!
 * Initialise the busfreq_driver.
 *
 * @return  The function always returns 0.
 */

static int __init busfreq_init(void)
{
	if (vpu352) {
		printk(KERN_INFO "VPU@352Mhz activated. Bus freq driver module inactive\n");
		return 0;
	}

	if (platform_driver_register(&busfreq_driver) != 0)
		return -ENODEV;

	if (platform_driver_register(&busfreq_driver) != 0)
		return -ENODEV;

	printk(KERN_INFO "Bus freq driver module loaded\nVPU@352Mhz is not activated\n");
	return 0;
}

static void __exit busfreq_cleanup(void)
{
	/* Unregister the device structure */
	platform_driver_unregister(&busfreq_driver);
}

__setup("vpu352=", vpu352_setup);
core_initcall(busfreq_init);
module_exit(busfreq_cleanup);

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("BusFreq driver");
MODULE_LICENSE("GPL");
