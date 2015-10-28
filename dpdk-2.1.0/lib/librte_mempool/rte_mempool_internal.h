/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

#ifndef _RTE_MEMPOOL_INTERNAL_H_
#define _RTE_MEMPOOL_INTERNAL_H_


/* Handler functions for external mempool support */
typedef int (*rte_mempool_put_t)(rte_mempool_rt_pool p,
		void * const *obj_table, unsigned n);
typedef int (*rte_mempool_get_t)(rte_mempool_rt_pool p, void **obj_table,
		unsigned n);
typedef unsigned (*rte_mempool_get_count)(rte_mempool_rt_pool p);

/* Functions that implement different behaviour for the common pool */
rte_mempool_rt_pool rte_mempool_common_ring_alloc(struct rte_mempool *mp,
		const char *name, unsigned n, int socket_id, unsigned flags);
rte_mempool_rt_pool rte_mempool_common_stack_alloc(struct rte_mempool *mp,
		const char *name, unsigned n, int socket_id, unsigned flags);


#define RTE_MEMPOOL_MAX_JUMP_IDX 	8

enum rte_mempool_put_jump_idx {
	RTE_MEMPOOL_COMMON_RING_MPPUT = 0,
	RTE_MEMPOOL_COMMON_RING_SPPUT,
	RTE_MEMPOOL_COMMON_STACK_PUT,

	/* Add new indices above this line */
	RTE_MEMPOOL_PUT_MAX_IDX,
};

enum rte_mempool_get_jump_idx {
	RTE_MEMPOOL_COMMON_RING_MCGET = 0,
	RTE_MEMPOOL_COMMON_RING_SCGET,
	RTE_MEMPOOL_COMMON_STACK_GET,

	/* Add new indices above this line */
	RTE_MEMPOOL_GET_MAX_IDX,
};

enum rte_mempool_get_count_idx {
	RTE_MEMPOOL_COMMON_RING_GETCOUNT = 0,
	RTE_MEMPOOL_COMMON_STACK_GETCOUNT,

	/* Add new indices above this line */
	RTE_MEMPOOL_GET_COUNT_MAX_IDX,
};

#include <rte_spinlock.h>
struct rte_mempool_jump_table {
	rte_spinlock_t sl;			/**< Spinlock for add/delete. */
	uint32_t num_put;			/**< Number of entries that are valid. */
	uint32_t num_get;
	uint32_t num_get_count;

	rte_mempool_put_t put[RTE_MEMPOOL_MAX_JUMP_IDX] __rte_cache_aligned;
	rte_mempool_get_t get[RTE_MEMPOOL_MAX_JUMP_IDX] __rte_cache_aligned;
	rte_mempool_get_count get_count[RTE_MEMPOOL_MAX_JUMP_IDX] __rte_cache_aligned;
};

#endif
