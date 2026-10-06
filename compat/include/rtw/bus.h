/*
 * rtw89win Linux compatibility: struct device, PCI, DMA, interrupts, firmware, module macros, and the empty
 * stand-ins for subsystems we do not have (DMI, ACPI methods, debugfs, devcoredump, PM).
 * The PCI device is the one KMDF bound; driver\ fills struct pci_dev and calls the pci_driver's probe.
 * Copyright (c) 2026 Meari
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RTW_BUS_H
#define RTW_BUS_H

#include <rtw/base.h>
#include <rtw/sched.h>
#include <rtw/mem.h>

/* ---- device ---- */
struct device_driver {
	const char *name;
	const struct dev_pm_ops *pm;
};
struct device {
	const char *init_name;
	void *driver_data;
	void *os_dev;                   /* driver\ RTW_DEVICE, for rtwk_pci_cfg_* */
	struct list_head devres;        /* devm_* allocations */
	struct device_driver *driver;
	u64 dma_mask;
};
static inline void *dev_get_drvdata (const struct device *d) { return d->driver_data; }
static inline void dev_set_drvdata (struct device *d, void *p) { d->driver_data = p; }
static inline const char *dev_name (const struct device *d) { return d->init_name ? d->init_name : "rtw89"; }
#define get_device(d)                   (d)
#define put_device(d)                   do { } while (0)
#define device_init_wakeup(d, v)        0
#define device_set_wakeup_capable(d, v) do { } while (0)
#define device_may_wakeup(d)            0
void *devm_kmalloc (struct device *d, size_t n, gfp_t f);
#define devm_kzalloc(d, n, f)           devm_kmalloc(d, n, (f) | __GFP_ZERO)
#define devm_kcalloc(d, n, s, f)        devm_kzalloc(d, (n) * (s), f)
#define devm_kmalloc_array(d, n, s, f)  devm_kmalloc(d, (n) * (s), f)
void *devm_kmemdup (struct device *d, const void *s, size_t n, gfp_t f);
void devm_kfree (struct device *d, const void *p);
void devres_release_all (struct device *d);

void rtw_dev_printk (int level, const struct device *d, const char *fmt, ...);
#define dev_err(d, fmt, ...)            rtw_dev_printk(0, d, fmt, ##__VA_ARGS__)
#define dev_warn(d, fmt, ...)           rtw_dev_printk(1, d, fmt, ##__VA_ARGS__)
#define dev_notice(d, fmt, ...)         rtw_dev_printk(2, d, fmt, ##__VA_ARGS__)
#define dev_info(d, fmt, ...)           rtw_dev_printk(2, d, fmt, ##__VA_ARGS__)
#define dev_dbg(d, fmt, ...)            do { } while (0)
#define dev_err_once(d, fmt, ...)       __rtw_once(dev_err(d, fmt, ##__VA_ARGS__))
#define dev_warn_once(d, fmt, ...)      __rtw_once(dev_warn(d, fmt, ##__VA_ARGS__))
#define dev_info_once(d, fmt, ...)      __rtw_once(dev_info(d, fmt, ##__VA_ARGS__))
#define dev_err_ratelimited(d, fmt, ...)  dev_err(d, fmt, ##__VA_ARGS__)
#define dev_warn_ratelimited(d, fmt, ...) dev_warn(d, fmt, ##__VA_ARGS__)
#define dev_info_ratelimited(d, fmt, ...) dev_info(d, fmt, ##__VA_ARGS__)
#define dev_err_probe(d, err, fmt, ...) ({ dev_err(d, fmt, ##__VA_ARGS__); (err); })

/* ---- power management ---- */
struct dev_pm_ops {
	int (*suspend)(struct device *dev);
	int (*resume)(struct device *dev);
};
#define SIMPLE_DEV_PM_OPS(name, s, r)   const struct dev_pm_ops name = { .suspend = (s), .resume = (r) }
#define DEFINE_SIMPLE_DEV_PM_OPS(name, s, r) SIMPLE_DEV_PM_OPS(name, s, r)
#define pm_ptr(p)                       (p)
#define pm_sleep_ptr(p)                 (p)
typedef int pm_message_t;

