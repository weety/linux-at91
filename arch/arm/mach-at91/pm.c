/*
 * arch/arm/mach-at91/pm.c
 * AT91 Power Management
 *
 * Copyright (C) 2005 David Brownell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/gpio.h>
#include <linux/suspend.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/genalloc.h>
#include <linux/interrupt.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/clk/at91_pmc.h>

#include <asm/irq.h>
#include <linux/atomic.h>
#include <asm/mach/time.h>
#include <asm/mach/irq.h>
#include <asm/fncpy.h>
#include <asm/cacheflush.h>

#include <mach/cpu.h>
#include <mach/hardware.h>

#include "generic.h"
#include "pm.h"

static struct {
	unsigned long uhp_udp_mask;
	int memctrl;
	unsigned int ddrc_pid;
	bool is_sama5d4;
} at91_pm_data;

void __iomem *at91_ramc_base[2];

static int at91_pm_valid_state(suspend_state_t state)
{
	switch (state) {
		case PM_SUSPEND_ON:
		case PM_SUSPEND_STANDBY:
		case PM_SUSPEND_MEM:
			return 1;

		default:
			return 0;
	}
}


static suspend_state_t target_state;

/*
 * Called after processes are frozen, but before we shutdown devices.
 */
static int at91_pm_begin(suspend_state_t state)
{
	target_state = state;
	return 0;
}

/*
 * Verify that all the clocks are correct before entering
 * slow-clock mode.
 */
static int at91_pm_verify_clocks(void)
{
	unsigned long scsr;
	int i;

	scsr = at91_pmc_read(AT91_PMC_SCSR);

	/* USB must not be using PLLB */
	if ((scsr & at91_pm_data.uhp_udp_mask) != 0) {
		pr_err("AT91: PM - Suspend-to-RAM with USB still active\n");
		return 0;
	}

	/* PCK0..PCK3 must be disabled, or configured to use clk32k */
	for (i = 0; i < 4; i++) {
		u32 css;

		if ((scsr & (AT91_PMC_PCK0 << i)) == 0)
			continue;

		css = at91_pmc_read(AT91_PMC_PCKR(i)) & AT91_PMC_CSS;
		if (css != AT91_PMC_CSS_SLOW) {
			pr_err("AT91: PM - Suspend-to-RAM with PCK%d src %d\n", i, css);
			return 0;
		}
	}

	/* Drivers should have previously suspended USB PLL */
	if (at91_pmc_read(AT91_CKGR_UCKR) & AT91_PMC_UPLLEN) {
		pr_err("AT91: PM - Suspend-to-RAM with USB PLL running\n");
		return 0;
	}

	/* Drivers should have previously suspended PLL B */
	if (at91_pmc_read(AT91_PMC_SR) & AT91_PMC_LOCKB) {
		pr_err("AT91: PM - Suspend-to-RAM with PLL B running\n");
		return 0;
	}

	return 1;
}

/*
 * Call this from platform driver suspend() to see how deeply to suspend.
 * For example, some controllers (like OHCI) need one of the PLL clocks
 * in order to act as a wakeup source, and those are not available when
 * going into slow clock mode.
 *
 * REVISIT: generalize as clk_will_be_available(clk)?  Other platforms have
 * the very same problem (but not using at91 main_clk), and it'd be better
 * to add one generic API rather than lots of platform-specific ones.
 */
int at91_suspend_entering_slow_clock(void)
{
	return (target_state == PM_SUSPEND_MEM);
}
EXPORT_SYMBOL(at91_suspend_entering_slow_clock);

static void (*at91_suspend_sram_fn)(void __iomem *pmc, void __iomem *ramc0,
			  void __iomem *ramc1, int memctrl);

extern void at91_pm_suspend_in_sram(void __iomem *pmc, void __iomem *ramc0,
			    void __iomem *ramc1, int memctrl);
extern u32 at91_pm_suspend_in_sram_sz;

