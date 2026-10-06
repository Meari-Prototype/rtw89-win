// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win compat: devm memory, PCI config space, the one interrupt line, DMA mapping, firmware loading.
 *
 * DMA: coherent memory and skb data come from WDF common buffers (rtwk_dma_*), which are either cache coherent or
 * mapped uncached, so no cache maintenance is needed. dma_map_single() of memory outside them (rare) goes through
 * a bounce buffer from the DMA heap.
 */
#include <rtw/linux.h>

/* ---- devm ---- */
struct devres {
	struct list_head node;
	u64 data[];
};

void *devm_kmalloc (struct device *d, size_t n, gfp_t f)
{
	struct devres *r = rtwk_alloc(sizeof(*r) + n, (f & __GFP_ZERO) != 0);

	if (!r)
		return NULL;
	list_add_tail(&r->node, &d->devres);
	return r->data;
}

void *devm_kmemdup (struct device *d, const void *s, size_t n, gfp_t f)
{
	void *p = devm_kmalloc(d, n, f);

	if (p)
		memcpy(p, s, n);
	return p;
}

void devm_kfree (struct device *d, const void *p)
{
	struct devres *r;

	(void)d;
	if (!p)
		return;
	r = container_of((void *)p, struct devres, data);
	list_del(&r->node);
	rtwk_free(r);
}

void devres_release_all (struct device *d)
{
	struct devres *r, *n;

	list_for_each_entry_safe(r, n, &d->devres, node) {
		list_del(&r->node);
		rtwk_free(r);
	}
}

/* ---- PCI config space ---- */
static int cfg_read (const struct pci_dev *p, int where, void *v, u32 len)
{
	return rtwk_pci_cfg_read(p->dev.os_dev, where, v, len) == len ? 0 : -EIO;
}

static int cfg_write (const struct pci_dev *p, int where, const void *v, u32 len)
{
	return rtwk_pci_cfg_write(p->dev.os_dev, where, v, len) == len ? 0 : -EIO;
}

int pci_read_config_byte (const struct pci_dev *p, int where, u8 *val) { return cfg_read(p, where, val, 1); }
int pci_read_config_word (const struct pci_dev *p, int where, u16 *val) { return cfg_read(p, where, val, 2); }
int pci_read_config_dword (const struct pci_dev *p, int where, u32 *val) { return cfg_read(p, where, val, 4); }
int pci_write_config_byte (const struct pci_dev *p, int where, u8 val) { return cfg_write(p, where, &val, 1); }
int pci_write_config_word (const struct pci_dev *p, int where, u16 val) { return cfg_write(p, where, &val, 2); }
int pci_write_config_dword (const struct pci_dev *p, int where, u32 val) { return cfg_write(p, where, &val, 4); }

int pci_find_capability (struct pci_dev *p, int cap)
{
	u8 pos = 0;
	u8 id;

	pci_read_config_byte(p, 0x34, &pos);
	for (int guard = 0; pos >= 0x40 && guard < 48; guard++) {
		pos &= ~3;
		if (pci_read_config_byte(p, pos, &id) || id == 0xff)
			break;
		if (id == cap)
			return pos;
		pci_read_config_byte(p, pos + 1, &pos);
	}
	return 0;
}

u16 pci_find_ext_capability (struct pci_dev *p, int cap)
{
	u32 hdr;
	int pos = 0x100;

	for (int guard = 0; pos >= 0x100 && pos < 0x1000 && guard < 64; guard++) {
		if (pci_read_config_dword(p, pos, &hdr) || hdr == 0 || hdr == 0xffffffff)
			return 0;
		if ((hdr & 0xffff) == (u32)cap)
			return (u16)pos;
		pos = hdr >> 20;
	}
	return 0;
}

static int pcie_cap (struct pci_dev *p)
{
	if (!p->pcie_cap)
		p->pcie_cap = (u8)pci_find_capability(p, PCI_CAP_ID_EXP);
	return p->pcie_cap;
}

