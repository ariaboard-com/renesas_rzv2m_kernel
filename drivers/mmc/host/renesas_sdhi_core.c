/*
 * Renesas SDHI
 *
 * Copyright (C) 2015-17 Renesas Electronics Corporation
 * Copyright (C) 2016-17 Sang Engineering, Wolfram Sang
 * Copyright (C) 2016-17 Horms Solutions, Simon Horman
 * Copyright (C) 2009 Magnus Damm
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Based on "Compaq ASIC3 support":
 *
 * Copyright 2001 Compaq Computer Corporation.
 * Copyright 2004-2005 Phil Blundell
 * Copyright 2007-2008 OpenedHand Ltd.
 *
 * Authors: Phil Blundell <pb@handhelds.org>,
 *	    Samuel Ortiz <sameo@openedhand.com>
 *
 */

#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/mmc/host.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/mfd/tmio.h>
#include <linux/sh_dma.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/sys_soc.h>
#include <linux/dma-mapping.h>

#include "renesas_sdhi.h"
#include "tmio_mmc.h"

#define HOST_MODE		0xe4

#define SDHI_VER_GEN2_SDR50	0x490c
#define SDHI_VER_RZ_A1		0x820b
/* very old datasheets said 0x490c for SDR104, too. They are wrong! */
#define SDHI_VER_GEN2_SDR104	0xcb0d
#define SDHI_VER_GEN3_SD	0xcc10
#define SDHI_VER_GEN3_SDMMC	0xcd10

#define B2REG_PSC_BASE_ADDR     (0x00000000A3700000ULL)
#define B2REG_PSC_SDSEL_OFSET   (0x0080)

#define MAX_CHIP_NAME_SIZE     (20)
#define RZV2M_DEVICE_NAME_SD0  "85000000.sd"
#define RZV2M_DEVICE_NAME_SD1  "85010000.sd"
#define RZV2M_DEVICE_NAME_EMMC "85020000.sd"


#define PSC_SEL_SD0                    (0)  //SD0 select
#define PSC_SEL_SD1                    (1)  //SD1 select

#define        PSC_REG_SD_3_3V         (0)
#define        PSC_REG_SD_1_8V         (1)



enum switching_voltage_mode{
       psc_mode,
       none,
};

static int psc_voltage_switch(uint32_t pins, uint8_t voltage)
{

    uint32_t data;
       void __iomem * remap_addr;

       remap_addr = ioremap(B2REG_PSC_BASE_ADDR,sizeof(*remap_addr));
       if (!remap_addr) {
        return -EINVAL;
    }

       data = (voltage << pins) | BIT((pins+16));
       writel(data , remap_addr + B2REG_PSC_SDSEL_OFSET);
       iounmap(remap_addr);

       return 0;
}

static void renesas_sdhi_sdbuf_width(struct tmio_mmc_host *host, int width)
{
	u32 val;

	/*
	 * see also
	 *	renesas_sdhi_of_data :: dma_buswidth
	 */
	switch (sd_ctrl_read16(host, CTL_VERSION)) {
	case SDHI_VER_GEN2_SDR50:
		val = (width == 32) ? 0x0001 : 0x0000;
		break;
	case SDHI_VER_GEN2_SDR104:
		val = (width == 32) ? 0x0000 : 0x0001;
		break;
	case SDHI_VER_GEN3_SD:
	case SDHI_VER_GEN3_SDMMC:
		if (width == 64)
			val = 0x0000;
		else if (width == 32)
			val = 0x0101;
		else
			val = 0x0001;
		break;
	default:
		/* nothing to do */
		return;
	}

	sd_ctrl_write16(host, HOST_MODE, val);
}

static int renesas_sdhi_clk_enable(struct tmio_mmc_host *host)
{
	struct mmc_host *mmc = host->mmc;
	struct renesas_sdhi *priv = host_to_priv(host);
	int ret = clk_prepare_enable(priv->clk);

	if (ret < 0)
		return ret;

	ret = clk_prepare_enable(priv->clk_cd);
	if (ret < 0) {
		clk_disable_unprepare(priv->clk);
		return ret;
	}

	/*
	 * The clock driver may not know what maximum frequency
	 * actually works, so it should be set with the max-frequency
	 * property which will already have been read to f_max.  If it
	 * was missing, assume the current frequency is the maximum.
	 */
	if (!mmc->f_max)
		mmc->f_max = clk_get_rate(priv->clk);

	/*
	 * Minimum frequency is the minimum input clock frequency
	 * divided by our maximum divider.
	 */
	mmc->f_min = max(clk_round_rate(priv->clk, 1) / 512, 1L);

	/* enable 16bit data access on SDBUF as default */
	renesas_sdhi_sdbuf_width(host, 16);

	return 0;
}

static unsigned int renesas_sdhi_clk_update(struct tmio_mmc_host *host,
					    unsigned int new_clock)
{
	struct renesas_sdhi *priv = host_to_priv(host);
	unsigned int freq, diff, best_freq = 0, diff_min = ~0;
	int i, ret;

	/* tested only on R-Car Gen2+ currently; may work for others */
	if (!(host->pdata->flags & TMIO_MMC_MIN_RCAR2))
		return clk_get_rate(priv->clk);

	/*
	 * We want the bus clock to be as close as possible to, but no
	 * greater than, new_clock.  As we can divide by 1 << i for
	 * any i in [0, 9] we want the input clock to be as close as
	 * possible, but no greater than, new_clock << i.
	 */
	for (i = min(9, ilog2(UINT_MAX / new_clock)); i >= 0; i--) {
		freq = clk_round_rate(priv->clk, new_clock << i);
		if (freq > (new_clock << i)) {
			/* Too fast; look for a slightly slower option */
			freq = clk_round_rate(priv->clk,
					      (new_clock << i) / 4 * 3);
			if (freq > (new_clock << i))
				continue;
		}

		diff = new_clock - (freq >> i);
		if (diff <= diff_min) {
			best_freq = freq;
			diff_min = diff;
		}
	}

	ret = clk_set_rate(priv->clk, best_freq);

	return ret == 0 ? best_freq : clk_get_rate(priv->clk);
}