/* ---- DMA (compat/device.c) ---- */
enum dma_data_direction {
	DMA_BIDIRECTIONAL = 0,
	DMA_TO_DEVICE = 1,
	DMA_FROM_DEVICE = 2,
	DMA_NONE = 3,
};
#define DMA_BIT_MASK(n)                 (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
#define DMA_MAPPING_ERROR               (~(dma_addr_t)0)
void *dma_alloc_coherent (struct device *d, size_t size, dma_addr_t *dma, gfp_t f);
void dma_free_coherent (struct device *d, size_t size, void *va, dma_addr_t dma);
dma_addr_t dma_map_single (struct device *d, void *va, size_t size, enum dma_data_direction dir);
void dma_unmap_single (struct device *d, dma_addr_t dma, size_t size, enum dma_data_direction dir);
void dma_sync_single_for_cpu (struct device *d, dma_addr_t dma, size_t size, enum dma_data_direction dir);
void dma_sync_single_for_device (struct device *d, dma_addr_t dma, size_t size, enum dma_data_direction dir);
static inline int dma_mapping_error (struct device *d, dma_addr_t dma) { (void)d; return dma == DMA_MAPPING_ERROR; }
/* the DMA enabler in driver\ is 32-bit; asking for more than 32 bits fails like on a non-DAC bridge */
static inline int dma_set_mask (struct device *d, u64 m) { if (m > DMA_BIT_MASK(32)) return -EIO; d->dma_mask = m; return 0; }
#define dma_set_coherent_mask(d, m)     dma_set_mask(d, m)
#define dma_set_mask_and_coherent(d, m) dma_set_mask(d, m)

/* ---- MMIO (Linux arm64 ordering: dma_rmb after a read, dma_wmb before a write) ---- */
static inline u8 readb (const volatile void __iomem *a) { u8 v = *(const volatile u8 *)a; dma_rmb(); return v; }
static inline u16 readw (const volatile void __iomem *a) { u16 v = *(const volatile u16 *)a; dma_rmb(); return v; }
static inline u32 readl (const volatile void __iomem *a) { u32 v = *(const volatile u32 *)a; dma_rmb(); return v; }
static inline u64 readq (const volatile void __iomem *a) { u64 v = *(const volatile u64 *)a; dma_rmb(); return v; }
static inline void writeb (u8 v, volatile void __iomem *a) { dma_wmb(); *(volatile u8 *)a = v; }
static inline void writew (u16 v, volatile void __iomem *a) { dma_wmb(); *(volatile u16 *)a = v; }
static inline void writel (u32 v, volatile void __iomem *a) { dma_wmb(); *(volatile u32 *)a = v; }
static inline void writeq (u64 v, volatile void __iomem *a) { dma_wmb(); *(volatile u64 *)a = v; }
#define readl_relaxed(a)                (*(const volatile u32 *)(a))
#define writel_relaxed(v, a)            (*(volatile u32 *)(a) = (v))
#define ioread8(a)                      readb(a)
#define ioread16(a)                     readw(a)
#define ioread32(a)                     readl(a)
#define iowrite8(v, a)                  writeb(v, a)
#define iowrite16(v, a)                 writew(v, a)
#define iowrite32(v, a)                 writel(v, a)

/* ---- interrupts ---- */
typedef int irqreturn_t;
#define IRQ_NONE                        0
#define IRQ_HANDLED                     1
#define IRQ_WAKE_THREAD                 2
#define IRQF_SHARED                     0x80
#define IRQF_ONESHOT                    0x2000
typedef irqreturn_t (*irq_handler_t)(int irq, void *dev_id);
int request_threaded_irq (unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn, unsigned long flags,
			  const char *name, void *dev_id);
#define request_irq(irq, h, f, n, d)    request_threaded_irq(irq, h, NULL, f, n, d)
void free_irq (unsigned int irq, void *dev_id);
#define devm_request_threaded_irq(dev, irq, h, t, f, n, d) request_threaded_irq(irq, h, t, f, n, d)
#define devm_free_irq(dev, irq, d)      free_irq(irq, d)
void synchronize_irq (unsigned int irq);
void disable_irq (unsigned int irq);
void enable_irq (unsigned int irq);

