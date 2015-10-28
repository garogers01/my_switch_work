/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <rte_mempool.h>
#include <rte_malloc.h>

#include "rte_mempool_internal.h"

struct rte_mempool_common_ring
{
	struct rte_ring *r;

#ifdef RTE_LIBRTE_MEMPOOL_DEBUG
#endif
};

static int
common_ring_mp_put(rte_mempool_rt_pool p, void * const *obj_table, unsigned n)
{
	return rte_ring_mp_enqueue_bulk(((struct rte_mempool_common_ring *)p)->r,
			obj_table, n);
}

static int
common_ring_sp_put(rte_mempool_rt_pool p, void * const *obj_table, unsigned n)
{
	return rte_ring_sp_enqueue_bulk(((struct rte_mempool_common_ring *)p)->r,
			obj_table, n);
}

static int
common_ring_mc_get(rte_mempool_rt_pool p, void **obj_table, unsigned n)
{
	return rte_ring_mc_dequeue_bulk(((struct rte_mempool_common_ring *)p)->r,
			obj_table, n);
}

static int
common_ring_sc_get(rte_mempool_rt_pool p, void **obj_table, unsigned n)
{
	return rte_ring_sc_dequeue_bulk(((struct rte_mempool_common_ring *)p)->r,
			obj_table, n);
}

static unsigned
common_ring_get_count(rte_mempool_rt_pool p)
{
	return rte_ring_count(((struct rte_mempool_common_ring *)p)->r);
}


rte_mempool_rt_pool rte_mempool_common_ring_alloc(struct rte_mempool *mp,
		const char *name, unsigned n, int socket_id, unsigned flags)
{
	struct rte_mempool_common_ring * cr;
	char rg_name[RTE_RING_NAMESIZE];
	int rg_flags = 0;

	if (flags & MEMPOOL_F_SP_PUT)
		rg_flags |= RING_F_SP_ENQ;
	if (flags & MEMPOOL_F_SC_GET)
		rg_flags |= RING_F_SC_DEQ;

	/* Allocate our local memory structure */
	snprintf(rg_name, sizeof(rg_name), "%s-common-ring", name);
	cr = rte_zmalloc(rg_name, sizeof(*cr), 0);
	if (cr == NULL) {
		RTE_LOG(ERR, MEMPOOL, "Cannot allocate tailq entry!\n");
		return NULL;
	}

	/* allocate the ring that will be used to store objects */
	/* Ring functions will return appropriate errors if we are
	 * running as a secondary process etc., so no checks made
	 * in this function for that condition */
	snprintf(rg_name, sizeof(rg_name), RTE_MEMPOOL_MZ_FORMAT, name);
	cr->r = rte_ring_create(rg_name, rte_align32pow2(n+1), socket_id, rg_flags);
	if (cr->r == NULL)
		return NULL;

	mp->rt_pool = (rte_mempool_rt_pool)cr;

	/* Setup the mempool get/put functions */
	mp->put_idx = flags & MEMPOOL_F_SP_PUT ? RTE_MEMPOOL_COMMON_RING_SPPUT :
			RTE_MEMPOOL_COMMON_RING_MPPUT;
	mp->get_idx = flags & MEMPOOL_F_SC_GET ? RTE_MEMPOOL_COMMON_RING_SCGET :
			RTE_MEMPOOL_COMMON_RING_MCGET;
	mp->get_count_idx = RTE_MEMPOOL_COMMON_RING_GETCOUNT;

	return (rte_mempool_rt_pool) cr;
}


struct rte_mempool_common_stack
{
	/* Spinlock to protect access */
	rte_spinlock_t sl;

	uint32_t size;
	uint32_t len;
	void *objs[];

#ifdef RTE_LIBRTE_MEMPOOL_DEBUG
#endif
};


static int common_stack_put(rte_mempool_rt_pool p, void * const *obj_table,
		unsigned n)
{
	struct rte_mempool_common_stack * s = (struct rte_mempool_common_stack *)p;
	void **cache_objs;
	unsigned index;

	/* Acquire lock */
	rte_spinlock_lock(&s->sl);
	cache_objs = &s->objs[s->len];

	/* Is there sufficient space in the stack ? */
	if((s->len + n) > s->size) {
		rte_spinlock_unlock(&s->sl);
		return -ENOENT;
	}

	/* Add elements back into the cache */
	for (index = 0; index < n; ++index, obj_table++)
		cache_objs[index] = *obj_table;

	s->len += n;

	rte_spinlock_unlock(&s->sl);
	return 0;
}