int pcie_capability_read_word (struct pci_dev *p, int pos, u16 *val)
{
	*val = 0;
	return pcie_cap(p) ? pci_read_config_word(p, pcie_cap(p) + pos, val) : -EINVAL;
}

int pcie_capability_read_dword (struct pci_dev *p, int pos, u32 *val)
{
	*val = 0;
	return pcie_cap(p) ? pci_read_config_dword(p, pcie_cap(p) + pos, val) : -EINVAL;
}

int pcie_capability_write_word (struct pci_dev *p, int pos, u16 val)
{
	return pcie_cap(p) ? pci_write_config_word(p, pcie_cap(p) + pos, val) : -EINVAL;
}

int pcie_capability_clear_and_set_word (struct pci_dev *p, int pos, u16 clear, u16 set)
{
	u16 v;
	int ret = pcie_capability_read_word(p, pos, &v);

	if (ret)
		return ret;
	return pcie_capability_write_word(p, pos, (u16)((v & ~clear) | set));
}

/* ---- interrupt: one INTx line; MSI is not available on the RK3588 PCIe controllers under Windows ---- */

/* The 8852BE's (AX) host interrupt registers: the PCIe-side pairs decide the INTx line (HISR00 has an indicator bit
   for HISR0, the MAC side's halt-C2H status); rtw89's disable_intr (rtw89_pci_disable_intr) clears all three masks */
#define INTX_HIMR0              0x01A0
#define INTX_HISR0              0x01A4
#define INTX_PCIE_HIMR00        0x10B0
#define INTX_PCIE_HISR00        0x10B4
#define INTX_PCIE_HIMR10        0x13B0
#define INTX_PCIE_HISR10        0x13B4

static struct {
	irq_handler_t handler;
	irq_handler_t thread_fn;
	void *dev_id;
	unsigned int irq;
	struct work_struct thread_work;
	struct workqueue_struct *wq;
	u8 __iomem *mmio;               /* BAR2, for the INTx check below */
	bool wake;                      /* the ISR wants the thread (IRQ_WAKE_THREAD) */
	bool note;                      /* the ISR masked the chip while rtw89 was stopped: log it (DPC) */
	u32 masked, not_ours;           /* counts */
	u32 snap[6];                    /* registers at the first masking since the last note (see intx_stopped) */
} irq;

void rtw_irq_set_mmio (void __iomem *mmio)
{
	irq.mmio = mmio;
}

int pci_alloc_irq_vectors (struct pci_dev *p, unsigned int min, unsigned int max, unsigned int flags)
{
	(void)p;
	(void)max;
	if (!(flags & PCI_IRQ_INTX) || min > 1)
		return -ENOSPC;
	return 1;
}

static void irq_thread (struct work_struct *w)
{
	(void)w;
	if (irq.thread_fn)
		irq.thread_fn((int)irq.irq, irq.dev_id);
}

int request_threaded_irq (unsigned int nr, irq_handler_t handler, irq_handler_t thread_fn, unsigned long flags,
			  const char *name, void *dev_id)
{
	(void)flags;
	if (irq.handler)
		return -EBUSY;
	irq.wq = alloc_workqueue("%s-irq", WQ_HIGHPRI, 1, name);
	if (!irq.wq)
		return -ENOMEM;
	INIT_WORK(&irq.thread_work, irq_thread);
	irq.irq = nr;
	irq.dev_id = dev_id;
	irq.thread_fn = thread_fn;
	smp_wmb();
	WRITE_ONCE(irq.handler, handler);
	return 0;
}

void synchronize_irq (unsigned int nr)
{
	(void)nr;
	rtwk_irq_synchronize();
	if (irq.wq)
		flush_work(&irq.thread_work);
}

void free_irq (unsigned int nr, void *dev_id)
{
	(void)dev_id;
	WRITE_ONCE(irq.handler, NULL);
	synchronize_irq(nr);
	cancel_work_sync(&irq.thread_work);
	destroy_workqueue(irq.wq);
	irq.wq = NULL;
	irq.thread_fn = NULL;
}

