/* SPDX-License-Identifier: ISC */
/*
 * Authors: Wei Chen <Wei.Chen@arm.com>
 *
 * Copyright (c) 2018 Arm Ltd.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <uk/config.h>
#include <libfdt.h>
#include <uk/plat/common/sections.h>
#include <kvm/console.h>
#include <kvm/config.h>
#include <uk/assert.h>
#include <kvm-arm/mm.h>
#include <kvm/intctrl.h>
#include <arm/cpu.h>
#include <uk/arch/limits.h>
#include <stdbool.h>
#include <uk/plat/bootstrap.h>
#include <uk/sched.h>

#include <uk/plat/memory.h>
#include <uk/plat/io.h>
#include <uk/plat/lcpu.h>
#include <arm/psci.h>
#include <ofw/fdt.h>

#ifdef CONFIG_SMP
#include <uk/plat/smp.h>
int aps_ready;
int smp_started;
int mp_ncpus;
int smp_cpus = 1;	/* how many cpu's running */
void mpentry(unsigned long cpu);
uint8_t secondary_stacks[MAXCPU - 1][__PAGE_SIZE * 4];
int cpu_possible_map[MAXCPU];
static int cpu0 = -1;
#endif


struct kvmplat_config _libkvmplat_cfg = { 0 };

#define MAX_CMDLINE_SIZE 1024
static char cmdline[MAX_CMDLINE_SIZE];
static const char *appname = CONFIG_UK_NAME;

smcc_psci_callfn_t smcc_psci_call;

extern void _libkvmplat_newstack(uint64_t stack_start,
			void (*tramp)(void *), void *arg);

int psci_cpu_on(unsigned long cpu, unsigned long entry)
{
	/* PSCI v0.1 and v0.2 both support cpu_on. the parameter @cpu after
	 * @entry will be passed as x0 for entry */
	return smcc_psci_hvc_call(PSCI_FNID_CPU_ON, cpu, entry, cpu);
}

static void _init_dtb(void *dtb_pointer)
{
	int ret;

	if ((ret = fdt_check_header(dtb_pointer)))
		UK_CRASH("Invalid DTB: %s\n", fdt_strerror(ret));

	_libkvmplat_cfg.dtb = dtb_pointer;
	uk_pr_info("Found device tree on: %p\n", dtb_pointer);
}

static void _dtb_get_psci_method(void)
{
	int fdtpsci, len;
	const char *fdtmethod;

	/*
	 * We just support PSCI-0.2 and PSCI-1.0, the PSCI-0.1 would not
	 * be supported.
	 */
	fdtpsci = fdt_node_offset_by_compatible(_libkvmplat_cfg.dtb,
						-1, "arm,psci-1.0");
	if (fdtpsci < 0)
		fdtpsci = fdt_node_offset_by_compatible(_libkvmplat_cfg.dtb,
							-1, "arm,psci-0.2");
	if (fdtpsci < 0) {
		uk_pr_info("No PSCI conduit found in DTB\n");
		goto enomethod;
	}

	fdtmethod = fdt_getprop(_libkvmplat_cfg.dtb, fdtpsci, "method", &len);
	if (!fdtmethod || (len <= 0)) {
		uk_pr_info("No PSCI method found\n");
		goto enomethod;
	}

	if (!strcmp(fdtmethod, "hvc"))
		smcc_psci_call = smcc_psci_hvc_call;
	else if (!strcmp(fdtmethod, "smc"))
		smcc_psci_call = smcc_psci_smc_call;
	else {
		uk_pr_info("Invalid PSCI conduit method: %s\n",
			   fdtmethod);
		goto enomethod;
	}

	uk_pr_info("PSCI method: %s\n", fdtmethod);
	return;

enomethod:
	uk_pr_info("Support PSCI from PSCI-0.2\n");
	smcc_psci_call = NULL;
}