static void renesas_sdhi_clk_disable(struct tmio_mmc_host *host)
{
	struct renesas_sdhi *priv = host_to_priv(host);

	clk_disable_unprepare(priv->clk);
	clk_disable_unprepare(priv->clk_cd);
}

static int renesas_sdhi_card_busy(struct mmc_host *mmc)
{
	struct tmio_mmc_host *host = mmc_priv(mmc);

	return !(sd_ctrl_read16_and_16_as_32(host, CTL_STATUS) &
		 TMIO_STAT_DAT0);
}

static int renesas_sdhi_start_signal_voltage_switch(struct mmc_host *mmc,
						    struct mmc_ios *ios)
{
	struct tmio_mmc_host *host = mmc_priv(mmc);
	struct renesas_sdhi *priv = host_to_priv(host);
       u8 psc_state;

       if(priv->switching_volt_type == psc_mode){
               switch (ios->signal_voltage) {
               case MMC_SIGNAL_VOLTAGE_330:
                               psc_state = PSC_REG_SD_3_3V;
                       break;
               case MMC_SIGNAL_VOLTAGE_180:
                               psc_state = PSC_REG_SD_1_8V;
                       break;
               default:
                       return -EINVAL;
               }
               return psc_voltage_switch(priv->psc_pins, psc_state);
       }
       else{
               //The voltage of eMMC is fixed at 1.8V.
       }

       return 0;
}

/* SCC registers */
#define SH_MOBILE_SDHI_SCC_DTCNTL	0x000
#define SH_MOBILE_SDHI_SCC_TAPSET	0x002
#define SH_MOBILE_SDHI_SCC_DT2FF	0x004
#define SH_MOBILE_SDHI_SCC_CKSEL	0x006
#define SH_MOBILE_SDHI_SCC_RVSCNTL	0x008
#define SH_MOBILE_SDHI_SCC_RVSREQ	0x00A
#define SH_MOBILE_SDHI_SCC_SMPCMP	0x00C
#define SH_MOBILE_SDHI_SCC_TMPPORT2	0x00E
#define SH_MOBILE_SDHI_SCC_TMPPORT3	0x014
#define SH_MOBILE_SDHI_SCC_TMPPORT4	0x016
#define SH_MOBILE_SDHI_SCC_TMPPORT5	0x018
#define SH_MOBILE_SDHI_SCC_TMPPORT6	0x01A
#define SH_MOBILE_SDHI_SCC_TMPPORT7	0x01C

/* Definitions for values the SH_MOBILE_SDHI_SCC_DTCNTL register */
#define SH_MOBILE_SDHI_SCC_DTCNTL_TAPEN		BIT(0)
#define SH_MOBILE_SDHI_SCC_DTCNTL_TAPNUM_SHIFT	16
#define SH_MOBILE_SDHI_SCC_DTCNTL_TAPNUM_MASK	0xff

/* Definitions for values the SH_MOBILE_SDHI_SCC_CKSEL register */
#define SH_MOBILE_SDHI_SCC_CKSEL_DTSEL		BIT(0)
/* Definitions for values the SH_MOBILE_SDHI_SCC_RVSCNTL register */
#define SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN	BIT(0)
/* Definitions for values the SH_MOBILE_SDHI_SCC_RVSREQ register */
#define SH_MOBILE_SDHI_SCC_RVSREQ_RVSERR	BIT(2)
#define SH_MOBILE_SDHI_SCC_RVSREQ_REQTAPUP	BIT(1)
#define SH_MOBILE_SDHI_SCC_RVSREQ_REQTAPDOWN	BIT(0)
/* Definitions for values the SH_MOBILE_SDHI_SCC_SMPCMP register */
#define SH_MOBILE_SDHI_SCC_SMPCMP_CMD_ERR	(BIT(24) | BIT(8))
#define SH_MOBILE_SDHI_SCC_SMPCMP_CMD_REQUP	BIT(24)
#define SH_MOBILE_SDHI_SCC_SMPCMP_CMD_REQDOWN	BIT(8)
/* Definitions for values the SH_MOBILE_SDHI_SCC_TMPPORT2 register */
#define SH_MOBILE_SDHI_SCC_TMPPORT2_HS400OSEL	BIT(4)
#define SH_MOBILE_SDHI_SCC_TMPPORT2_HS400EN	BIT(31)

/* Definitions for values the SH_MOBILE_SDHI_SCC_TMPPORT3 register */
#define SH_MOBILE_SDHI_SCC_TMPPORT3_OFFSET_0	3
#define SH_MOBILE_SDHI_SCC_TMPPORT3_OFFSET_1	2
#define SH_MOBILE_SDHI_SCC_TMPPORT3_OFFSET_2	1
#define SH_MOBILE_SDHI_SCC_TMPPORT3_OFFSET_3	0
#define SH_MOBILE_SDHI_SCC_TMPPORT3_OFFSET_MASK		0x3

