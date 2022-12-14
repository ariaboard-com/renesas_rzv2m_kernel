/*
 * Watchdog driver for Renesas WDT watchdog
 *
 * Copyright (C) 2015-17 Wolfram Sang, Sang Engineering <wsa@sang-engineering.com>
 * Copyright (C) 2015-17 Renesas Electronics Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/smp.h>
#include <linux/sys_soc.h>
#include <linux/watchdog.h>
#include <linux/delay.h>
#include <linux/reset.h>

#define WDT_DEFAULT_TIMEOUT 60U 	/*Default time out can be set by user*/

#define WDTCNT      0x00
#define WDTSET      0x04
#define WDTTIM      0x08
#define WDTINT      0x0C
#define WDTCNT_WDTEN    BIT(0)
#define WDTINT_INTDISP  BIT(0)

#define F2CYCLE_NSEC(f) (1000000000/f)
#define WDT_CYCLE_MSEC(f, wdttime) ((1024 * 1024 * ((u64)wdttime + 1)) \
                                / ((u64)(f) / 1000000))

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
				__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

struct rzv2m_wdt_priv {
	void __iomem *base;
	struct watchdog_device wdev;
	unsigned long	clk_rate;
	unsigned long	PCLK;
	u32	timeout_min, timeout_max;
	int	irq;
	struct clk *clk;
};

static void rzv2m_wdt_wait_delay(struct rzv2m_wdt_priv *priv)
{
	u32 delay;

	/* Delay timer when change the setting register */
	delay = F2CYCLE_NSEC(priv->PCLK)*6 + F2CYCLE_NSEC(priv->clk_rate)*9;
	ndelay(delay);
}

static int rzv2m_wdt_write(struct rzv2m_wdt_priv *priv, u32 val, unsigned int reg)
{
	int i;
	
	if (reg == WDTSET)
		val &= 0xFFF00000;
	writel_relaxed(val, priv->base + reg);
		
	for (i = 0; i < 10; i++) {
		if (readl_relaxed(priv->base + reg) == val)
			return 0;
		rzv2m_wdt_wait_delay(priv);
	}
	return -ETIMEDOUT;
}

static int rzv2m_wdt_init_timeout(struct watchdog_device *wdev)
{
	struct rzv2m_wdt_priv *priv = watchdog_get_drvdata(wdev);
	u32 time_out;

	/* Clear Lapsed Time Register and clear Interrupt */
	rzv2m_wdt_write(priv, WDTINT_INTDISP, WDTINT);
	/* Initialize timeout value */
	time_out = ((wdev->timeout)/2) * (1000000)/(priv->timeout_min);	
	/* Setting period time register only 12 bit set in WDTSET[31:20] */
	time_out <<= 20;
	/* Delay timer before setting watchdog counter*/
	rzv2m_wdt_wait_delay(priv);
	
	rzv2m_wdt_write(priv, time_out, WDTSET);
	rzv2m_wdt_wait_delay(priv);

	return 0;
}

static irqreturn_t rzv2m_wdt_irq(int irq, void *devid)
{
	return IRQ_HANDLED;
}

static int rzv2m_wdt_start(struct watchdog_device *wdev)
{
	/*Enable clock for reset status*/
	{
		void __iomem *cpg_base = ioremap_nocache(0xA3500000, 0x1000);

		iowrite32(0x70007000, cpg_base + 0x614);        //CPG_RST6
		iowrite32(0xF000F000, cpg_base + 0x428);        //CPG_CLK_ON11
		iowrite32(0x30003000, cpg_base + 0x42C);        //CPG_CLK_ON12
		iounmap(cpg_base);
        }

	struct rzv2m_wdt_priv *priv = watchdog_get_drvdata(wdev);
	
	pm_runtime_get_sync(wdev->parent);
	clk_enable(priv->clk);

	rzv2m_wdt_write(priv, 0, WDTCNT);
	rzv2m_wdt_wait_delay(priv);
	/* Initialize time out */
	rzv2m_wdt_init_timeout(wdev);
	rzv2m_wdt_wait_delay(priv);
	/* Initialize watchdog counter register */
	rzv2m_wdt_write(priv, 0, WDTTIM);
	
	rzv2m_wdt_wait_delay(priv);
	
	set_bit(WDOG_HW_RUNNING, &wdev->status);

	return 0;
}

static int rzv2m_wdt_stop(struct watchdog_device *wdev)
{
	struct rzv2m_wdt_priv *priv = watchdog_get_drvdata(wdev);

	rzv2m_wdt_wait_delay(priv);
	rzv2m_wdt_write(priv, WDTINT_INTDISP, WDTINT);
        {
                /* FIXME: Hard code to access to CPG registers until can find
                * another way. CPG_RST6 register is belong to CPG used
                * to mask/unmask WDT overflow system reset
                */
                void __iomem *cpg_base = ioremap_nocache(0xA3500000, 0x1000);

                iowrite32(0x70000000, cpg_base + 0x614);        //CPG_RST6
                iowrite32(0xF0000000, cpg_base + 0x428);        //CPG_CLK_ON11
                iowrite32(0x30000000, cpg_base + 0x42C);        //CPG_CLK_ON12
                iounmap(cpg_base);
        }
	
	clk_disable(priv->clk);

	pm_runtime_put(wdev->parent);
	
	return 0;
}

static const struct watchdog_info rzv2m_wdt_ident = {
	.options = WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT |
		WDIOF_CARDRESET,
	.identity = "Renesas WDT Watchdog",
};