void disable_irq (unsigned int nr)
{
	synchronize_irq(nr);
}

void enable_irq (unsigned int nr)
{
	(void)nr;
}

/*
 * rtw89's handler claimed an interrupt without waking its thread: rtw89 is stopped (rtwpci->running false, e.g.
 * between the reset and the restart of a SER level 2) and leaves the chip alone. That is right for the MSI rtw89 is
 * written for (a message in flight, nothing to clear), not for a level-triggered INTx: whatever the chip still
 * asserts raises the interrupt again at once, forever (an interrupt storm on CPU0 and a DPC_WATCHDOG_VIOLATION,
 * observed at every SER: rtw89_core_start's MAC init has the masks back on before rtw89_hci_start sets running). So look at the chip: nothing pending under its PCIe masks - not ours (the line is shared); something
 * pending - mask the chip's host interrupts, the state rtw89's stop left them in and its start sets up again. DIRQL.
 * Returns 0 (not ours) or 1 (handled), or 2 when the DPC should log what was masked.
 */
static int intx_stopped (void)
{
	u8 __iomem *m = irq.mmio;
	u32 s00, m00, s10, m10;

	if (!m)
		return 1;
	s00 = readl(m + INTX_PCIE_HISR00);
	m00 = readl(m + INTX_PCIE_HIMR00);
	s10 = readl(m + INTX_PCIE_HISR10);
	m10 = readl(m + INTX_PCIE_HIMR10);
	if (!(s00 & m00) && !(s10 & m10)) {
		irq.not_ours++;
		return 0;
	}
	if (!READ_ONCE(irq.note)) {
		irq.snap[0] = s00;
		irq.snap[1] = m00;
		irq.snap[2] = s10;
		irq.snap[3] = m10;
		irq.snap[4] = readl(m + INTX_HISR0);
		irq.snap[5] = readl(m + INTX_HIMR0);
	}
	writel(0, m + INTX_HIMR0);
	writel(0, m + INTX_PCIE_HIMR00);
	writel(0, m + INTX_PCIE_HIMR10);
	irq.masked++;
	WRITE_ONCE(irq.note, true);
	return 2;
}

int rtw_linux_isr (void)
{
	irq_handler_t h = READ_ONCE(irq.handler);
	irqreturn_t ret;

	if (!h)
		return 0;
	ret = h((int)irq.irq, irq.dev_id);
	if (ret == IRQ_HANDLED) {
		int r = intx_stopped();

		if (r != 2)
			return r;
		/* rtw89_pci_ops_start may have set running and enabled the chip (under rtw89's irq_lock, on another
		   CPU) since the handler looked: ask it again, so the masking just done never outlives a start - a
		   running rtw89 then masks the chip itself and its thread (NAPI) turns the interrupts on again */
		ret = h((int)irq.irq, irq.dev_id);
		if (ret == IRQ_WAKE_THREAD)
			WRITE_ONCE(irq.wake, true);
		return 2;
	}
	if (ret == IRQ_WAKE_THREAD) {
		WRITE_ONCE(irq.wake, true);
		return 2;
	}
	return 0;
}

void rtw_linux_irq_dpc (void)
{
	if (xchg(&irq.note, false))
		pr_warn("rtw89: INTx: the chip interrupted while rtw89 was stopped; masked it (%u times, %u not ours): "
			"PCIE_HISR00 %08x/HIMR00 %08x, PCIE_HISR10 %08x/HIMR10 %08x, HISR0 %08x/HIMR0 %08x",
			irq.masked, irq.not_ours, irq.snap[0], irq.snap[1], irq.snap[2], irq.snap[3], irq.snap[4],
			irq.snap[5]);
	if (xchg(&irq.wake, false) && irq.wq)
		queue_work(irq.wq, &irq.thread_work);
}