/* Definitions for values the SH_MOBILE_SDHI_SCC_TMPPORT4 register */
#define SH_MOBILE_SDHI_SCC_TMPPORT4_DLL_ACC_START	BIT(0)

/* Definitions for values the SH_MOBILE_SDHI_SCC_TMPPORT5 register */
#define SH_MOBILE_SDHI_SCC_TMPPORT5_DLL_RW_SEL_R	BIT(8)
#define SH_MOBILE_SDHI_SCC_TMPPORT5_DLL_RW_SEL_W	(0 << 8)
#define SH_MOBILE_SDHI_SCC_TMPPORT5_DLL_ADR_MASK	0x3F

/* Definitions for values the SH_MOBILE_SDHI_SCC register */
#define SH_MOBILE_SDHI_SCC_TMPPORT_DISABLE_WP_CODE	0xa5000000
#define SH_MOBILE_SDHI_SCC_TMPPORT_CALIB_CODE_MASK	0x1f
#define SH_MOBILE_SDHI_SCC_TMPPORT_MANUAL_MODE		BIT(7)
#define CALIB_TABLE_MAX		(SH_MOBILE_SDHI_SCC_TMPPORT_CALIB_CODE_MASK + 1)

static const u32 r8a7796_rev1_calib_table[2][CALIB_TABLE_MAX] = {
	{ 3,  3,  3,  3,  3,  3,  3,  4,  4,  5,  6,  7,  8,  9, 10, 15,
	 16, 16, 16, 16, 16, 16, 17, 18, 18, 19, 20, 21, 22, 23, 24, 25 },
	{ 5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  6,  7,  8, 11,
	 12, 17, 18, 18, 18, 18, 18, 18, 18, 19, 20, 21, 22, 23, 25, 25 }
};

static const u32 r8a77965_calib_table[2][CALIB_TABLE_MAX] = {
	{ 1,  2,  6,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 15, 15, 16,
	 17, 18, 19, 20, 21, 22, 23, 24, 25, 25, 26, 27, 28, 29, 30, 31 },
	{ 2,  3,  4,  4,  5,  6,  7,  9, 10, 11, 12, 13, 14, 15, 16, 17,
	 17, 17, 20, 21, 22, 23, 24, 25, 27, 28, 29, 30, 31, 31, 31, 31 }
};

static const u32 r8a77990_calib_table[2][CALIB_TABLE_MAX] = {
	{ 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },
	{ 0,  0,  0,  1,  2,  3,  3,  4,  4,  4,  5,  5,  6,  8,  9, 10,
	 11, 12, 13, 15, 16, 17, 17, 18, 18, 19, 20, 22, 24, 25, 26, 26 }
};

static inline u32 sd_scc_read32(struct tmio_mmc_host *host,
				struct renesas_sdhi *priv, int addr)
{
	return readl(priv->scc_ctl + (addr << host->bus_shift));
}

static inline void sd_scc_write32(struct tmio_mmc_host *host,
				  struct renesas_sdhi *priv,
				  int addr, u32 val)
{
	writel(val, priv->scc_ctl + (addr << host->bus_shift));
}

static unsigned int renesas_sdhi_init_tuning(struct tmio_mmc_host *host)
{
	struct renesas_sdhi *priv;

	priv = host_to_priv(host);

	/* Initialize SCC */
	sd_ctrl_write32_as_16_and_16(host, CTL_STATUS, 0x0);

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, ~CLK_CTL_SCLKEN &
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	/* set sampling clock selection range */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_DTCNTL,
		       SH_MOBILE_SDHI_SCC_DTCNTL_TAPEN |
		       0x8 << SH_MOBILE_SDHI_SCC_DTCNTL_TAPNUM_SHIFT);

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_CKSEL,
		       SH_MOBILE_SDHI_SCC_CKSEL_DTSEL |
		       sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_CKSEL));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL,
		       ~SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN &
		       sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_DT2FF, priv->scc_tappos);

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, CLK_CTL_SCLKEN |
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	/* Read TAPNUM */
	return (sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_DTCNTL) >>
		SH_MOBILE_SDHI_SCC_DTCNTL_TAPNUM_SHIFT) &
		SH_MOBILE_SDHI_SCC_DTCNTL_TAPNUM_MASK;
}

static void renesas_sdhi_prepare_tuning(struct tmio_mmc_host *host,
					unsigned long tap)
{
	struct renesas_sdhi *priv = host_to_priv(host);

	priv->doing_tune = true;

	/* Set sampling clock position */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TAPSET, tap);
}

static void renesas_sdhi_hs400_complete(struct tmio_mmc_host *host)
{
	struct renesas_sdhi *priv = host_to_priv(host);

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, ~CLK_CTL_SCLKEN &
		sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	/* Set HS400 mode */
	sd_ctrl_write16(host, CTL_SDIF_MODE, 0x0001 |
			sd_ctrl_read16(host, CTL_SDIF_MODE));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_DT2FF,
		       priv->scc_tappos_hs400);

	/* Gen3 can't do automatic tap correction with HS400, so disable it */
	if (sd_ctrl_read16(host, CTL_VERSION) == SDHI_VER_GEN3_SDMMC)
		sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL,
			       ~SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN &
			       sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT2,
		       (SH_MOBILE_SDHI_SCC_TMPPORT2_HS400EN |
			SH_MOBILE_SDHI_SCC_TMPPORT2_HS400OSEL) |
			sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT2));

	/* Set the sampling clock selection range of HS400 mode */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_DTCNTL,
		       SH_MOBILE_SDHI_SCC_DTCNTL_TAPEN |
		       0x4 << SH_MOBILE_SDHI_SCC_DTCNTL_TAPNUM_SHIFT);


	if (priv->quirks && priv->quirks->hs400_4taps)
		sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TAPSET,
			       host->tap_set / 2);

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_CKSEL,
		       SH_MOBILE_SDHI_SCC_CKSEL_DTSEL |
		       sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_CKSEL));

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, CLK_CTL_SCLKEN |
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));
}

