/*
 * Coherency fabric (Aurora) support for Armada 370, 375, 38x and XP
 * platforms.
 *
 * Copyright (C) 2012 Marvell
 *
 * Yehuda Yitschak <yehuday@marvell.com>
 * Gregory Clement <gregory.clement@free-electrons.com>
 * Thomas Petazzoni <thomas.petazzoni@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 *
 * The Armada 370, 375, 38x and XP SOCs have a coherency fabric which is
 * responsible for ensuring hardware coherency between all CPUs and between
 * CPUs and I/O masters. This file initializes the coherency fabric and
 * supplies basic routines for configuring and controlling hardware coherency
 */

#define pr_fmt(fmt) "mvebu-coherency: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/smp.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/mbus.h>
#include <linux/pci.h>
#include <asm/smp_plat.h>
#include <asm/cacheflush.h>
#include <asm/mach/map.h>
#include <asm/dma-mapping.h>
#include "coherency.h"
#include "mvebu-soc-id.h"

unsigned long coherency_phys_base;
void __iomem *coherency_base;
static void __iomem *coherency_cpu_base;

/* Coherency fabric registers */
#define COHERENCY_FABRIC_CFG_OFFSET		   0x4

#define IO_SYNC_BARRIER_CTL_OFFSET		   0x0

enum {
	COHERENCY_FABRIC_TYPE_NONE,
	COHERENCY_FABRIC_TYPE_ARMADA_370_XP,
	COHERENCY_FABRIC_TYPE_ARMADA_375,
	COHERENCY_FABRIC_TYPE_ARMADA_380,
};

static struct of_device_id of_coherency_table[] = {
	{.compatible = "marvell,coherency-fabric",
	 .data = (void *) COHERENCY_FABRIC_TYPE_ARMADA_370_XP },
	{.compatible = "marvell,armada-375-coherency-fabric",
	 .data = (void *) COHERENCY_FABRIC_TYPE_ARMADA_375 },
	{.compatible = "marvell,armada-380-coherency-fabric",
	 .data = (void *) COHERENCY_FABRIC_TYPE_ARMADA_380 },
	{ /* end of list */ },
};

/* Functions defined in coherency_ll.S */
int ll_enable_coherency(void);
void ll_add_cpu_to_smp_group(void);

int set_cpu_coherent(void)
{
	if (!coherency_base) {
		pr_warn("Can't make current CPU cache coherent.\n");
		pr_warn("Coherency fabric is not initialized\n");
		return 1;
	}

	ll_add_cpu_to_smp_group();
	return ll_enable_coherency();
}

static int mvebu_hwcc_notifier(struct notifier_block *nb,
			       unsigned long event, void *__dev)
{
	struct device *dev = __dev;

	if (event != BUS_NOTIFY_ADD_DEVICE)
		return NOTIFY_DONE;
	set_dma_ops(dev, &arm_coherent_dma_ops);

	return NOTIFY_OK;
}

static struct notifier_block mvebu_hwcc_nb = {
	.notifier_call = mvebu_hwcc_notifier,
};

static struct notifier_block mvebu_hwcc_pci_nb = {
	.notifier_call = mvebu_hwcc_notifier,
};

static void __init armada_370_coherency_init(struct device_node *np)
{
	struct resource res;

	of_address_to_resource(np, 0, &res);
	coherency_phys_base = res.start;
	/*
	 * Ensure secondary CPUs will see the updated value,
	 * which they read before they join the coherency
	 * fabric, and therefore before they are coherent with
	 * the boot CPU cache.
	 */
	sync_cache_w(&coherency_phys_base);
	coherency_base = of_iomap(np, 0);
	coherency_cpu_base = of_iomap(np, 1);
	set_cpu_coherent();
}

/*
 * This ioremap hook is used on Armada 375/38x to ensure that PCIe
 * memory areas are mapped as MT_UNCACHED instead of MT_DEVICE. This
 * is needed as a workaround for a deadlock issue between the PCIe
 * interface and the cache controller.
 */
static void __iomem *
armada_pcie_wa_ioremap_caller(phys_addr_t phys_addr, size_t size,
			      unsigned int mtype, void *caller)
{
	struct resource pcie_mem;

	mvebu_mbus_get_pcie_mem_aperture(&pcie_mem);

	if (pcie_mem.start <= phys_addr && (phys_addr + size) <= pcie_mem.end)
		mtype = MT_UNCACHED;

	return __arm_ioremap_caller(phys_addr, size, mtype, caller);
}

static void __init armada_375_380_coherency_init(struct device_node *np)
{
	struct device_node *cache_dn;

	coherency_cpu_base = of_iomap(np, 0);
	arch_ioremap_caller = armada_pcie_wa_ioremap_caller;

	/*
	 * Add the PL310 property "arm,io-coherent". This makes sure the
	 * outer sync operation is not used, which allows to
	 * workaround the system erratum that causes deadlocks when
	 * doing PCIe in an SMP situation on Armada 375 and Armada
	 * 38x.
	 */
	for_each_compatible_node(cache_dn, NULL, "arm,pl310-cache") {
		struct property *p;

		p = kzalloc(sizeof(*p), GFP_KERNEL);
		p->name = kstrdup("arm,io-coherent", GFP_KERNEL);
		of_add_property(cache_dn, p);
	}
}

static int coherency_type(void)
{
	struct device_node *np;
	const struct of_device_id *match;

	np = of_find_matching_node_and_match(NULL, of_coherency_table, &match);
	if (np) {
		int type = (int) match->data;

		/* Armada 370/XP coherency works in both UP and SMP */
		if (type == COHERENCY_FABRIC_TYPE_ARMADA_370_XP)
			return type;

		/* Armada 375 coherency works only on SMP */
		else if (type == COHERENCY_FABRIC_TYPE_ARMADA_375 && is_smp())
			return type;

		/* Armada 380 coherency works only on SMP */
		else if (type == COHERENCY_FABRIC_TYPE_ARMADA_380 && is_smp())
			return type;
	}

	return COHERENCY_FABRIC_TYPE_NONE;
}

int coherency_available(void)
{
	return coherency_type() != COHERENCY_FABRIC_TYPE_NONE;
}

int __init coherency_init(void)
{
	int type = coherency_type();
	struct device_node *np;

	np = of_find_matching_node(NULL, of_coherency_table);

	if (type == COHERENCY_FABRIC_TYPE_ARMADA_370_XP)
		armada_370_coherency_init(np);
	else if (type == COHERENCY_FABRIC_TYPE_ARMADA_375 ||
		 type == COHERENCY_FABRIC_TYPE_ARMADA_380)
		armada_375_380_coherency_init(np);

	of_node_put(np);

	return 0;
}

static int __init coherency_late_init(void)
{
	if (coherency_available())
		bus_register_notifier(&platform_bus_type,
				      &mvebu_hwcc_nb);
	return 0;
}

postcore_initcall(coherency_late_init);

#if IS_ENABLED(CONFIG_PCI)
static int __init coherency_pci_init(void)
{
	if (coherency_available())
		bus_register_notifier(&pci_bus_type,
				       &mvebu_hwcc_pci_nb);
	return 0;
}

arch_initcall(coherency_pci_init);
#endif
