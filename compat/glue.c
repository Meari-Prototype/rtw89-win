// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win compat: entry points for the KMDF driver (rtw/glue.h). Builds the struct pci_dev of the bound device
 * and runs the 8852BE pci_driver (rtw8852be.c, module_pci_driver) probe/remove.
 */
#include <rtw/linux.h>
#include <rtw/glue.h>

void rtw_device_init (void);
void rtw_irq_set_mmio (void __iomem *mmio);
int rtw_mac_init (void);
void rtw_mac_exit (void);
void rtw_mac_log_regulatory (void);

static struct pci_dev pdev;
static struct pci_bus pbus;
static bool probed;

int rtw_linux_init (void)
{
	int ret;

	rtw_device_init();
	ret = rtw_workqueue_init();
	if (ret)
		return ret;
	rtw_rcu_init();
	ret = rtw_mac_init();
	if (ret) {
		rtw_rcu_exit();
		rtw_workqueue_exit();
	}
	return ret;
}

void rtw_linux_exit (void)
{
	rtw_linux_remove();
	rtw_mac_exit();
	rtw_rcu_exit();
	rtw_workqueue_exit();
}

int rtw_linux_probe (const struct rtw_pci_info *info)
{
	const struct pci_device_id *id;
	int ret;

	if (probed)
		return -EBUSY;
	for (id = rtwk_pci_driver->id_table; id->vendor; id++)
		if (id->vendor == info->vendor && id->device == info->device)
			break;
	if (!id->vendor) {
		pr_err("rtw89: no driver for %04x:%04x", info->vendor, info->device);
		return -ENODEV;
	}

	memset(&pdev, 0, sizeof(pdev));
	memset(&pbus, 0, sizeof(pbus));
	INIT_LIST_HEAD(&pdev.dev.devres);
	pdev.dev.init_name = rtwk_pci_driver->name;
	pdev.dev.os_dev = info->os_dev;
	pdev.dev.driver = &rtwk_pci_driver->driver;
	pdev.dev.dma_mask = DMA_BIT_MASK(32);
	pdev.bus = &pbus;               /* bus->self = NULL: the upstream bridge is not a DAC-capable Intel/ASMedia one */
	pdev.vendor = info->vendor;
	pdev.device = info->device;
	pdev.subsystem_vendor = info->subsystem_vendor;
	pdev.subsystem_device = info->subsystem_device;
	pdev.revision = info->revision;
	pdev.irq = info->irq;
	pdev.bar_va[2] = info->bar_va;
	pdev.bar_len[2] = info->bar_len;
	rtw_irq_set_mmio(info->bar_va); /* the ISR's INTx check (device.c) */

	pr_info("rtw89: probing %s for %04x:%04x", rtwk_pci_driver->name, info->vendor, info->device);
	ret = rtwk_pci_driver->probe(&pdev, id);
	if (ret) {
		pr_err("rtw89: probe failed: %d", ret);
		devres_release_all(&pdev.dev);
		rtw_skb_reclaim();
		rtw_irq_set_mmio(NULL);
		return ret;
	}
	probed = true;
	pr_info("rtw89: probe done");
	rtw_mac_log_regulatory();
	return 0;
}

void rtw_linux_remove (void)
{
	if (!probed)
		return;
	/* mac80211 closes the interfaces before the driver goes; rtw89's remove frees the interrupt first */
	rtw_linux_stop();
	pr_info("rtw89: removing");
	rtwk_pci_driver->remove(&pdev);
	devres_release_all(&pdev.dev);
	rtw_skb_reclaim();
	rtw_irq_set_mmio(NULL);
	probed = false;
	pr_info("rtw89: removed");
}

int rtw_linux_probed (void)
{
	return probed;
}
