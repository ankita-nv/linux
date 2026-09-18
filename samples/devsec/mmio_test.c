// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 NVIDIA Corporation */

/*
 * Out-of-tree smoke test for pci_tsm_mmio_alloc() / pci_tsm_mmio_setup() /
 * pci_tsm_coherent_range(), including the CXL-derived coherent-range path.
 * Needs only a struct pci_dev and a raw TDISP Device Interface Report
 * buffer, so it runs against samples/devsec/bus.c's fake endpoint without
 * the TDISP LOCKED/RUN state machine this tree doesn't have ported yet.
 *
 * Load after devsec_bus; check dmesg for PASS/FAIL of each case. Downstream
 * test tool, not part of the upstream PCI/TSM series.
 */

#define dev_fmt(fmt) "devsec_mmio_test: " fmt
#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pci-tsm.h>
#include <linux/pci.h>

static char *bdf;
module_param(bdf, charp, 0444);
MODULE_PARM_DESC(bdf,
		 "domain:bus:dev.fn (e.g. 0000:41:00.0) of a real PCI device to test against, "
		 "such as a CXL Type-2/3 endpoint with a pre-committed HDM decoder captured in "
		 "pdev->coh_resource[]. If unset, tests against the samples/devsec fake bus "
		 "endpoint (load devsec_bus first).");

static bool expect_coherent;
module_param(expect_coherent, bool, 0444);
MODULE_PARM_DESC(expect_coherent,
		 "Assert the coherent-range resolves successfully. Set this when bdf= points "
		 "at a device with a populated pdev->coh_resource[] entry. Default: assert it "
		 "fails closed, which is correct for the devsec fake bus (no CXL backing).");

/*
 * Mirrors the wire format of PCIe r7.0 section 11.3.11 "Device Interface
 * Report" as consumed by pci_tsm_mmio_alloc() in drivers/pci/tsm.c. That
 * struct is private to tsm.c, so it is duplicated here rather than shared.
 */
struct test_devif_mmio {
	__le64 phys;
	__le32 nr_pfns;
	__le32 attributes;
};

struct test_devif_report {
	__le16 interface_info;
	__le16 reserved;
	__le16 msi_x_message_control;
	__le16 lnr_control;
	__le32 tph_control;
	__le32 mmio_range_count;
	struct test_devif_mmio mmio[];
} __packed;

#define TEST_MMIO_ATTR_IS_NON_TEE BIT(2)
#define TEST_MMIO_ATTR_RANGE_ID GENMASK(31, 16)
#define TEST_MMIO_RANGE_ID_COHERENT 0xffff

static int test_pass, test_fail;