static void _init_dtb_mem(void)
{
	int fdt_mem, prop_len = 0, prop_min_len;
	int naddr, nsize;
	const uint64_t *regs;
	uint64_t mem_base, mem_size, max_addr;

	/* search for assigned VM memory in DTB */
	if (fdt_num_mem_rsv(_libkvmplat_cfg.dtb) != 0)
		uk_pr_warn("Reserved memory is not supported\n");

	fdt_mem = fdt_node_offset_by_prop_value(_libkvmplat_cfg.dtb, -1,
						"device_type",
						"memory", sizeof("memory"));
	if (fdt_mem < 0) {
		uk_pr_warn("No memory found in DTB\n");
		return;
	}

	naddr = fdt_address_cells(_libkvmplat_cfg.dtb, fdt_mem);
	if (naddr < 0 || naddr >= FDT_MAX_NCELLS)
		UK_CRASH("Could not find proper address cells!\n");

	nsize = fdt_size_cells(_libkvmplat_cfg.dtb, fdt_mem);
	if (nsize < 0 || nsize >= FDT_MAX_NCELLS)
		UK_CRASH("Could not find proper size cells!\n");

	/*
	 * QEMU will always provide us at least one bank of memory.
	 * unikraft will use the first bank for the time-being.
	 */
	regs = fdt_getprop(_libkvmplat_cfg.dtb, fdt_mem, "reg", &prop_len);

	/*
	 * The property must contain at least the start address
	 * and size, each of which is 8-bytes.
	 */
	prop_min_len = (int)sizeof(fdt32_t) * (naddr + nsize);
	if (regs == NULL || prop_len < prop_min_len)
		UK_CRASH("Bad 'reg' property: %p %d\n", regs, prop_len);

	/* If we have more than one memory bank, give a warning messasge */
	if (prop_len > prop_min_len)
		uk_pr_warn("Currently, we support only one memory bank!\n");

	mem_base = fdt64_to_cpu(regs[0]);
	mem_size = fdt64_to_cpu(regs[1]);
	if (mem_base > __TEXT)
		UK_CRASH("Fatal: Image outside of RAM\n");

	max_addr = mem_base + mem_size;
	_libkvmplat_cfg.pagetable.start = ALIGN_DOWN((uintptr_t)__END,
						     __PAGE_SIZE);
	_libkvmplat_cfg.pagetable.len   = ALIGN_UP(page_table_size,
						   __PAGE_SIZE);
	_libkvmplat_cfg.pagetable.end   = _libkvmplat_cfg.pagetable.start
					  + _libkvmplat_cfg.pagetable.len;

	/* AArch64 require stack be 16-bytes alignment by default */
	_libkvmplat_cfg.bstack.end   = ALIGN_DOWN(max_addr,
						  __STACK_ALIGN_SIZE);
	_libkvmplat_cfg.bstack.len   = ALIGN_UP(__STACK_SIZE,
						__STACK_ALIGN_SIZE);
	_libkvmplat_cfg.bstack.start = _libkvmplat_cfg.bstack.end
				       - _libkvmplat_cfg.bstack.len;

	_libkvmplat_cfg.heap.start = _libkvmplat_cfg.pagetable.end;
	_libkvmplat_cfg.heap.end   = _libkvmplat_cfg.bstack.start;
	_libkvmplat_cfg.heap.len   = _libkvmplat_cfg.heap.end
				     - _libkvmplat_cfg.heap.start;

	if (_libkvmplat_cfg.heap.start > _libkvmplat_cfg.heap.end)
		UK_CRASH("Not enough memory, giving up...\n");
}

static void _dtb_get_cmdline(char *cmdline, size_t maxlen)
{
	int fdtchosen, len;
	const char *fdtcmdline;

	/* TODO: Proper error handling */
	fdtchosen = fdt_path_offset(_libkvmplat_cfg.dtb, "/chosen");
	if (!fdtchosen)
		goto enocmdl;
	fdtcmdline = fdt_getprop(_libkvmplat_cfg.dtb, fdtchosen, "bootargs",
				 &len);
	if (!fdtcmdline || (len <= 0))
		goto enocmdl;

	if (likely(maxlen >= (unsigned int)len))
		maxlen = len;
	else
		uk_pr_err("Command line too long, truncated\n");

	strncpy(cmdline, fdtcmdline, maxlen);
	/* ensure null termination */
	cmdline[maxlen - 1] = '\0';

	uk_pr_info("Command line: %s\n", cmdline);
	return;

enocmdl:
	uk_pr_info("No command line found\n");
}

static void _libkvmplat_entry2(void *arg __attribute__((unused)))
{
	ukplat_entry_argp(DECONST(char *, appname),
			  (char *)cmdline, strlen(cmdline));
}

static void _init_dtb_cpu(void)
{
	int fdt_cpu;
	int naddr, nsizei, cell;
	const fdt32_t *prop;
	uint64_t core_id, index;
	int subnode;
	int i;

	/* Init the cpu_possible_map */
	for (i = 0; i < MAXCPU; i++)
		cpu_possible_map[i] = -1;

	/* Search for assigned VM cpus in DTB */
	fdt_cpu = fdt_path_offset(_libkvmplat_cfg.dtb, "/cpus");
	if (fdt_cpu < 0)
		uk_pr_warn("cpus node is not found in device tree\n");

	/* Get address,size cell */
	prop = fdt_getprop(_libkvmplat_cfg.dtb, fdt_cpu, "#address-cells", &cell);
	if (prop != NULL)
		naddr = fdt32_to_cpu(prop[0]);
	if (naddr < 0 || naddr >= FDT_MAX_NCELLS) {
		UK_CRASH("Could not find cpu address!\n");
		return;
	}

	/* Search all the cpu nodes in DTB */
	index = 0;
	fdt_for_each_subnode(subnode, _libkvmplat_cfg.dtb, fdt_cpu) {
		const struct fdt_property *prop;
		int prop_len = 0;

		index++;

		prop = fdt_get_property(_libkvmplat_cfg.dtb, subnode,
						"enable-method", NULL);
		if (!prop || strcmp(prop->data, "psci")) {
			uk_pr_err("Only support psci method!(%s)\n",
					prop->data);
			return;
		}

		prop = fdt_get_property(_libkvmplat_cfg.dtb, subnode,
						"device_type", &prop_len);
		if (!prop)
			continue;
		if (prop_len < 4)
			continue;
		if (strcmp(prop->data, "cpu"))
			continue;

		prop = fdt_get_property(_libkvmplat_cfg.dtb, subnode,
						"reg", &prop_len);
		if (prop == NULL || prop_len <= 0) {
			uk_pr_err("Error when searching reg property\n");
			return;
		}

		core_id = fdt_reg_read_number((const fdt32_t *)prop->data,
						naddr);
		cpu_possible_map[index-1] = core_id;
		mp_ncpus++;
	}
}