static void renesas_sdhi_reset_scc(struct tmio_mmc_host *host,
				   struct renesas_sdhi *priv)
{
	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, ~CLK_CTL_SCLKEN &
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_CKSEL,
		       ~SH_MOBILE_SDHI_SCC_CKSEL_DTSEL &
		       sd_scc_read32(host, priv,
				     SH_MOBILE_SDHI_SCC_CKSEL));
}

static void renesas_sdhi_disable_scc(struct tmio_mmc_host *host)
{
	struct renesas_sdhi *priv = host_to_priv(host);

	renesas_sdhi_reset_scc(host, priv);

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_DTCNTL,
		       ~SH_MOBILE_SDHI_SCC_DTCNTL_TAPEN &
		       sd_scc_read32(host, priv,
				     SH_MOBILE_SDHI_SCC_DTCNTL));

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, CLK_CTL_SCLKEN |
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));
}

static void renesas_sdhi_reset_hs400_mode(struct tmio_mmc_host *host,
					  struct renesas_sdhi *priv)
{
	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, ~CLK_CTL_SCLKEN &
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	/* Reset HS400 mode */
	sd_ctrl_write16(host, CTL_SDIF_MODE, ~0x0001 &
			sd_ctrl_read16(host, CTL_SDIF_MODE));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_DT2FF, priv->scc_tappos);

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT2,
		       ~(SH_MOBILE_SDHI_SCC_TMPPORT2_HS400EN |
			 SH_MOBILE_SDHI_SCC_TMPPORT2_HS400OSEL) &
			sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT2));
	if (host->adjust_hs400_mode_disable)
		host->adjust_hs400_mode_disable(host->mmc);

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, CLK_CTL_SCLKEN |
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));
}

static void renesas_sdhi_prepare_hs400_tuning(struct tmio_mmc_host *host)
{
	renesas_sdhi_reset_hs400_mode(host, host_to_priv(host));
	/* execute adjust hs400 offset after setting to HS400 mode */
	host->needs_adjust_hs400 = true;
}

static u32 sd_scc_tmpport_read32(struct tmio_mmc_host *host,
				 struct renesas_sdhi *priv, u32 addr)
{
	/* read mode */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT5,
		       SH_MOBILE_SDHI_SCC_TMPPORT5_DLL_RW_SEL_R |
		       (SH_MOBILE_SDHI_SCC_TMPPORT5_DLL_ADR_MASK & addr));

	/* access start and stop */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT4,
		       SH_MOBILE_SDHI_SCC_TMPPORT4_DLL_ACC_START);
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT4, 0);

	return sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT7);
}

static void sd_scc_tmpport_write32(struct tmio_mmc_host *host,
				   struct renesas_sdhi *priv, u32 addr, u32 val)
{
	/* write mode */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT5,
		       SH_MOBILE_SDHI_SCC_TMPPORT5_DLL_RW_SEL_W |
		       (SH_MOBILE_SDHI_SCC_TMPPORT5_DLL_ADR_MASK & addr));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT6, val);

	/* access start and stop */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT4,
		       SH_MOBILE_SDHI_SCC_TMPPORT4_DLL_ACC_START);
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT4, 0);
}

static void renesas_sdhi_adjust_hs400_mode_enable(struct mmc_host *mmc)
{
	struct tmio_mmc_host *host = mmc_priv(mmc);
	struct renesas_sdhi *priv = host_to_priv(host);
	u32 calib_code;

	if (!priv->adjust_hs400_calib_table)
		return;

	/* Enabled Manual adjust HS400 mode
	 *
	 * 1) Disabled Write Protect
	 *    W(addr=0x00, WP_DISABLE_CODE)
	 * 2) Read Calibration code
	 *    read_value = R(addr=0x26)
	 * 3) Refer to calibration table
	 *    Calibration code = table[read_value]
	 * 4) Enabled Manual Calibration
	 *    W(addr=0x22, manual mode | Calibration code)
	 * 5) Set Offset value to TMPPORT3 Reg
	 */

	sd_scc_tmpport_write32(host, priv, 0x00,
			       SH_MOBILE_SDHI_SCC_TMPPORT_DISABLE_WP_CODE);
	calib_code = sd_scc_tmpport_read32(host, priv, 0x26);
	calib_code &= SH_MOBILE_SDHI_SCC_TMPPORT_CALIB_CODE_MASK;
	sd_scc_tmpport_write32(host, priv, 0x22,
			       SH_MOBILE_SDHI_SCC_TMPPORT_MANUAL_MODE |
			       priv->adjust_hs400_calib_table[calib_code]);
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT3,
		       priv->adjust_hs400_offset);

	/* Clear flag */
	host->needs_adjust_hs400 = false;
}

static void renesas_sdhi_adjust_hs400_mode_disable(struct mmc_host *mmc)
{
	struct tmio_mmc_host *host = mmc_priv(mmc);
	struct renesas_sdhi *priv = host_to_priv(host);

	/* Disabled Manual adjust HS400 mode
	 *
	 * 1) Disabled Write Protect
	 *    W(addr=0x00, WP_DISABLE_CODE)
	 * 2) Disabled Manual Calibration
	 *    W(addr=0x22, 0)
	 * 3) Clear offset value to TMPPORT3 Reg
	 */
	sd_scc_tmpport_write32(host, priv, 0x00,
			       SH_MOBILE_SDHI_SCC_TMPPORT_DISABLE_WP_CODE);
	sd_scc_tmpport_write32(host, priv, 0x22, 0);
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TMPPORT3, 0);
}
#define SH_MOBILE_SDHI_MAX_TAP 3