/* ---- PCI ---- */
#define PCI_ANY_ID                      (~0u)
#define PCI_VENDOR_ID_REALTEK           0x10ec
#define PCI_VENDOR_ID_INTEL             0x8086
#define PCI_VENDOR_ID_ASMEDIA           0x1b21
#define PCI_DEVICE(v, d)                .vendor = (v), .device = (d), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define PCI_DEVICE_SUB(v, d, sv, sd)    .vendor = (v), .device = (d), .subvendor = (sv), .subdevice = (sd)
struct pci_device_id {
	u32 vendor, device;
	u32 subvendor, subdevice;
	u32 class, class_mask;
	kernel_ulong_t driver_data;
};
struct pci_bus {
	struct pci_dev *self;           /* upstream bridge; NULL here (the RK3588 root port is not visible) */
	unsigned char number;
};
struct pci_dev {
	struct device dev;
	struct pci_bus *bus;
	u16 vendor, device;
	u16 subsystem_vendor, subsystem_device;
	u8 revision;
	u32 class;
	unsigned int irq;
	unsigned int devfn;
	u8 pcie_cap;
	void __iomem *bar_va[6];        /* filled by driver\ (only the mapped BAR) */
	u64 bar_len[6];
	void *irq_os;                   /* driver\ interrupt context */
	bool is_busmaster;
};
#define to_pci_dev(d)                   container_of(d, struct pci_dev, dev)
static inline void *pci_get_drvdata (struct pci_dev *p) { return p->dev.driver_data; }
static inline void pci_set_drvdata (struct pci_dev *p, void *d) { p->dev.driver_data = d; }
static inline const char *pci_name (const struct pci_dev *p) { (void)p; return "0002:21:00.0"; }
static inline struct pci_dev *pci_upstream_bridge (struct pci_dev *p) { return p->bus ? p->bus->self : NULL; }
static inline int pci_enable_device (struct pci_dev *p) { (void)p; return 0; }
static inline void pci_disable_device (struct pci_dev *p) { (void)p; }
static inline void pci_set_master (struct pci_dev *p) { p->is_busmaster = true; }
static inline void pci_clear_master (struct pci_dev *p) { p->is_busmaster = false; }
static inline int pci_request_regions (struct pci_dev *p, const char *n) { (void)p; (void)n; return 0; }
static inline void pci_release_regions (struct pci_dev *p) { (void)p; }
static inline u64 pci_resource_len (struct pci_dev *p, int bar) { return p->bar_len[bar]; }
static inline void __iomem *pci_iomap (struct pci_dev *p, int bar, unsigned long max) { (void)max; return p->bar_va[bar]; }
static inline void pci_iounmap (struct pci_dev *p, void __iomem *a) { (void)p; (void)a; }
int pci_read_config_byte (const struct pci_dev *p, int where, u8 *val);
int pci_read_config_word (const struct pci_dev *p, int where, u16 *val);
int pci_read_config_dword (const struct pci_dev *p, int where, u32 *val);
int pci_write_config_byte (const struct pci_dev *p, int where, u8 val);
int pci_write_config_word (const struct pci_dev *p, int where, u16 val);
int pci_write_config_dword (const struct pci_dev *p, int where, u32 val);
int pcie_capability_read_word (struct pci_dev *p, int pos, u16 *val);
int pcie_capability_read_dword (struct pci_dev *p, int pos, u32 *val);
int pcie_capability_write_word (struct pci_dev *p, int pos, u16 val);
int pcie_capability_clear_and_set_word (struct pci_dev *p, int pos, u16 clear, u16 set);
#define pcie_capability_set_word(p, pos, set)     pcie_capability_clear_and_set_word(p, pos, 0, set)
#define pcie_capability_clear_word(p, pos, clear) pcie_capability_clear_and_set_word(p, pos, clear, 0)
int pci_find_capability (struct pci_dev *p, int cap);
u16 pci_find_ext_capability (struct pci_dev *p, int cap);
#define PCI_IRQ_INTX                    (1 << 0)
#define PCI_IRQ_LEGACY                  PCI_IRQ_INTX
#define PCI_IRQ_MSI                     (1 << 1)
#define PCI_IRQ_MSIX                    (1 << 2)
#define PCI_IRQ_ALL_TYPES               (PCI_IRQ_INTX | PCI_IRQ_MSI | PCI_IRQ_MSIX)
int pci_alloc_irq_vectors (struct pci_dev *p, unsigned int min, unsigned int max, unsigned int flags);
static inline void pci_free_irq_vectors (struct pci_dev *p) { (void)p; }
static inline int pci_irq_vector (struct pci_dev *p, unsigned int n) { (void)n; return (int)p->irq; }
static inline int pci_disable_link_state (struct pci_dev *p, int s) { (void)p; (void)s; return 0; }
static inline int pci_set_power_state (struct pci_dev *p, int s) { (void)p; (void)s; return 0; }
static inline int pci_save_state (struct pci_dev *p) { (void)p; return 0; }
static inline void pci_restore_state (struct pci_dev *p) { (void)p; }
static inline int pci_enable_wake (struct pci_dev *p, int state, bool enable) { (void)p; (void)state; (void)enable; return 0; }
#define PCI_D0                          0
#define PCI_D3hot                       3
#define PCI_COMMAND                     0x04
#define PCI_COMMAND_MEMORY              0x2
#define PCI_COMMAND_MASTER              0x4
#define PCI_COMMAND_INTX_DISABLE        0x400
#define PCI_CAP_ID_EXP                  0x10
#define PCI_EXP_DEVCTL                  0x08
#define PCI_EXP_LNKCAP                  0x0c
#define PCI_EXP_LNKCTL                  0x10
#define PCI_EXP_LNKCTL_ASPMC            0x0003
#define PCI_EXP_LNKCTL_ASPM_L0S         0x0001
#define PCI_EXP_LNKCTL_ASPM_L1          0x0002
#define PCI_EXP_LNKCTL_CLKREQ_EN        0x0100
#define PCI_EXP_LNKSTA                  0x12
#define PCI_EXP_DEVCTL2                 0x28
#define PCI_EXP_DEVCTL2_COMP_TMOUT_DIS  0x0010
#define PCI_EXP_DEVCTL2_LTR_EN          0x0400
#define PCI_EXT_CAP_ID_L1SS             0x1e
#define PCI_L1SS_CAP                    0x04
#define PCI_L1SS_CTL1                   0x08
#define PCI_L1SS_CTL1_L1SS_MASK         0x0000000f
#define PCI_L1SS_CTL1_PCIPM_L1_2        0x00000001
#define PCI_L1SS_CTL1_PCIPM_L1_1        0x00000002
#define PCI_L1SS_CTL1_ASPM_L1_2         0x00000004
#define PCI_L1SS_CTL1_ASPM_L1_1         0x00000008
typedef unsigned int pci_ers_result_t;
#define PCI_ERS_RESULT_NONE             1
#define PCI_ERS_RESULT_CAN_RECOVER      2
#define PCI_ERS_RESULT_NEED_RESET       3
#define PCI_ERS_RESULT_DISCONNECT       4
#define PCI_ERS_RESULT_RECOVERED        5
typedef unsigned int pci_channel_state_t;
struct pci_error_handlers {
	pci_ers_result_t (*error_detected)(struct pci_dev *dev, pci_channel_state_t state);
	pci_ers_result_t (*slot_reset)(struct pci_dev *dev);
	void (*resume)(struct pci_dev *dev);
};
struct pci_driver {
	const char *name;
	const struct pci_device_id *id_table;
	int (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
	void (*remove)(struct pci_dev *dev);
	void (*shutdown)(struct pci_dev *dev);
	struct device_driver driver;
	const struct pci_error_handlers *err_handler;
};
/* The 8852BE module's pci_driver, picked up by driver\ (module_pci_driver in rtw8852be.c). */
extern struct pci_driver *rtwk_pci_driver;
#define module_pci_driver(drv)          struct pci_driver *rtwk_pci_driver = &(drv)

/* ---- firmware (compat/device.c) ---- */
struct firmware {
	size_t size;
	const u8 *data;
};
int request_firmware (const struct firmware **fw, const char *name, struct device *d);
#define firmware_request_nowarn(fw, n, d) request_firmware(fw, n, d)
#define request_firmware_direct(fw, n, d) request_firmware(fw, n, d)
void release_firmware (const struct firmware *fw);

/* ---- module ---- */
struct module;
#define THIS_MODULE                     ((struct module *)0)
#define KBUILD_MODNAME                  "rtw89"
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define MODULE_FIRMWARE(x)
#define MODULE_DEVICE_TABLE(type, name)
#define MODULE_PARM_DESC(var, desc)
#define MODULE_IMPORT_NS(ns)
#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)
#define EXPORT_SYMBOL_NS_GPL(sym, ns)
#define module_param(name, type, perm)
#define module_param_named(name, var, type, perm)
#define module_init(fn)
#define module_exit(fn)

/* ---- DMI: no SMBIOS matching ---- */
/* The fields rtw89's quirk tables name (rtw8852ce.c uses DMI_PRODUCT_SKU). The values only label the slots: the
 * functions below never match, so a table entry has no effect. */
enum dmi_field { DMI_NONE, DMI_SYS_VENDOR, DMI_PRODUCT_NAME, DMI_PRODUCT_VERSION, DMI_BOARD_VENDOR, DMI_BOARD_NAME,
		 DMI_PRODUCT_SKU };
struct dmi_strmatch {
	unsigned char slot:7;
	unsigned char exact_match:1;
	char substr[79];
};
struct dmi_system_id {
	int (*callback)(const struct dmi_system_id *);
	const char *ident;
	struct dmi_strmatch matches[4];
	void *driver_data;
};
#define DMI_MATCH(a, b)                 { .slot = a, .substr = b }
#define DMI_EXACT_MATCH(a, b)           { .slot = a, .substr = b, .exact_match = 1 }
static inline const struct dmi_system_id *dmi_first_match (const struct dmi_system_id *list) { (void)list; return NULL; }
static inline int dmi_check_system (const struct dmi_system_id *list) { (void)list; return 0; }

/* ---- ACPI: no _DSM/methods from Windows side; rtw89 falls back to defaults ---- */
typedef void *acpi_handle;
typedef u32 acpi_status;
typedef u64 acpi_size;
typedef char *acpi_string;
#define AE_OK                           0
#define AE_ERROR                        1
#define AE_NOT_FOUND                    5
#define ACPI_SUCCESS(s)                 ((s) == AE_OK)
#define ACPI_FAILURE(s)                 ((s) != AE_OK)
#define ACPI_HANDLE(dev)                ((acpi_handle)NULL)
#define ACPI_ALLOCATE_BUFFER            ((acpi_size)-1)
#define ACPI_TYPE_INTEGER               1
#define ACPI_TYPE_STRING                2
#define ACPI_TYPE_BUFFER                3
#define ACPI_TYPE_PACKAGE               4
union acpi_object {
	u32 type;
	struct { u32 type; u64 value; } integer;
	struct { u32 type; u32 length; char *pointer; } string;
	struct { u32 type; u32 length; u8 *pointer; } buffer;
	struct { u32 type; u32 count; union acpi_object *elements; } package;
};
struct acpi_buffer {
	acpi_size length;
	void *pointer;
};
typedef struct {
	u8 b[16];
} guid_t;
/* GUID_INIT: the first three fields (32, 16, 16 bits) are stored least significant byte first, d0..d7 as given */
#define __rtw_byte(v, i)                ((u8)((v) >> (8 * (i))))
#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
	((guid_t){ { __rtw_byte(a, 0), __rtw_byte(a, 1), __rtw_byte(a, 2), __rtw_byte(a, 3), \
		     __rtw_byte(b, 0), __rtw_byte(b, 1), __rtw_byte(c, 0), __rtw_byte(c, 1), \
		     d0, d1, d2, d3, d4, d5, d6, d7 } })
static inline union acpi_object *acpi_evaluate_dsm (acpi_handle h, const guid_t *g, u64 rev, u64 func, union acpi_object *argv)
{ (void)h; (void)g; (void)rev; (void)func; (void)argv; return NULL; }
static inline acpi_status acpi_evaluate_object (acpi_handle h, const char *path, void *args, struct acpi_buffer *buf)
{ (void)h; (void)path; (void)args; (void)buf; return AE_NOT_FOUND; }
#define ACPI_FREE(p)                    kfree(p)
static inline acpi_status acpi_get_handle (acpi_handle parent, acpi_string path, acpi_handle *h)
{ (void)parent; (void)path; *h = NULL; return AE_NOT_FOUND; }

/* ---- debugfs / devcoredump: not available ---- */
struct dentry;
struct file;
struct inode;
struct seq_file;
struct file_operations {
	struct module *owner;
};
#define debugfs_create_dir(n, p)        ((struct dentry *)NULL)
#define debugfs_create_file(n, m, p, d, f) ((struct dentry *)NULL)
#define debugfs_remove_recursive(d)     do { } while (0)
static inline void dev_coredumpv (struct device *d, void *data, size_t len, gfp_t gfp) { (void)d; (void)len; (void)gfp; vfree(data); }

/* ---- USB: types only (usb.h is not built) ---- */
struct usb_interface;
struct usb_device;
struct usb_device_id;
struct urb;
struct usb_anchor {
	struct list_head urb_list;
};

#endif
