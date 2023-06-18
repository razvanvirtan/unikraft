/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <uk/arch/atomic.h>
#include <uk/assert.h>
#include <uk/rwlock.h>
#include <uk/assert.h>
#include <uk/config.h>

void uk_rwlock_init_config(struct uk_rwlock *rwl, unsigned int config_flags)
{
	UK_ASSERT(rwl);

	rwl->nactive = 0;
	rwl->npending_reads = 0;
	rwl->npending_writes = 0;
	rwl->config_flags = config_flags;

	/* TODO: The implementation does not support recursive write locking */
	UK_ASSERT(!uk_rwlock_is_write_recursive(rwl));

	uk_spin_init(&rwl->sl);
	uk_waitq_init(&rwl->shared);
	uk_waitq_init(&rwl->exclusive);
}

void uk_rwlock_rlock(struct uk_rwlock *rwl)
{
	UK_ASSERT(rwl);

	uk_spin_lock(&rwl->sl);
	rwl->npending_reads++;
	uk_spin_unlock(&rwl->sl);

	while (1) {
		uk_spin_lock(&rwl->sl);
		if (rwl->nactive >= 0)
			break;
		uk_spin_unlock(&rwl->sl);
	}

	rwl->nactive++;
	rwl->npending_reads--;
	uk_spin_unlock(&rwl->sl);
}

void uk_rwlock_wlock(struct uk_rwlock *rwl)
{
	UK_ASSERT(rwl);

	uk_spin_lock(&rwl->sl);
	rwl->npending_writes++;

	uk_spin_unlock(&rwl->sl);
	while (1) {
		uk_spin_lock(&rwl->sl);
		if (rwl->nactive == 0)
			break;
		uk_spin_unlock(&rwl->sl);
	}

	UK_ASSERT(rwl->npending_writes > 0);
	UK_ASSERT(rwl->nactive == 0);

	rwl->npending_writes--;
	rwl->nactive = -1;
	uk_spin_unlock(&rwl->sl);
}

void uk_rwlock_runlock(struct uk_rwlock *rwl)
{
	int wake_writers;

	UK_ASSERT(rwl);

	uk_spin_lock(&rwl->sl);
	UK_ASSERT(rwl->nactive > 0);

	/* Remove this thread from the active readers. We wake up a writer if
	 * this was the last reader and there are writers waiting. If there
	 * are no writers pending, readers can always enter. We make sure that
	 * readers are not starving by prioritizing readers on write unlocks.
	 */
	rwl->nactive--;
	wake_writers = (rwl->nactive == 0 && rwl->npending_writes > 0);
	uk_spin_unlock(&rwl->sl);
}

void uk_rwlock_wunlock(struct uk_rwlock *rwl)
{
	int wake_readers;

	UK_ASSERT(rwl);

	uk_spin_lock(&rwl->sl);
	UK_ASSERT(rwl->nactive == -1);

	/* We are the writer. When we unlock we give priority to readers
	 * instead of writers so they do not starve. We avoid starvation of
	 * writers in uk_rwlock_rlock().
	 */
	rwl->nactive = 0;
	wake_readers = (rwl->npending_reads > 0);
	uk_spin_unlock(&rwl->sl);
}

void uk_rwlock_upgrade(struct uk_rwlock *rwl)
{
	UK_ASSERT(rwl);

	uk_spin_lock(&rwl->sl);
	if (rwl->nactive == 1) {
		/* We are the only active reader. Just upgrade to writer */
		rwl->nactive = -1;
	} else {
		/* There are other readers. Wait until these have left */
		UK_ASSERT(rwl->nactive > 1);

		/*
		 * Indicate that we are waiting for write access and remove
		 * this thread from the active readers.
		 */
		rwl->npending_writes++;
		rwl->nactive--;
		
		uk_spin_unlock(&rwl->sl);
		while (1) {
			uk_spin_lock(&rwl->sl);
			if (rwl->nactive == 0)
				break;
			uk_spin_unlock(&rwl->sl);
		}

		UK_ASSERT(rwl->npending_writes > 0);
		UK_ASSERT(rwl->nactive == 0);

		/* We are now the writer. Remove the satisfied request and mark
		 * the lock for write access.
		 */
		rwl->npending_writes--;
		rwl->nactive = -1;
	}
	uk_spin_unlock(&rwl->sl);
}

void uk_rwlock_downgrade(struct uk_rwlock *rwl)
{
	int wake_readers;

	UK_ASSERT(rwl);

	uk_spin_lock(&rwl->sl);
	UK_ASSERT(rwl->nactive == -1);

	/* We are the writer. Downgrade the lock to read access by
	 * transforming to a reader. If there are other readers waiting, wake
	 * them up.
	 */
	rwl->nactive = 1;
	wake_readers = (rwl->npending_reads > 0);
	uk_spin_unlock(&rwl->sl);
}