static int renesas_sdhi_select_tuning(struct tmio_mmc_host *host)
{
	struct renesas_sdhi *priv = host_to_priv(host);
	unsigned long tap_cnt;  /* counter of tuning success */
	unsigned long tap_start;/* start position of tuning success */
	unsigned long tap_end;  /* end position of tuning success */
	unsigned long ntap;     /* temporary counter of tuning success */
	unsigned long i;

	priv->doing_tune = false;

	/* clear flag */
	host->needs_adjust_hs400 = false;

	/* Clear SCC_RVSREQ */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSREQ, 0);

	/*
	 * When tuning CMD19 is issued twice for each tap, merge the
	 * result requiring the tap to be good in both runs before
	 * considering it for tuning selection.
	 */
	for (i = 0; i < host->tap_num * 2; i++) {
		int offset = host->tap_num * (i < host->tap_num ? 1 : -1);

		if (!test_bit(i, host->taps))
			clear_bit(i + offset, host->taps);
	}

	/*
	 * Find the longest consecutive run of successful probes.  If that
	 * is more than SH_MOBILE_SDHI_MAX_TAP probes long then use the
	 * center index as the tap.
	 */
	tap_cnt = 0;
	ntap = 0;
	tap_start = 0;
	tap_end = 0;
	for (i = 0; i < host->tap_num * 2; i++) {
		if (test_bit(i, host->taps)) {
			ntap++;
		} else {
			if (ntap > tap_cnt) {
				tap_start = i - ntap;
				tap_end = i - 1;
				tap_cnt = ntap;
			}
			ntap = 0;
		}
	}

	if (ntap > tap_cnt) {
		tap_start = i - ntap;
		tap_end = i - 1;
		tap_cnt = ntap;
	}

	if (tap_cnt >= SH_MOBILE_SDHI_MAX_TAP)
		host->tap_set = (tap_start + tap_end) / 2 % host->tap_num;
	else
		return -EIO;

	/* Set SCC */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TAPSET, host->tap_set);

	/* Enable auto re-tuning */
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL,
		       SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN |
		       sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL));

	return 0;
}
static bool renesas_sdhi_manual_correction(struct tmio_mmc_host *host, bool use_4tap)
{
	struct renesas_sdhi *priv = host_to_priv(host);
	unsigned long new_tap = host->tap_set;
	u32 val;

	val = sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSREQ);
	if (!val)
		return false;

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSREQ, 0);

	/* Change TAP position according to correction status */
	if (sd_ctrl_read16(host, CTL_VERSION) == SDHI_VER_GEN3_SDMMC &&
	    host->mmc->ios.timing == MMC_TIMING_MMC_HS400) {
		/*
		 * With HS400, the DAT signal is based on DS, not CLK.
		 * Therefore, use only CMD status.
		 */
		u32 smpcmp = sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_SMPCMP) &
					   SH_MOBILE_SDHI_SCC_SMPCMP_CMD_ERR;
		if (!smpcmp)
			return false;   /* no error in CMD signal */
		else if (smpcmp == SH_MOBILE_SDHI_SCC_SMPCMP_CMD_REQUP)
			new_tap++;
		else if (smpcmp == SH_MOBILE_SDHI_SCC_SMPCMP_CMD_REQDOWN)
			new_tap--;
		else
			return true;    /* need retune */
	} else {
		if (val & SH_MOBILE_SDHI_SCC_RVSREQ_RVSERR)
			return true;    /* need retune */
		else if (val & SH_MOBILE_SDHI_SCC_RVSREQ_REQTAPUP)
			new_tap++;
		else if (val & SH_MOBILE_SDHI_SCC_RVSREQ_REQTAPDOWN)
			new_tap--;
		else
			return false;
	}

	host->tap_set = (new_tap % host->tap_num);
	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_TAPSET,
		       host->tap_set / (use_4tap ? 2 : 1));

	return false;
}

static bool renesas_sdhi_auto_correction(struct tmio_mmc_host *host)
{
	struct renesas_sdhi *priv = host_to_priv(host);

	/* Check SCC error */
	if (sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSREQ) &
	    SH_MOBILE_SDHI_SCC_RVSREQ_RVSERR) {
		sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSREQ, 0);
		return true;
	}

	return false;
}

static bool renesas_sdhi_check_scc_error(struct tmio_mmc_host *host)
{
	struct renesas_sdhi *priv = host_to_priv(host);
	bool use_4tap = priv->quirks && priv->quirks->hs400_4taps;

	if (!(host->mmc->ios.timing == MMC_TIMING_UHS_SDR104) &&
	    !(host->mmc->ios.timing == MMC_TIMING_MMC_HS200) &&
	    !(host->mmc->ios.timing == MMC_TIMING_MMC_HS400 &&
	    !(priv->quirks && priv->quirks->hs400_4taps)))
		return false;

	if (priv->doing_tune)
		return false;

	/* Check SCC error */
	if (sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL) &
	    SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN)
		return renesas_sdhi_auto_correction(host);

	return renesas_sdhi_manual_correction(host, use_4tap);
}

