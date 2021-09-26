/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Simon Kuenzer <simon.kuenzer@neclab.eu>
 *
 *
 * Copyright (c) 2017, NEC Europe Ltd., NEC Corporation. All rights reserved.
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
 */

#ifndef __UKPLAT_LCPU_H__
#define __UKPLAT_LCPU_H__

#include <uk/arch/time.h>
#include <uk/essentials.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * Enables interrupts
 */
void ukplat_lcpu_enable_irq(void);

/**
 * Disables interrupts
 */
void ukplat_lcpu_disable_irq(void);

/**
 * Returns current interrupt flags and disables them
 * @return interrupt flags (Note that the format is unspecified)
 */
unsigned long ukplat_lcpu_save_irqf(void);

/**
 * Loads interrupt flags
 * @param flags interrupt flags (Note that the format is unspecified)
 */
void ukplat_lcpu_restore_irqf(unsigned long flags);

/**
 * Checks if interrupts are disabled
 * @return non-zero value if interrupts are disabled
 */
int ukplat_lcpu_irqs_disabled(void);

void ukplat_lcpu_irqs_handle_pending(void);

/**
 * Halts the current logical CPU execution
 */
void ukplat_lcpu_halt(void);

/**
 * Halts the current logical CPU execution
 * Execution is returned when an interrupt/signal arrived or
 * the specified deadline expired
 * @param until deadline in nanoseconds
 */
void ukplat_lcpu_halt_to(__snsec until);

/**
 * Halts the current logical CPU execution
 * Execution is returned when an interrupt/signal arrived
 */
void ukplat_lcpu_halt_irq(void);

#ifdef CONFIG_HAVE_SMP
#include <uk/list.h>

typedef void (*ukplat_lcpu_entry_t)(void) __noreturn;
typedef __u32 __lcpuid;

struct ukplat_lcpu_func {
	struct uk_list_head lentry;
	void (*fn)(struct __regs *regs, struct ukplat_lcpu_func *fn);
	void *user;
};

/**
 * Starts multiple logical CPUs
 * @param lcpuid array with the ids of the cores that are to be started
 * @param sp array of stack pointers - provide a stack for each core
 * @param entry array of function pointers - the entry for each core
 * @param num number of cores that are to be started
 * @return number of cores that have started
 */
int ukplat_lcpu_start(__lcpuid lcpuid[], void *sp[],
		      ukplat_lcpu_entry_t entry[], int num);

/**
 * Return the (physical) ID of the current logical CPU
 */
__lcpuid ukplat_lcpu_id(void);

/**
 * Return the number of logical CPUs present on the system
 */
__lcpuid ukplat_lcpu_count(void);

/**
 * Return whether the current logical CPU is the Bootstrapping one
 */
int ukplat_lcpu_is_bsp(void);

/**
 * Wait for the given lcpus to enter the "idle" state, or until the timeout
 * expires. If lcpuid[] is NULL, wait for all the lcpus, except the current
 * one.
 * @return 1, if the timeout expired, 0 otherwise
 */

int ukplat_lcpu_wait(__lcpuid lcpuid[], int num, __nsec timeout);

int ukplat_lcpu_run(__lcpuid lcpuid[], struct ukplat_lcpu_func *fn, int num,
		    int flags);

int ukplat_lcpu_wakeup(__lcpuid lcpuid[], int num);

#else
#define ukplat_lcpu_id() (0)
#define ukplat_lcpu_count() (1)
#endif /* CONFIG_HAVE_SMP */

#ifdef __cplusplus
}
#endif

#endif /* __UKPLAT_LCPU_H__ */