static void ndelay(__u64 nsec)
{
       __nsec until = ukplat_monotonic_clock() + nsec;

       while (until > ukplat_monotonic_clock())
               ukplat_lcpu_halt_to(until);
}

static void mdelay(__u64 msec)
{
       ndelay(msec*1000*1000);
}

void release_aps(void)
{
	int i, started;

	/* Only release CPUs if they exist */
	if (mp_ncpus == 1)
		return;

	//TODO: make aps_ready atomic
	aps_ready = 1;

	/* Wake up the other CPUs */
	__asm __volatile(
		"dsb ishst	\n"
		"sev		\n"
		::: "memory");

	uk_pr_info("Release APs...");

	started = 0;
	for (i = 0; i < 20000; i++) {
		if (smp_started) {
			uk_pr_info("done\n");
			return;
		}
		/*
		 * Don't time out while we are making progress. Some large
		 * systems can take a while to start all CPUs.
		 */
		if (smp_cpus > started) {
			i = 0;
			started = smp_cpus;
		}

		uk_pr_info("sleep for a while\n");
		mdelay(1);
	}

	uk_pr_err("APs not started\n");
}

void init_secondary(uint64_t cpu)
{
	struct uk_sched *s = NULL;
	struct uk_alloc *a = NULL;

	uk_pr_info("init secondary cpu=%lu\n", cpu);

	/*
	 * Set the pcpu pointer with a backup in tpidr_el1 to be
	 * loaded when entering the kernel from userland.
	 */
	__asm __volatile(
		"mov x18, %0\n"
		"msr tpidr_el1, %0" :: "r"(pcpup));

	/* Spin until the BSP releases the APs */
	while (!aps_ready)
		__asm __volatile("wfe");
	uk_pr_info("after wfe cpu=%lu\n", cpu);

	smp_cpus += 1;

	if (smp_cpus == mp_ncpus)
		smp_started = 1;
}

void start_cpu(uint64_t target_cpu)
{

	uint32_t pa;
	int err;

	/* Check we are able to start this cpu */
	UK_ASSERT(target_cpu < MAXCPU);

	uk_pr_info("Starting CPU %lu\n", target_cpu);

	/* We are already running on cpu 0 */
	if (target_cpu == (uint64_t)cpu0)
		return;

	pa = ukplat_virt_to_phys(mpentry);
	err = psci_cpu_on(target_cpu, pa);
	if (err != PSCI_RET_SUCCESS) {
		mp_ncpus--;

		/* Notify the user that the CPU failed to start */
		uk_pr_info("Failed to start CPU (%lx)\n", target_cpu);
	}

	uk_pr_info("Starting CPU %lu successfully\n", target_cpu);
}

void _libkvmplat_start(void *dtb_pointer)
{
	_init_dtb(dtb_pointer);
	_libkvmplat_init_console();

	uk_pr_info("Entering from KVM (arm64)...\n");

	/* Get command line from DTB */
	_dtb_get_cmdline(cmdline, sizeof(cmdline));

	/* Get PSCI method from DTB */
	_dtb_get_psci_method();

	/* Initialize memory from DTB */
	_init_dtb_mem();

	/* Initialize interrupt controller */
	intctrl_init();

	uk_pr_info("pagetable start: %p\n",
		   (void *) _libkvmplat_cfg.pagetable.start);
	uk_pr_info("     heap start: %p\n",
		   (void *) _libkvmplat_cfg.heap.start);
	uk_pr_info("      stack top: %p\n",
		   (void *) _libkvmplat_cfg.bstack.start);

	_init_dtb_cpu();

	if (cpu0 < 0) {
		uint64_t mpidr_reg = SYSREG_READ32(mpidr_el1);

		uk_pr_info("get mpidr_el1 0x%lx\n", mpidr_reg);

		if ((mpidr_reg & 0xff00fffffful) == 0)
			cpu0 = 0;
	}

	/*
	 * Switch away from the bootstrap stack as early as possible.
	 */
	uk_pr_info("Switch from bootstrap stack to stack @%p\n",
		   (void *) _libkvmplat_cfg.bstack.end);

	pcpup = &__pcpu[0];

	/*
	 * Set the pcpu pointer with a backup in tpidr_el1 to be
	 * loaded when entering the kernel from userland.
	 */
	__asm __volatile(
		"mov x18, %0\n"
		"msr tpidr_el1, %0" :: "r"(pcpup));

	_libkvmplat_newstack((uint64_t) _libkvmplat_cfg.bstack.end,
				_libkvmplat_entry2, NULL);
}