#define CHECK(cond, fmt, ...)                                        \
	do {                                                          \
		if (cond) {                                            \
			pr_info("PASS: " fmt "\n", ##__VA_ARGS__);     \
			test_pass++;                                   \
		} else {                                               \
			pr_err("FAIL: " fmt "\n", ##__VA_ARGS__);      \
			test_fail++;                                   \
		}                                                      \
	} while (0)

static struct test_devif_report *
build_report(size_t *len, u64 phys, u32 nr_pfns, u32 range_id, u32 extra_attr)
{
	struct test_devif_report *r;

	*len = struct_size(r, mmio, 1);
	r = kzalloc(*len, GFP_KERNEL);
	if (!r)
		return NULL;

	r->mmio_range_count = cpu_to_le32(1);
	r->mmio[0].phys = cpu_to_le64(phys);
	r->mmio[0].nr_pfns = cpu_to_le32(nr_pfns);
	r->mmio[0].attributes = cpu_to_le32(
		FIELD_PREP(TEST_MMIO_ATTR_RANGE_ID, range_id) | extra_attr);

	return r;
}

static void test_bar_range(struct pci_dev *pdev)
{
	struct pci_tsm_mmio *mmio;
	size_t len;

	if (pci_resource_len(pdev, 0) == 0 ||
	    !(pci_resource_flags(pdev, 0) & IORESOURCE_MEM)) {
		pr_err("FAIL: BAR0 not usable, skipping BAR-range case\n");
		test_fail++;
		return;
	}

	struct test_devif_report *r __free(kfree) =
		build_report(&len, pci_resource_start(pdev, 0), 1, 0, 0);
	CHECK(r, "allocate BAR-range report");
	if (!r)
		return;

	mmio = pci_tsm_mmio_alloc(pdev, r, len);
	CHECK(mmio, "pci_tsm_mmio_alloc() resolves a BAR-relative range");
	if (!mmio)
		return;

	CHECK(mmio->nr == 1 &&
	      pci_tsm_mmio_resource(mmio, 0)->start ==
		      pci_resource_start(pdev, 0),
	      "resolved range starts at BAR0 base");

	scoped_guard(device, &pdev->dev)
		CHECK(!pci_tsm_mmio_setup(pdev, mmio),
		      "pci_tsm_mmio_setup() marks the BAR range encrypted");
	pci_tsm_mmio_teardown(mmio);
	pci_tsm_mmio_free(pdev, mmio);
}

static void test_coherent_range(struct pci_dev *pdev, bool expect_success)
{
	struct pci_tsm_mmio *mmio;
	size_t len;

	/*
	 * The devsec fake bus is never enumerated as a CXL device, so
	 * coh_resource[] stays unpopulated and pci_tsm_mmio_alloc() must
	 * fail closed. Point this module at a real CXL device with a
	 * firmware-committed HDM decoder (bdf=<domain:bus:dev.fn>
	 * expect_coherent=1) to exercise the success path instead.
	 */
	struct test_devif_report *r __free(kfree) =
		build_report(&len, 0, 1, TEST_MMIO_RANGE_ID_COHERENT, 0);
	CHECK(r, "allocate coherent-range report");
	if (!r)
		return;

	mmio = pci_tsm_mmio_alloc(pdev, r, len);
	if (expect_success) {
		CHECK(mmio,
		      "pci_tsm_mmio_alloc() resolves the coherent range from pdev->coh_resource[]");
		if (mmio)
			pr_info("resolved coherent range: %pr\n",
				pci_tsm_mmio_resource(mmio, 0));
	} else {
		CHECK(!mmio,
		      "pci_tsm_mmio_alloc() fails closed for a coherent range with no CXL backing");
	}
	if (mmio) {
		pci_tsm_mmio_teardown(mmio);
		pci_tsm_mmio_free(pdev, mmio);
	}
}

static void test_bar_invalid_id(struct pci_dev *pdev)
{
	struct pci_tsm_mmio *mmio;
	size_t len;

	if (pci_resource_len(pdev, 0) == 0)
		return;

	/* range_id names a BAR that does not exist on this device */
	struct test_devif_report *r __free(kfree) =
		build_report(&len, pci_resource_start(pdev, 0), 1,
			    PCI_STD_NUM_BARS, 0);
	CHECK(r, "allocate invalid-BAR-id report");
	if (!r)
		return;

	mmio = pci_tsm_mmio_alloc(pdev, r, len);
	CHECK(!mmio, "pci_tsm_mmio_alloc() rejects an out-of-range BAR id");
	if (mmio) {
		pci_tsm_mmio_teardown(mmio);
		pci_tsm_mmio_free(pdev, mmio);
	}
}

static struct pci_dev *find_target_pdev(void)
{
	unsigned int domain, bus, dev, fn;

	if (!bdf) {
		struct pci_dev *pdev = NULL;

		/*
		 * devsec_bus's root ports and endpoints share the same
		 * 8086:ffff id, so a plain id match can return a bridge.
		 * Skip bridges.
		 */
		while ((pdev = pci_get_device(0x8086, 0xffff, pdev))) {
			if ((pdev->class >> 8) != PCI_CLASS_BRIDGE_PCI)
				return pdev;
		}
		return NULL;
	}

	if (sscanf(bdf, "%x:%x:%x.%x", &domain, &bus, &dev, &fn) != 4) {
		pr_err("bad bdf '%s', expected domain:bus:dev.fn e.g. 0000:41:00.0\n",
		       bdf);
		return NULL;
	}

	return pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(dev, fn));
}

static int __init devsec_mmio_test_init(void)
{
	struct pci_dev *pdev __free(pci_dev_put) = find_target_pdev();

	if (!pdev) {
		pr_err("no target device found (bdf=%s); load devsec_bus first if not using bdf=\n",
		       bdf ?: "<unset>");
		return -ENODEV;
	}

	pr_info("running against %s\n", pci_name(pdev));

	test_bar_range(pdev);
	test_coherent_range(pdev, expect_coherent);
	test_bar_invalid_id(pdev);

	pr_info("%d passed, %d failed\n", test_pass, test_fail);

	return test_fail ? -EINVAL : 0;
}
module_init(devsec_mmio_test_init);

static void __exit devsec_mmio_test_exit(void)
{
}
module_exit(devsec_mmio_test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Out-of-tree smoke test for pci_tsm_mmio_alloc()/pci_tsm_coherent_range()");