static int rzv2m_wdt_ping(struct watchdog_device *wdev)
{
	struct rzv2m_wdt_priv *priv = watchdog_get_drvdata(wdev);

	rzv2m_wdt_wait_delay(priv);
	rzv2m_wdt_write(priv, WDTINT_INTDISP, WDTINT);
	rzv2m_wdt_wait_delay(priv);
	
	/* Enable watchdog timer*/
	rzv2m_wdt_write(priv, WDTCNT_WDTEN, WDTCNT);
	rzv2m_wdt_wait_delay(priv);

	return 0;
}

static const struct watchdog_ops rzv2m_wdt_ops = {
	.owner = THIS_MODULE,
	.start = rzv2m_wdt_start,
	.stop = rzv2m_wdt_stop,
	.ping = rzv2m_wdt_ping,
};

static int rzv2m_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rzv2m_wdt_priv *priv;
	struct resource *res;
	struct clk *clks[2];
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(priv->clk)) {
		/*
		 * Clock framework support is optional, continue on
		 * anyways if we don't find a matching clock.
		 */
		priv->clk = NULL;
	}

	pm_runtime_enable(&pdev->dev);
        pm_runtime_get_sync(&pdev->dev);

	ret = clk_prepare_enable(priv->clk);
	if(ret)	{
		dev_err(dev, "Unable to enable clock\n");
		return ret;
	}
	
	{
		/* FIXME: Hard code to access to CPG registers until can find
		* another way. CPG_RST6 register is belong to CPG used
		* to mask/unmask WDT overflow system reset
		*/
		void __iomem *cpg_base = ioremap_nocache(0xA3500000, 0x1000);
		
		iowrite32(0x000F000F, cpg_base + 0x504);	//CPG_RST_MSK
		iowrite32(0x70007000, cpg_base + 0x614);	//CPG_RST6
		iowrite32(0xF000F000, cpg_base + 0x428);	//CPG_CLK_ON11
		iowrite32(0x30003000, cpg_base + 0x42C);	//CPG_CLK_ON12
		iounmap(cpg_base);
	}

	clks[0] = of_clk_get(pdev->dev.of_node, 0);
	if (IS_ERR(clks[0]))
		return PTR_ERR(clks[0]);

	clks[1] = of_clk_get(pdev->dev.of_node, 1);
	if (IS_ERR(clks[1]))
		return PTR_ERR(clks[0]);

	/*Get watchdog clock */
	priv->PCLK = clk_get_rate(clks[0]);
	if (!priv->PCLK) {
		ret = -ENOENT;
		goto out_pm_disable;
	}
	
	/*Get Peripheral clock */
	priv->clk_rate = clk_get_rate(clks[1]);
	if (!priv->clk_rate) {
		ret = -ENOENT;
		goto out_pm_disable;
	}

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq > 0) {
		ret = devm_request_irq(&pdev->dev, priv->irq,
			rzv2m_wdt_irq, 0, pdev->name, priv);
		if (ret < 0)
			dev_warn(&pdev->dev, "failed to request IRQ\n");
	}
	else
		dev_err(&pdev->dev, "IRQ index 0 not found\n");
	
	/* time out min microsecond */
	priv->timeout_min = WDT_CYCLE_MSEC(priv->clk_rate, 0);
	printk("timeout_min = %u\n", priv->timeout_min);
	/* time out max microsecond */
	priv->timeout_max = WDT_CYCLE_MSEC(priv->clk_rate, 0xfff);
	printk("timeout_max = %u\n", priv->timeout_max);

	priv->wdev.info = &rzv2m_wdt_ident;
	priv->wdev.ops = &rzv2m_wdt_ops;
	priv->wdev.parent = dev;
	priv->wdev.min_timeout = 1;
	priv->wdev.max_timeout = priv->timeout_max;
	priv->wdev.timeout = WDT_DEFAULT_TIMEOUT;

	platform_set_drvdata(pdev, priv);
	watchdog_set_drvdata(&priv->wdev, priv);
	watchdog_set_nowayout(&priv->wdev, nowayout);
	watchdog_set_restart_priority(&priv->wdev, 0);
	watchdog_stop_on_unregister(&priv->wdev);

	/* This overrides the default timeout only if DT configuration was found */
	ret = watchdog_init_timeout(&priv->wdev, 0, &pdev->dev);
	if (ret)
		dev_warn(dev, "Specified timeout value invalid, using default\n");
	
	ret = watchdog_register_device(&priv->wdev);
	if (ret < 0)
		goto out_pm_disable;

	return 0;
	
out_pm_disable:
	pm_runtime_disable(dev);
	return ret;
}

static int rzv2m_wdt_remove(struct platform_device *pdev)
{
	struct rzv2m_wdt_priv *priv = platform_get_drvdata(pdev);

	watchdog_unregister_device(&priv->wdev);

	return 0;
}

static const struct of_device_id rzv2m_wdt_ids[] = {
	{ .compatible = "renesas,rzv2m-wdt", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rzv2m_wdt_ids);

static struct platform_driver rzv2m_wdt_driver = {
	.driver = {
		.name = "rzv2m_wdt",
		.of_match_table = rzv2m_wdt_ids,
	},
	.probe = rzv2m_wdt_probe,
	.remove = rzv2m_wdt_remove,
};
module_platform_driver(rzv2m_wdt_driver);

MODULE_DESCRIPTION("RZ/V2M WDT driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Renesas Electronics Corporation");