static void at91_pm_suspend(suspend_state_t state)
{
	unsigned int pm_data = at91_pm_data.memctrl;

	pm_data |= (state == PM_SUSPEND_MEM) ?
				AT91_PM_MODE(AT91_PM_SLOW_CLOCK) : 0;

	pm_data |= AT91_PM_DDRC_PID(at91_pm_data.ddrc_pid);
	pm_data |= at91_pm_data.is_sama5d4 ?
			AT91_PM_IS_SAMA5D4(AT91_PM_SAMA5D4_BIT) : 0;

	flush_cache_all();
	outer_disable();

	at91_suspend_sram_fn(at91_pmc_base, at91_ramc_base[0],
				at91_ramc_base[1], pm_data);

	outer_resume();
}

static int at91_pm_enter(suspend_state_t state)
{
	at91_pinctrl_gpio_suspend();

	switch (state) {
	/*
	 * Suspend-to-RAM is like STANDBY plus slow clock mode, so
	 * drivers must suspend more deeply, the master clock switches
	 * to the clk32k and turns off the main oscillator
	 */
	case PM_SUSPEND_MEM:
		/*
		 * Ensure that clocks are in a valid state.
		 */
		if (!at91_pm_verify_clocks())
			goto error;

		at91_pm_suspend(state);

		break;

	/*
	 * STANDBY mode has *all* drivers suspended; ignores irqs not
	 * marked as 'wakeup' event sources; and reduces DRAM power.
	 * But otherwise it's identical to PM_SUSPEND_ON: cpu idle, and
	 * nothing fancy done with main or cpu clocks.
	 */
	case PM_SUSPEND_STANDBY:
		at91_pm_suspend(state);
		break;

	case PM_SUSPEND_ON:
		cpu_do_idle();
		break;

	default:
		pr_debug("AT91: PM - bogus suspend state %d\n", state);
		goto error;
	}

error:
	target_state = PM_SUSPEND_ON;

	at91_pinctrl_gpio_resume();
	return 0;
}

/*
 * Called right prior to thawing processes.
 */
static void at91_pm_end(void)
{
	target_state = PM_SUSPEND_ON;
}


static const struct platform_suspend_ops at91_pm_ops = {
	.valid	= at91_pm_valid_state,
	.begin	= at91_pm_begin,
	.enter	= at91_pm_enter,
	.end	= at91_pm_end,
};

static struct platform_device at91_cpuidle_device = {
	.name = "cpuidle-at91",
};

static void at91_pm_set_standby(void (*at91_standby)(void))
{
	if (at91_standby)
		at91_cpuidle_device.dev.platform_data = at91_standby;
}

static struct of_device_id ramc_ids[] = {
	{ .compatible = "atmel,at91rm9200-sdramc", .data = at91rm9200_standby },
	{ .compatible = "atmel,at91sam9260-sdramc", .data = at91sam9_sdram_standby },
	{ .compatible = "atmel,at91sam9g45-ddramc", .data = at91_ddr_standby },
	{ .compatible = "atmel,sama5d3-ddramc", .data = at91_ddr_standby },
	{ /*sentinel*/ }
};

static void at91_dt_ramc(void)
{
	struct device_node *np;
	const struct of_device_id *of_id;
	int idx = 0;
	const void *standby = NULL;

	for_each_matching_node_and_match(np, ramc_ids, &of_id) {
		at91_ramc_base[idx] = of_iomap(np, 0);
		if (!at91_ramc_base[idx])
			panic(pr_fmt("unable to map ramc[%d] cpu registers\n"), idx);

		if (!standby)
			standby = of_id->data;

		idx++;
	}

	if (!idx)
		panic(pr_fmt("unable to find compatible ram controller node in dtb\n"));

	if (!standby) {
		pr_warn("ramc no standby function available\n");
		return;
	}

	at91_pm_set_standby(standby);
}