static void renesas_sdhi_hw_reset(struct tmio_mmc_host *host)
{
	struct renesas_sdhi *priv;

	priv = host_to_priv(host);

	renesas_sdhi_reset_scc(host, priv);
	renesas_sdhi_reset_hs400_mode(host, priv);

	/* Disaled adjust HS400 mode */
	if (host->adjust_hs400_mode_disable)
		host->adjust_hs400_mode_disable(host->mmc);

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, CLK_CTL_SCLKEN |
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL,
		       ~SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN &
		       sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL));

	sd_scc_write32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL,
		       ~SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN &
		       sd_scc_read32(host, priv, SH_MOBILE_SDHI_SCC_RVSCNTL));
}

static int renesas_sdhi_wait_idle(struct tmio_mmc_host *host, u32 bit)
{
	int timeout = 1000;
	/* CBSY is set when busy, SCLKDIVEN is cleared when busy */
	u32 wait_state = (bit == TMIO_STAT_CMD_BUSY ? TMIO_STAT_CMD_BUSY : 0);

	while (--timeout && (sd_ctrl_read16_and_16_as_32(host, CTL_STATUS)
			      & bit) == wait_state)
		udelay(1);

	if (!timeout) {
		dev_warn(&host->pdev->dev, "timeout waiting for SD bus idle\n");
		return -EBUSY;
	}

	return 0;
}

static int renesas_sdhi_write16_hook(struct tmio_mmc_host *host, int addr)
{
	u32 bit = TMIO_STAT_SCLKDIVEN;

	switch (addr) {
	case CTL_SD_CMD:
	case CTL_STOP_INTERNAL_ACTION:
	case CTL_XFER_BLK_COUNT:
	case CTL_SD_XFER_LEN:
	case CTL_SD_MEM_CARD_OPT:
	case CTL_TRANSACTION_CTL:
	case CTL_DMA_ENABLE:
	case HOST_MODE:
		if (host->pdata->flags & TMIO_MMC_HAVE_CBSY)
			bit = TMIO_STAT_CMD_BUSY;
		/* fallthrough */
	case CTL_SD_CARD_CLK_CTL:
		return renesas_sdhi_wait_idle(host, bit);
	}

	return 0;
}

static int renesas_sdhi_multi_io_quirk(struct mmc_card *card,
				       unsigned int direction, int blk_size)
{
	/*
	 * In Renesas controllers, when performing a
	 * multiple block read of one or two blocks,
	 * depending on the timing with which the
	 * response register is read, the response
	 * value may not be read properly.
	 * Use single block read for this HW bug
	 */
	if ((direction == MMC_DATA_READ) &&
	    blk_size == 2)
		return 1;

	return blk_size;
}

static void renesas_sdhi_enable_dma(struct tmio_mmc_host *host, bool enable)
{
	/* Iff regs are 8 byte apart, sdbuf is 64 bit. Otherwise always 32. */
	int width = (host->bus_shift == 2) ? 64 : 32;

	sd_ctrl_write16(host, CTL_DMA_ENABLE, enable ? DMA_ENABLE_DMASDRW : 0);
	renesas_sdhi_sdbuf_width(host, enable ? width : 16);
}

static const struct renesas_sdhi_quirks sdhi_quirks_4tap = {
	.hs400_4taps = true,
};

static const struct renesas_sdhi_quirks sdhi_quirks_4tap_nohs400_bit17 = {
	.hs400_disabled = true,
	.hs400_4taps = true,
	.dtranend1_bit17 = true,
};

static const struct renesas_sdhi_quirks sdhi_quirks_4tap_nohs400 = {
	.hs400_disabled = true,
	.hs400_4taps = true,
};

static const struct renesas_sdhi_quirks sdhi_quirks_r8a7796_rev1 = {
	.hs400_4taps = true,
	.hs400_manual_calib = true,
	.hs400_offset = SH_MOBILE_SDHI_SCC_TMPPORT3_OFFSET_0,
	.hs400_calib_table = r8a7796_rev1_calib_table[0],
};

static const struct renesas_sdhi_quirks sdhi_quirks_r8a77965 = {
	.hs400_manual_calib = true,
	.hs400_offset = SH_MOBILE_SDHI_SCC_TMPPORT3_OFFSET_0,
	.hs400_calib_table = r8a77965_calib_table[0],
};

static const struct renesas_sdhi_quirks sdhi_quirks_r8a77990 = {
	.hs400_manual_calib = true,
	.hs400_offset = SH_MOBILE_SDHI_SCC_TMPPORT3_OFFSET_0,
	.hs400_calib_table = r8a77990_calib_table[0],
};

static const struct soc_device_attribute sdhi_quirks_match[]  = {
	{ .soc_id = "r8a774a1", .revision = "ES1.*",
	  .data = &sdhi_quirks_r8a7796_rev1, },
	{ .soc_id = "r8a774b1",
	  .data = &sdhi_quirks_r8a77965, },
	{ .soc_id = "r8a774c0",
	  .data = &sdhi_quirks_r8a77990, },
	{ .soc_id = "r8a7795", .revision = "ES1.*",
	  .data = &sdhi_quirks_4tap_nohs400_bit17, },
	{ .soc_id = "r8a7795", .revision = "ES2.0",
	  .data = &sdhi_quirks_4tap, },
	{ .soc_id = "r8a7796", .revision = "ES1.0",
	  .data = &sdhi_quirks_4tap_nohs400_bit17, },
	{ .soc_id = "r8a7796", .revision = "ES1.1",
	  .data = &sdhi_quirks_4tap_nohs400, },
	{ .soc_id = "r8a7796", .revision = "ES1.*",
	  .data = &sdhi_quirks_r8a7796_rev1, },
	{ .soc_id = "r8a77965",
	  .data = &sdhi_quirks_r8a77965, },
	{ .soc_id = "r8a77990",
	  .data = &sdhi_quirks_r8a77990, },
	{ /* sentinel */ }
};