/* ---- DMA ---- */
struct bounce {
	struct list_head node;
	void *va;               /* caller's buffer */
	void *buf;              /* DMA heap copy */
	dma_addr_t dma;
	size_t size;
	enum dma_data_direction dir;
};
static LIST_HEAD(bounces);
static spinlock_t bounce_lock;

void *dma_alloc_coherent (struct device *d, size_t size, dma_addr_t *dma, gfp_t f)
{
	u64 a;
	void *va;

	(void)d;
	(void)f;
	va = rtwk_dma_alloc(size, &a);
	*dma = va ? a : 0;
	return va;
}

void dma_free_coherent (struct device *d, size_t size, void *va, dma_addr_t dma)
{
	(void)d;
	(void)size;
	(void)dma;
	if (va)
		rtwk_dma_free(va);
}

dma_addr_t dma_map_single (struct device *d, void *va, size_t size, enum dma_data_direction dir)
{
	struct bounce *b;
	u64 a;

	(void)d;
	if (rtwk_dma_lookup(va, &a) == 0)
		return a;
	b = rtwk_alloc(sizeof(*b), 1);
	if (!b)
		return DMA_MAPPING_ERROR;
	b->buf = rtwk_dma_heap_alloc(size);
	if (!b->buf || rtwk_dma_lookup(b->buf, &a)) {
		rtwk_free(b);
		return DMA_MAPPING_ERROR;
	}
	b->va = va;
	b->dma = a;
	b->size = size;
	b->dir = dir;
	if (dir != DMA_FROM_DEVICE)
		memcpy(b->buf, va, size);
	spin_lock(&bounce_lock);
	list_add(&b->node, &bounces);
	spin_unlock(&bounce_lock);
	return a;
}

static struct bounce *find_bounce (dma_addr_t dma, bool unlink)
{
	struct bounce *b, *found = NULL;

	spin_lock(&bounce_lock);
	list_for_each_entry(b, &bounces, node) {
		if (b->dma == dma) {
			found = b;
			if (unlink)
				list_del(&b->node);
			break;
		}
	}
	spin_unlock(&bounce_lock);
	return found;
}

void dma_unmap_single (struct device *d, dma_addr_t dma, size_t size, enum dma_data_direction dir)
{
	struct bounce *b = find_bounce(dma, true);

	(void)d;
	(void)size;
	if (!b)
		return;
	if (dir != DMA_TO_DEVICE)
		memcpy(b->va, b->buf, b->size);
	rtwk_dma_heap_free(b->buf);
	rtwk_free(b);
}

void dma_sync_single_for_cpu (struct device *d, dma_addr_t dma, size_t size, enum dma_data_direction dir)
{
	struct bounce *b = find_bounce(dma, false);

	(void)d;
	if (b && dir != DMA_TO_DEVICE)
		memcpy(b->va, b->buf, min(size, b->size));
}

void dma_sync_single_for_device (struct device *d, dma_addr_t dma, size_t size, enum dma_data_direction dir)
{
	struct bounce *b = find_bounce(dma, false);

	(void)d;
	if (b && dir != DMA_FROM_DEVICE)
		memcpy(b->buf, b->va, min(size, b->size));
}

/* ---- firmware ---- */
int request_firmware (const struct firmware **fw, const char *name, struct device *d)
{
	struct firmware *f;
	void *data;
	size_t size;
	int ret;

	(void)d;
	*fw = NULL;
	ret = rtwk_firmware_load(name, &data, &size);
	if (ret)
		return ret;
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) {
		rtwk_free(data);
		return -ENOMEM;
	}
	f->data = data;
	f->size = size;
	*fw = f;
	return 0;
}

void release_firmware (const struct firmware *fw)
{
	if (!fw)
		return;
	rtwk_free((void *)fw->data);
	kfree(fw);
}

void rtw_device_init (void)
{
	spin_lock_init(&bounce_lock);
	memset(&irq, 0, sizeof(irq));
}