static void __init at91_pm_sram_init(void)
{
	struct gen_pool *sram_pool;
	phys_addr_t sram_pbase;
	unsigned long sram_base;
	struct device_node *node;
	struct platform_device *pdev = NULL;

	for_each_compatible_node(node, NULL, "mmio-sram") {
		pdev = of_find_device_by_node(node);
		if (pdev) {
			of_node_put(node);
			break;
		}
	}

	if (!pdev) {
		pr_warn("%s: failed to find sram device!\n", __func__);
		return;
	}

	sram_pool = dev_get_gen_pool(&pdev->dev);
	if (!sram_pool) {
		pr_warn("%s: sram pool unavailable!\n", __func__);
		return;
	}

	sram_base = gen_pool_alloc(sram_pool, at91_pm_suspend_in_sram_sz);
	if (!sram_base) {
		pr_warn("%s: unable to alloc ocram!\n", __func__);
		return;
	}

	sram_pbase = gen_pool_virt_to_phys(sram_pool, sram_base);
	at91_suspend_sram_fn = __arm_ioremap_exec(sram_pbase,
					at91_pm_suspend_in_sram_sz, false);
	if (!at91_suspend_sram_fn) {
		pr_warn("SRAM: Could not map\n");
		return;
	}

	/* Copy the pm suspend handler to SRAM */
	at91_suspend_sram_fn = fncpy(at91_suspend_sram_fn,
			&at91_pm_suspend_in_sram, at91_pm_suspend_in_sram_sz);
}

static void __init at91_pm_init(void)
{
	at91_pm_sram_init();

	if (at91_cpuidle_device.dev.platform_data)
		platform_device_register(&at91_cpuidle_device);

	if (at91_suspend_sram_fn)
		suspend_set_ops(&at91_pm_ops);
	else
		pr_info("AT91: PM not supported, due to no SRAM allocated\n");
}

void __init at91rm9200_pm_init(void)
{
	at91_dt_ramc();

	/*
	 * AT91RM9200 SDRAM low-power mode cannot be used with self-refresh.
	 */
	at91_ramc_write(0, AT91RM9200_SDRAMC_LPR, 0);

	at91_pm_data.uhp_udp_mask = AT91RM9200_PMC_UHP | AT91RM9200_PMC_UDP;
	at91_pm_data.memctrl = AT91_MEMCTRL_MC;

	at91_pm_init();
}

void __init at91sam9260_pm_init(void)
{
	at91_dt_ramc();
	at91_pm_data.memctrl = AT91_MEMCTRL_SDRAMC;
	at91_pm_data.uhp_udp_mask = AT91SAM926x_PMC_UHP | AT91SAM926x_PMC_UDP;
	return at91_pm_init();
}

void __init at91sam9g45_pm_init(void)
{
	at91_dt_ramc();
	at91_pm_data.uhp_udp_mask = AT91SAM926x_PMC_UHP;
	at91_pm_data.memctrl = AT91_MEMCTRL_DDRSDR;
	return at91_pm_init();
}

void __init at91sam9x5_pm_init(void)
{
	at91_dt_ramc();
	at91_pm_data.uhp_udp_mask = AT91SAM926x_PMC_UHP | AT91SAM926x_PMC_UDP;
	at91_pm_data.memctrl = AT91_MEMCTRL_DDRSDR;
	return at91_pm_init();
}

void __init sam5d3_pm_init(void)
{
	at91_dt_ramc();
	at91_pm_data.uhp_udp_mask = AT91SAM926x_PMC_UHP | AT91SAM926x_PMC_UDP;
	at91_pm_data.memctrl = AT91_MEMCTRL_DDRSDR;
	at91_pm_data.ddrc_pid = SAMA5D3_ID_MPDDRC;
	return at91_pm_init();
}

void __init sam5d4_pm_init(void)
{
	at91_dt_ramc();
	at91_pm_data.uhp_udp_mask = AT91SAM926x_PMC_UHP | AT91SAM926x_PMC_UDP;
	at91_pm_data.memctrl = AT91_MEMCTRL_DDRSDR;
	at91_pm_data.ddrc_pid = SAMA5D4_ID_MPDDRC;
	at91_pm_data.is_sama5d4 = true;
	return at91_pm_init();
}
