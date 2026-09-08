# devsec_mmio_test: testing pci_tsm_mmio_alloc() / pci_tsm_coherent_range()

This is a runbook for exercising `pci_tsm_mmio_alloc()`,
`pci_tsm_mmio_setup()`/`pci_tsm_mmio_teardown()`, and
`pci_tsm_coherent_range()` (drivers/pci/tsm.c) -- the CXL coherent-range
derivation logic added on top of v7.3-rc1 in this branch -- using the
`samples/devsec/mmio_test.c` module. It is written to be followed
step-by-step by an agent (e.g. Claude) with shell access to the target
machine, including deciding which scenario applies and interpreting the
result.

Scope: `samples/devsec/mmio_test.c` and `samples/devsec/bus.c` (the fake
PCI bus/endpoint it can target) are a downstream test tool, not part of
the upstream PCI/TSM series. They are not intended to be upstreamed as-is.

## What this tests

`pci_tsm_mmio_alloc(pdev, report, report_len)` parses a raw TDISP Device
Interface Report and, for the "coherent range" entry
(`PCI_TSM_DEVIF_REPORT_MMIO_RANGE_ID_COHERENT`), calls
`pci_tsm_coherent_range()` to resolve the device's CXL host physical
address window. That function resolves the window entirely from
`pdev->coh_resource[]`, the snapshot `pci_cxl_hdm_init()` takes of any
committed CXL HDM decoder strictly before a driver can bind ("PCI/CXL:
Populate pdev->coh_resource[] from cached committed HDM decoders"). It
does not consult the CXL region/decoder machinery at all, so there is
no separate success path for a driver-committed region.

This only needs a `struct pci_dev`, not any TDISP LOCKED/RUN state
machine (which is not ported into this tree), so the test module calls
it directly.

## Prerequisites

- This tree, branch `v7.3-rc1-guest-cxl-devsec-test` (or later), built
  and **booted** (module ABI must match the running kernel -- building
  the module against this tree and then `insmod`ing it into a
  differently-versioned running kernel, e.g. a host distro kernel, will
  fail with a version-magic mismatch).
- `.config` has `CONFIG_SAMPLE_DEVSEC=m`, `CONFIG_PCI_TSM=y`,
  `CONFIG_CXL_HDM=y`. If `CONFIG_CXL_HDM` is off, `pdev->coh_resource[]`
  doesn't exist and the coherent-range case always fails closed:
  ```
  grep -E '^CONFIG_PCI_TSM|^CONFIG_CXL_HDM' /boot/config-$(uname -r)   # or your .config
  ```

## Build

```
make -j"$(nproc)" M=samples/devsec
```
Expect `devsec_bus.ko`, `devsec_common.ko`, `devsec_mmio_test.ko` with no
`error:` output (unrelated pre-existing warnings from other headers are
not a build failure -- only fail on `error:` lines from files under
`samples/devsec/`).

## Decide which scenario applies

Check for CXL hardware first:
```
ls /sys/bus/cxl/devices/ 2>/dev/null
lspci -d ::0502   # CXL memory device class; adjust if the device reports differently
```
- Nothing found -> **Scenario A** (no CXL hardware).
- A CXL Type-2/3 endpoint present -> **Scenario B**.

## Scenario A: no real CXL device present

Tests report parsing, BAR-relative range resolution, and the
coherent-range *fail-closed* contract, against the fake `devsec_bus`
endpoint (which has no CXL backing at all).

```
insmod samples/devsec/devsec_common.ko
insmod samples/devsec/devsec_bus.ko
insmod samples/devsec/devsec_mmio_test.ko
dmesg | tail -20
```

Expected (`expect_coherent` defaults to false):
```
devsec_mmio_test: running against 10000:01:00.0
devsec_mmio_test: PASS: allocate BAR-range report
devsec_mmio_test: PASS: pci_tsm_mmio_alloc() resolves a BAR-relative range
devsec_mmio_test: PASS: resolved range starts at BAR0 base
devsec_mmio_test: PASS: pci_tsm_mmio_setup() marks the BAR range encrypted
devsec_mmio_test: PASS: allocate coherent-range report
devsec_mmio_test: PASS: pci_tsm_mmio_alloc() fails closed for a coherent range with no CXL backing
devsec_mmio_test: PASS: allocate invalid-BAR-id report
devsec_mmio_test: PASS: pci_tsm_mmio_alloc() rejects an out-of-range BAR id
devsec_mmio_test: 5 passed, 0 failed
```
`insmod` itself succeeding (module init returns 0) is an additional
pass/fail signal: the module returns `-EINVAL` if any case fails.

Teardown: `rmmod devsec_mmio_test devsec_bus devsec_common`.

## Scenario B: real CXL device present, decoder pre-committed

This exercises the actual coherent-range *success* path, which cannot be
validated with the fake bus. It requires the device's HDM decoder to
already be committed in hardware by firmware/BIOS (or a prior boot)
strictly before `pci_cxl_hdm_init()` first caches this device's
decoders -- that is the only moment `pdev->coh_resource[]` is
populated. A decoder committed later by a Linux CXL driver (e.g. via
`cxl create-region`) is not captured and will not make this pass.

Get the device's BDF:
```
lspci -d ::0502
# e.g. 0000:41:00.0
```

Confirm the decoder is already committed and check the resolved range
before the test module even loads, e.g. via `dmesg` for CXL HDM-init
messages at boot, or `/proc/iomem` (a populated `coh_resource[]` entry
that was successfully inserted shows up there once "PCI/CXL: insert and
remove coh_resource entries from iomem_resource" is applied):
```
grep -i "CXL Coherent" /proc/iomem
```

Then run the test:
```
insmod samples/devsec/devsec_mmio_test.ko bdf=0000:41:00.0 expect_coherent=1
dmesg | tail -20
```
Expected: `PASS: pci_tsm_mmio_alloc() resolves the coherent range from
pdev->coh_resource[]`, plus a line `resolved coherent range: [mem
BASE-END]`. Cross-check that range against the `/proc/iomem` entry (or
an independent read of the decoder BASE/SIZE registers, e.g.
`setpci`/EFI tooling) -- they must match exactly.

If it does *not* resolve as expected, turn on debug prints and re-run:
```
echo 'file drivers/pci/tsm.c +p' > /sys/kernel/debug/dynamic_debug/control
insmod samples/devsec/devsec_mmio_test.ko bdf=0000:41:00.0 expect_coherent=1
dmesg | tail -40
```

## Teardown (all scenarios)

```
rmmod devsec_mmio_test
rmmod devsec_bus 2>/dev/null       # only if loaded (Scenario A)
rmmod devsec_common 2>/dev/null    # only if loaded (Scenario A)
```

## Module parameters reference

| param             | type  | default | meaning                                                                 |
|-------------------|-------|---------|--------------------------------------------------------------------------|
| `bdf`             | charp | unset   | `domain:bus:dev.fn` of a real device to test against. Unset = fake devsec_bus endpoint (requires `devsec_bus` loaded first). |
| `expect_coherent` | bool  | false   | Assert the coherent-range case *succeeds* (real CXL device with a resolvable range) instead of asserting it fails closed (fake bus / no CXL backing). |