static int common_stack_get(rte_mempool_rt_pool p, void **obj_table,
		unsigned n)
{
	struct rte_mempool_common_stack * s = (struct rte_mempool_common_stack *)p;
	void **cache_objs;
	unsigned index, len;

	if(unlikely(n > s->len))
		return -ENOENT;

	/* Acquire lock */
	rte_spinlock_lock(&s->sl);
	cache_objs = s->objs;

	for (index = 0, len = s->len - 1; index < n; ++index, len--, obj_table++)
		*obj_table = cache_objs[len];

	s->len -= n;
	rte_spinlock_unlock(&s->sl);
	return n;
}

#if 0
#pragma GCC diagnostic ignored "-Wclobbered"
static int common_stack_get_tsx(rte_mempool_rt_pool p, void **obj_table,
		unsigned n)
{
	struct rte_mempool_common_stack * s = (struct rte_mempool_common_stack *)p;
	void **cache_objs;
	unsigned index, len;

	__transaction_atomic {
		cache_objs = s->objs;

		for (index = 0, len = s->len - 1; index < n; ++index, len--, obj_table++)
			*obj_table = cache_objs[len];

		s->len -= n;
	}

	return n;
}
#endif

static unsigned common_stack_get_count(rte_mempool_rt_pool p)
{
	struct rte_mempool_common_stack * s = (struct rte_mempool_common_stack *)p;
	return s->len;
}

rte_mempool_rt_pool rte_mempool_common_stack_alloc(struct rte_mempool *mp,
		const char *name, unsigned n, int socket_id, unsigned flags)
{
	struct rte_mempool_common_stack *s;
	char stack_name[RTE_RING_NAMESIZE];

	int size = sizeof(*s) + (n+16)*sizeof(void*);

	flags = flags;

	/* Allocate our local memory structure */
	snprintf(stack_name, sizeof(stack_name), "%s-common-stack", name);
	s = rte_zmalloc_socket(stack_name, size, 64, socket_id);
	if (s == NULL) {
		RTE_LOG(ERR, MEMPOOL, "Cannot allocate stack!\n");
		return NULL;
	}

	/* And the spinlock we use to protect access */
	rte_spinlock_init(&s->sl);
	s->size = n;

	mp->rt_pool = (rte_mempool_rt_pool) s;

	/* Setup the mempool get/put functions */
	mp->put_idx = RTE_MEMPOOL_COMMON_STACK_PUT;
	mp->get_idx = RTE_MEMPOOL_COMMON_STACK_GET;
	mp->get_count_idx = RTE_MEMPOOL_COMMON_STACK_GETCOUNT;

	return (rte_mempool_rt_pool) s;
}

/*
 * Indirect jump table to support primary and secondary process external
 * memory pools
 *
 */
struct rte_mempool_jump_table mempool_jump_table = {
	.num_put = RTE_MEMPOOL_PUT_MAX_IDX,

	.put[RTE_MEMPOOL_COMMON_RING_MPPUT] = common_ring_mp_put,
	.put[RTE_MEMPOOL_COMMON_RING_SPPUT] = common_ring_sp_put,
	.put[RTE_MEMPOOL_COMMON_STACK_PUT] = common_stack_put,

	.num_get = RTE_MEMPOOL_GET_MAX_IDX,

	.get[RTE_MEMPOOL_COMMON_RING_MCGET] = common_ring_mc_get,
	.get[RTE_MEMPOOL_COMMON_RING_SCGET] = common_ring_sc_get,
	.get[RTE_MEMPOOL_COMMON_STACK_GET] = common_stack_get,

	.num_get_count = RTE_MEMPOOL_GET_COUNT_MAX_IDX,

	.get_count[RTE_MEMPOOL_COMMON_RING_GETCOUNT] = common_ring_get_count,
	.get_count[RTE_MEMPOOL_COMMON_STACK_GETCOUNT] = common_stack_get_count,
};


int
rte_mempool_ext_get_bulk(struct rte_mempool *mp, void **obj_table, unsigned n)
{
	return (*mempool_jump_table.get[mp->get_idx])(mp->rt_pool, obj_table, n);
}

int
rte_mempool_ext_put_bulk(struct rte_mempool *mp, void * const *obj_table,
		unsigned n)
{
	return (*mempool_jump_table.put[mp->put_idx])(mp->rt_pool, obj_table, n);
}

int
rte_mempool_ext_get_count(const struct rte_mempool *mp)
{
	return (*mempool_jump_table.get_count[mp->get_count_idx])(mp->rt_pool);
}