int renesas_sdhi_probe(struct platform_device *pdev,
		       const struct tmio_mmc_dma_ops *dma_ops)
{
	struct tmio_mmc_data *mmd = pdev->dev.platform_data;
	const struct renesas_sdhi_quirks *quirks = NULL;
	const struct renesas_sdhi_of_data *of_data;
	const struct soc_device_attribute *attr;
	struct tmio_mmc_data *mmc_data;
	struct tmio_mmc_dma *dma_priv;
	struct tmio_mmc_host *host;
	struct renesas_sdhi *priv;
	struct resource *res;
	int irq, ret, i;
	u16 ver;
        u32 psc_pins = 0;
	int port_num_offset = 0;

	of_data = of_device_get_match_data(&pdev->dev);

	attr = soc_device_match(sdhi_quirks_match);
	if (attr)
		quirks = attr->data;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	priv = devm_kzalloc(&pdev->dev, sizeof(struct renesas_sdhi),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dma_set_mask_and_coherent(&pdev->dev,DMA_BIT_MASK(34));  //34bit extension for rzv2m
	priv->quirks = quirks;
	mmc_data = &priv->mmc_data;
	dma_priv = &priv->dma_priv;

	priv->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(priv->clk)) {
		ret = PTR_ERR(priv->clk);
		dev_err(&pdev->dev, "cannot get clock: %d\n", ret);
		return ret;
	}

	/*
	 * Some controllers provide a 2nd clock just to run the internal card
	 * detection logic. Unfortunately, the existing driver architecture does
	 * not support a separation of clocks for runtime PM usage. When
	 * native hotplug is used, the tmio driver assumes that the core
	 * must continue to run for card detect to stay active, so we cannot
	 * disable it.
	 * Additionally, it is prohibited to supply a clock to the core but not
	 * to the card detect circuit. That leaves us with if separate clocks
	 * are presented, we must treat them both as virtually 1 clock.
	 */
	priv->clk_cd = devm_clk_get(&pdev->dev, "cd");
	if (IS_ERR(priv->clk_cd))
		priv->clk_cd = NULL;

       /* Voltage switching type depends on the soc_id.
        * If the soc_device_match finds a soc_id, it run power slaveport control mode.
        * If you use pin function control unit, do not write a soc_id to whitelist.
        */
       if (strncmp(pdev->name, RZV2M_DEVICE_NAME_SD0, strlen(RZV2M_DEVICE_NAME_SD0)) == 0) {
               priv->psc_pins = PSC_SEL_SD0;
               priv->switching_volt_type = psc_mode;
       }
       else if (strncmp(pdev->name, RZV2M_DEVICE_NAME_SD1, strlen(RZV2M_DEVICE_NAME_SD1)) == 0) {
               priv->psc_pins = PSC_SEL_SD1;
               priv->switching_volt_type = psc_mode;
       }
       else{
               //The voltage of eMMC is fixed at 1.8V.
               priv->switching_volt_type = none;
	}

	host = tmio_mmc_host_alloc(pdev, mmc_data);
	if (IS_ERR(host))
		return PTR_ERR(host);

	priv->dtranend1_bit17 =
		(quirks && quirks->dtranend1_bit17) ? true : false;

	if (of_data) {
		mmc_data->flags |= of_data->tmio_flags;
		mmc_data->ocr_mask = of_data->tmio_ocr_mask;
		mmc_data->capabilities |= of_data->capabilities;
		mmc_data->capabilities2 |= of_data->capabilities2;
		mmc_data->dma_rx_offset = of_data->dma_rx_offset;
		mmc_data->max_blk_count = of_data->max_blk_count;
		mmc_data->max_segs = of_data->max_segs;
		dma_priv->dma_buswidth = of_data->dma_buswidth;
		host->bus_shift = of_data->bus_shift;
	}

	host->write16_hook	= renesas_sdhi_write16_hook;
	host->clk_enable	= renesas_sdhi_clk_enable;
	host->clk_update	= renesas_sdhi_clk_update;
	host->clk_disable	= renesas_sdhi_clk_disable;
	host->multi_io_quirk	= renesas_sdhi_multi_io_quirk;
	host->dma_ops		= dma_ops;
	host->needs_adjust_hs400 = false;

	/* For some SoC, we disable internal WP. GPIO may override this */
	if (mmc_can_gpio_ro(host->mmc))
		mmc_data->capabilities2 &= ~MMC_CAP2_NO_WRITE_PROTECT;

	/* SDR speeds are only available on Gen2+ */
	if (mmc_data->flags & TMIO_MMC_MIN_RCAR2) {
		/* card_busy caused issues on r8a73a4 (pre-Gen2) CD-less SDHI */
		host->ops.card_busy = renesas_sdhi_card_busy;
		host->ops.start_signal_voltage_switch =
			renesas_sdhi_start_signal_voltage_switch;

		/* SDR and HS200/400 registers requires HW reset */
		if (of_data && of_data->scc_offset) {
			priv->scc_ctl = host->ctl + of_data->scc_offset;
			host->mmc->caps |= MMC_CAP_HW_RESET;
			host->hw_reset = renesas_sdhi_hw_reset;
		}
	}

	/* Adjust HS400 mode */
	priv->adjust_hs400_offset = 0;
	priv->adjust_hs400_calib_table = NULL;

	if (host->mmc->caps2 & MMC_CAP2_HS400) {
		if (quirks && quirks->hs400_disabled) {
			host->mmc->caps2 &=
				~(MMC_CAP2_HS400 | MMC_CAP2_HS400_ES);
		} else if (quirks && quirks->hs400_manual_calib) {
			priv->adjust_hs400_offset =
				quirks->hs400_offset &
				SH_MOBILE_SDHI_SCC_TMPPORT3_OFFSET_MASK;
			priv->adjust_hs400_calib_table =
				quirks->hs400_calib_table + port_num_offset;
			host->adjust_hs400_mode_enable =
				renesas_sdhi_adjust_hs400_mode_enable;
			host->adjust_hs400_mode_disable =
				renesas_sdhi_adjust_hs400_mode_disable;
		}
	}

	/* Orginally registers were 16 bit apart, could be 32 or 64 nowadays */
	if (!host->bus_shift && resource_size(res) > 0x100) /* old way to determine the shift */
		host->bus_shift = 1;

	if (mmd)
		*mmc_data = *mmd;

	dma_priv->filter = shdma_chan_filter;
	dma_priv->enable = renesas_sdhi_enable_dma;

	mmc_data->alignment_shift = 1; /* 2-byte alignment */
	mmc_data->capabilities |= MMC_CAP_MMC_HIGHSPEED;

	/*
	 * All SDHI blocks support 2-byte and larger block sizes in 4-bit
	 * bus width mode.
	 */
	mmc_data->flags |= TMIO_MMC_BLKSZ_2BYTES;

	/*
	 * All SDHI blocks support SDIO IRQ signalling.
	 */
	mmc_data->flags |= TMIO_MMC_SDIO_IRQ;

	/* All SDHI have CMD12 control bit */
	mmc_data->flags |= TMIO_MMC_HAVE_CMD12_CTRL;

	/* All SDHI have SDIO status bits which must be 1 */
	mmc_data->flags |= TMIO_MMC_SDIO_STATUS_SETBITS;

	ret = renesas_sdhi_clk_enable(host);
	if (ret)
		goto efree;

	ver = sd_ctrl_read16(host, CTL_VERSION);
	/* GEN2_SDR104 is first known SDHI to use 32bit block count */
	if (ver < SDHI_VER_GEN2_SDR104 && mmc_data->max_blk_count > U16_MAX)
		mmc_data->max_blk_count = U16_MAX;

	ret = tmio_mmc_host_probe(host);
	if (ret < 0)
		goto edisclk;

	/* One Gen2 SDHI incarnation does NOT have a CBSY bit */
	if (ver == SDHI_VER_GEN2_SDR50)
		mmc_data->flags &= ~TMIO_MMC_HAVE_CBSY;

	/* Enable tuning iff we have an SCC and a supported mode */
	if (of_data && of_data->scc_offset &&
	    (host->mmc->caps & MMC_CAP_UHS_SDR104 ||
	     host->mmc->caps2 & (MMC_CAP2_HS200_1_8V_SDR |
				 MMC_CAP2_HS400_1_8V))) {
		const struct renesas_sdhi_scc *taps = of_data->taps;
		bool use_4tap = priv->quirks && priv->quirks->hs400_4taps;
		bool hit = false;

		for (i = 0; i < of_data->taps_num; i++) {
			if (taps[i].clk_rate == 0 ||
			    taps[i].clk_rate == host->mmc->f_max) {
				priv->scc_tappos = taps->tap;
				priv->scc_tappos_hs400 = use_4tap ?
							 taps->tap_hs400_4tap :
							 taps->tap;
				hit = true;
				break;
			}
		}

		if (!hit)
			dev_warn(&host->pdev->dev, "Unknown clock rate for tuning\n");

		host->init_tuning = renesas_sdhi_init_tuning;
		host->prepare_tuning = renesas_sdhi_prepare_tuning;
		host->select_tuning = renesas_sdhi_select_tuning;
		host->check_scc_error = renesas_sdhi_check_scc_error;
		host->prepare_hs400_tuning =
			renesas_sdhi_prepare_hs400_tuning;
		host->hs400_downgrade = renesas_sdhi_disable_scc;
		host->hs400_complete = renesas_sdhi_hs400_complete;
	}

	i = 0;
	while (1) {
		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			break;
		i++;
		ret = devm_request_irq(&pdev->dev, irq, tmio_mmc_irq, 0,
				       dev_name(&pdev->dev), host);
		if (ret)
			goto eirq;
	}

	/* There must be at least one IRQ source */
	if (!i) {
		ret = irq;
		goto eirq;
	}

	dev_info(&pdev->dev, "%s base at 0x%08lx max clock rate %u MHz\n",
		 mmc_hostname(host->mmc), (unsigned long)
		 (platform_get_resource(pdev, IORESOURCE_MEM, 0)->start),
		 host->mmc->f_max / 1000000);

	return ret;

eirq:
	tmio_mmc_host_remove(host);
edisclk:
	renesas_sdhi_clk_disable(host);
efree:
	tmio_mmc_host_free(host);

	return ret;
}
EXPORT_SYMBOL_GPL(renesas_sdhi_probe);

int renesas_sdhi_remove(struct platform_device *pdev)
{
	struct tmio_mmc_host *host = platform_get_drvdata(pdev);

	tmio_mmc_host_remove(host);
	renesas_sdhi_clk_disable(host);
	tmio_mmc_host_free(host);

	return 0;
}
EXPORT_SYMBOL_GPL(renesas_sdhi_remove);

MODULE_LICENSE("GPL v2");
