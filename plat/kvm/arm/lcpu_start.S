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

 #include <uk/asm.h>

/* void _lcpu_start(arm64_cpu *cpu) */
.text
ENTRY(_lcpu_start)
	/* save x0 */
	mov x9, x0

	/* Disable interrupts */
	msr	daifset, #2

	/* Set the context id */
	msr	contextidr_el1, xzr

	/* Enable the mmu */
	mov x7, #1
	bl	start_mmu

	/* Jump to the virtual address space */
	ldr	x15, =mp_virtdone
	br	x15

mp_virtdone:
	/* restore x0 */
	mov x0, x9

	/* Load the stack pointer */
	ldr x9, [x0, #16]
	mov sp, x9

    /* Increase the number of started cpus */
    ldr x9, =smp_aps_started
    ldr x10, [x9]
    add x10, x10, 1
    str x10, [x9]
 
    /* Spin until the BSP releases the APs */
spin:
    ldr x9, =bspdone
    ldr x10, [x9]
    cmp x10, #0
    b.ne go_to_entry
    wfe
    ldr x11, =spin
    br x11

go_to_entry:
	/* Load the entry address and jump at it */
	ldr x9, [x0, #8]
	br	x9
END(_lcpu_start)
