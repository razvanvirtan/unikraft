/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Răzvan Vîrtan <virtanrazvan@gmail.com>
 *          Justin He     <justin.he@arm.com>
 *
 * Copyright (c) 2021, Arm Ltd., University Politehnica of Bucharest. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <uk/config.h>
#include <libfdt.h>
#include <uk/plat/common/sections.h>
#include <kvm/console.h>
#include <kvm/config.h>
#include <uk/assert.h>
#include <kvm-arm/mm.h>
#include <kvm-arm/smp.h>
#include <kvm/intctrl.h>
#include <arm/cpu.h>
#include <uk/arch/limits.h>
#include <stdbool.h>
#include <uk/plat/bootstrap.h>
#include <uk/sched.h>
#include <uk/plat/time.h>

#include <uk/plat/memory.h>
#include <uk/plat/io.h>
#include <uk/plat/lcpu.h>
#include <arm/psci.h>
#include <ofw/fdt.h>

static __u64 smp_numcores;
__u64 bspdone;
__u64 smp_aps_started; // APs that are started and reached their entry function
__u64 smp_aps_running; // APs that are started and aware of bspdone = 1

struct arm64_cpu {
    uint64_t id;
	ukplat_lcpu_entry_t entry;
    void *stackp;
    struct uk_list_head fnlist;
};
const size_t arm64_cpu_size = sizeof(struct arm64_cpu);

struct arm64_cpu cpus[CONFIG_MAX_CPUS];

static __lcpuid bspid;

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

int smp_init(void)
{
	int fdt_cpu;
	int naddr, cell;
	const fdt32_t *prop;
	uint64_t index;
	int subnode;

	/* Init the cpu_possible_map */
	for (index = 0; index < CONFIG_MAX_CPUS; index++)
        cpus[index].id = -1;

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
		return -1;
	}

	/* Search all the cpu nodes in DTB */
	index = 0;
	fdt_for_each_subnode(subnode, _libkvmplat_cfg.dtb, fdt_cpu) {
		const struct fdt_property *prop;
		int prop_len = 0;

		prop = fdt_get_property(_libkvmplat_cfg.dtb, subnode,
						"enable-method", NULL);
        if (!prop) {
            uk_pr_err("No method found!\n");
            return -1;
        }
        else if (strcmp(prop->data, "psci")) {
            uk_pr_err("Only support psci method!(%s)\n",
				prop->data);
			return -1;
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
			return -1;
		}

		cpus[index].id = fdt_reg_read_number((const fdt32_t *)prop->data,
						naddr);
        uk_pr_info("Initialized core %d\n", cpus[index].id);
		index++;
	}
	smp_numcores = index;

    bspid = ukplat_lcpu_id();
	uk_pr_info("Bootstrapping processor has the ID %d\n", bspid);

    
    return 0;
}

int psci_cpu_on(uint64_t cpu, uint64_t entry) {
	/* PSCI v0.1 and v0.2 both support cpu_on. the parameter @cpu after
	 * @entry will be passed as x0 for entry
	 */
	return smcc_psci_hvc_call(PSCI_FNID_CPU_ON, ((struct arm64_cpu*) cpu)->id, entry, cpu);
}


int ukplat_lcpu_start(__lcpuid lcpuid[], void *sp[],
				ukplat_lcpu_entry_t entry[], int num) {
	int i, err;
	uint64_t entry_address, old_aps_running;
	__lcpuid logic_id;

	/* start the APs */
	smp_aps_started = 0;
	bspdone = 0;
	for (i = 0; i < num; i++) {
		logic_id = (lcpuid) ? lcpuid[i] : (__lcpuid) i;

		if (cpus[logic_id].id == bspid)
			continue;

		if (logic_id > smp_numcores)
			continue;
		
		cpus[logic_id].entry = entry[i];
		cpus[logic_id].stackp = sp[i];
		UK_INIT_LIST_HEAD(&cpus[logic_id].fnlist);

		entry_address = ukplat_virt_to_phys(_lcpu_start);
		err = psci_cpu_on(&(cpus[logic_id]), entry_address);
		if (err != PSCI_RET_SUCCESS)
			uk_pr_info("Failed to start core (%lu)\n", cpus[logic_id].id);
		else
			uk_pr_info("Started core (%lu) successfully\n", cpus[logic_id].id);
	}
	
	/* Wake up secondary CPUs */
	if (smp_numcores == 1)
		return 0;

	smp_aps_running = 0;
	old_aps_running = 0;

	bspdone = 1;
	__asm __volatile(
		"dsb ishst	\n"
		"sev		\n"
		::: "memory");
	uk_pr_info("Wake up APs...\n");

	/* Wait until all cpus are aware of bspdone */
	for (i = 0; i < 20000; i++) {
		if (smp_aps_running == smp_aps_started) {
			uk_pr_info("Finished APs boot\n");
			return 0;
		}
		/*
		 * Don't time out while we are making progress. Some large
		 * systems can take a while to start all CPUs.
		 */
		if (smp_aps_running > old_aps_running) {
			i = 0;
			old_aps_running = smp_aps_running;
		}

		uk_pr_info("sleep for a while\n");
		mdelay(1);
	}
	return -1;
}

void __noreturn _lcpu_entry_default(void) {
	smp_aps_running += 1;

	struct uk_sched *s = NULL;
	struct uk_alloc *a = NULL;

	uk_pr_info("Initializing cpu....\n");

	a = ukplat_memallocator_get();
	if (a == NULL)
		UK_CRASH("memallocator is not initialized\n");

	s = uk_sched_default_init(a);
	if (unlikely(!s))
		UK_CRASH("Could not initialize the scheduler in APs\n");

	intctrl_init();

	/* Enable interrupts before starting the application */
	ukplat_lcpu_enable_irq();

	/* Enter the scheduler */
	uk_sched_start(s);

	UK_CRASH("scheduler returned us to init secondary\n");
	/* NOTREACHED */
}

__lcpuid ukplat_lcpu_id(void) {
    uint64_t mpidr_reg;

    mpidr_reg = SYSREG_READ32(mpidr_el1);
    uk_pr_info("get mpidr_el1 0x%lx\n", mpidr_reg);

    /* return the affinity bits for the current core */
    return mpidr_reg & 0xff00fffffful;
}
