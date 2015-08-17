/*
 * Power off by restarting and let u-boot keep hold of the machine
 * until the user presses a button for example.
 *
 * Andrew Lunn <andrew@lunn.ch>
 *
 * Copyright (C) 2012 Andrew Lunn
 * Copyright (C) 2015 Jon Nettleton
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <asm/system_misc.h>

extern void snvs_poweroff(void);

static int snvs_poweroff_probe(struct platform_device *pdev)
{
	/*
	 * if no specific power off function in board file, power off system by
	 * SNVS
	 */
	if (!pm_power_off)
		pm_power_off = snvs_poweroff;

	return 0;
}

static int snvs_poweroff_remove(struct platform_device *pdev)
{
        if (pm_power_off == &snvs_poweroff)
                pm_power_off = NULL;

        return 0;
}

static const struct of_device_id of_snvs_poweroff_match[] = {
        { .compatible = "fsl,snvs-poweroff", },
        {},
};

static struct platform_driver snvs_poweroff_driver = {
        .probe = snvs_poweroff_probe,
        .remove = snvs_poweroff_remove,
        .driver = {
                .name = "snvs-poweroff",
                .owner = THIS_MODULE,
                .of_match_table = of_snvs_poweroff_match,
        },
};
module_platform_driver(snvs_poweroff_driver);
