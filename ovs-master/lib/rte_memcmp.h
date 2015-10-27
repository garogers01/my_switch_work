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

#ifndef _RTE_MEMCMP_X86_64_H_
#define _RTE_MEMCMP_X86_64_H_

/**
 * @file
 *
 * Functions for SSE/AVX/AVX2 implementation of memcmp().
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <rte_vect.h>
#include <rte_branch_prediction.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Compare bytes between two locations. The locations must not overlap.
 *
 * @note This is implemented as a macro, so it's address should not be taken
 * and care is needed as parameter expressions may be evaluated multiple times.
 *
 * @param src_1
 *   Pointer to the first source of the data.
 * @param src_2
 *   Pointer to the second source of the data.
 * @param n
 *   Number of bytes to compare.
 * @return
 *   true if equal otherwise false.
 */
static inline int
rte_memcmp(const void *src_1, const void *src,
		size_t n) __attribute__((always_inline));

/**
 * Compare 16 bytes between two locations.
 * locations should not overlap.
 */
static inline int
rte_cmp16(const void *src_1, const void *src_2)
{
	__m128i xmm0, xmm1, xmm2;
int ret = 0;

	xmm0 = _mm_lddqu_si128((const __m128i *)src_1);
	xmm1 = _mm_lddqu_si128((const __m128i *)src_2);
	xmm2 = _mm_xor_si128(xmm0, xmm1);

	if (unlikely(!_mm_testz_si128(xmm2, xmm2))) {

		const uint64_t mm11 = *(const uint64_t *)src_1;
		const uint64_t mm12 = *((const uint64_t *)src_1 + 1);

		const uint64_t mm21 = *(const uint64_t *)src_2;
		const uint64_t mm22 = *((const uint64_t *)src_2 + 1);

		if (mm11 == mm21)
			(mm12 < mm22) ? (ret = -1) : (ret = 1);
		else
			(mm11 < mm21) ? (ret = -1) : (ret = 1);
	}

	return ret;
}

/**
 * AVX2 implementation below
 */
#ifdef RTE_MACHINE_CPUFLAG_AVX2

/**
 * Compare 32 bytes between two locations.
 * Locations should not overlap.
 */
static inline int
rte_cmp32(const void *src_1, const void *src_2)
{
	const __m128i* src1 = (const __m128i*)src_1;
	const __m128i* src2 = (const __m128i*)src_2;

	__m128i mm11 = _mm_lddqu_si128(src1);
	__m128i mm12 = _mm_lddqu_si128(src1 + 1);
	__m128i mm21 = _mm_lddqu_si128(src2);
	__m128i mm22 = _mm_lddqu_si128(src2 + 1);

	__m128i mm1 = _mm_xor_si128(mm11, mm21);
	__m128i mm2 = _mm_xor_si128(mm12, mm22);
	__m128i mm = _mm_or_si128(mm1, mm2);

	if (unlikely(!_mm_testz_si128(mm, mm))) {

		/*
		 * Find out which of the two 16-byte blocks
		 * are different.
		 */
		if (_mm_testz_si128(mm1, mm1)) {
			mm11 = mm12;
			mm21 = mm22;
			mm1 = mm2;
		}

		// Produce the comparison result
		__m128i mm_cmp = _mm_cmpgt_epi8(mm21, mm11);
		__m128i mm_rcmp = _mm_cmpgt_epi8(mm11, mm21);
		mm_cmp = _mm_xor_si128(mm1, mm_cmp);
		mm_rcmp = _mm_xor_si128(mm1, mm_rcmp);

		uint32_t cmp = _mm_movemask_epi8(mm_cmp);
		uint32_t rcmp = _mm_movemask_epi8(mm_rcmp);
		cmp = (cmp - 1u) ^ cmp;
		rcmp = (rcmp - 1u) ^ rcmp;
		return (int32_t)cmp - (int32_t)rcmp;
	}

	return 0;
}

static inline int
rte_cmp64 (const void* src_1, const void* src_2)
{
	const __m256i* src1 = (const __m256i*)src_1;
	const __m256i* src2 = (const __m256i*)src_2;

	__m256i mm11 = _mm256_lddqu_si256(src1);
	__m256i mm12 = _mm256_lddqu_si256(src1 + 1);
	__m256i mm21 = _mm256_lddqu_si256(src2);
	__m256i mm22 = _mm256_lddqu_si256(src2 + 1);

	__m256i mm1 = _mm256_xor_si256(mm11, mm21);
	__m256i mm2 = _mm256_xor_si256(mm12, mm22);
	__m256i mm = _mm256_or_si256(mm1, mm2);

	if (unlikely(!_mm256_testz_si256(mm, mm))) {
		/*
		 * Find out which of the two 32-byte blocks
		 * are different.
		 */
		if (_mm256_testz_si256(mm1, mm1)) {
			mm11 = mm12;
			mm21 = mm22;
			mm1 = mm2;
		}

		// Produce the comparison result
		__m256i mm_cmp = _mm256_cmpgt_epi8(mm21, mm11);
		__m256i mm_rcmp = _mm256_cmpgt_epi8(mm11, mm21);
		mm_cmp = _mm256_xor_si256(mm1, mm_cmp);
		mm_rcmp = _mm256_xor_si256(mm1, mm_rcmp);

		uint32_t cmp = _mm256_movemask_epi8(mm_cmp);
		uint32_t rcmp = _mm256_movemask_epi8(mm_rcmp);
		cmp = (cmp - 1u) ^ cmp;
		rcmp = (rcmp - 1u) ^ rcmp;
		return (int32_t)cmp - (int32_t)rcmp;
	}

	return 0;
}

static inline int
rte_cmp128 (const void* src_1, const void* src_2)
{
	const __m256i* src1 = (const __m256i*)src_1;
	const __m256i* src2 = (const __m256i*)src_2;
	const size_t n = 2;
	size_t i;

	for (i = 0; i < n; ++i, src1 += 2, src2 += 2) {
		__m256i mm11 = _mm256_lddqu_si256(src1);
		__m256i mm12 = _mm256_lddqu_si256(src1 + 1);
		__m256i mm21 = _mm256_lddqu_si256(src2);
		__m256i mm22 = _mm256_lddqu_si256(src2 + 1);

		__m256i mm1 = _mm256_xor_si256(mm11, mm21);
		__m256i mm2 = _mm256_xor_si256(mm12, mm22);
		__m256i mm = _mm256_or_si256(mm1, mm2);

		if (unlikely(!_mm256_testz_si256(mm, mm))) {
			/*
			 * Find out which of the two 32-byte blocks
			 * are different.
			 */
			if (_mm256_testz_si256(mm1, mm1)) {
				mm11 = mm12;
				mm21 = mm22;
				mm1 = mm2;
			}

			// Produce the comparison result
			__m256i mm_cmp = _mm256_cmpgt_epi8(mm21, mm11);
			__m256i mm_rcmp = _mm256_cmpgt_epi8(mm11, mm21);
			mm_cmp = _mm256_xor_si256(mm1, mm_cmp);
			mm_rcmp = _mm256_xor_si256(mm1, mm_rcmp);

			uint32_t cmp = _mm256_movemask_epi8(mm_cmp);
			uint32_t rcmp = _mm256_movemask_epi8(mm_rcmp);
			cmp = (cmp - 1u) ^ cmp;
			rcmp = (rcmp - 1u) ^ rcmp;
			return (int32_t)cmp - (int32_t)rcmp;
		}
	}

	return 0;
}
#else /* RTE_MACHINE_CPUFLAG_AVX2 */

/**
 * Compare 32 bytes between two locations.
 * Locations should not overlap.
 */
static inline int
rte_cmp32(const void *src_1, const void *src_2)
{
	int ret;

	ret = rte_cmp16((const uint8_t *)src_1 + 0 * 16,
				(const uint8_t *)src_2 + 0 * 16);

	if (likely(ret == 0))
		return rte_cmp16((const uint8_t *)src_1 + 1 * 16,
				(const uint8_t *)src_2 + 1 * 16);

	return ret;
}

/**
 * Compare 64 bytes between two locations.
 * Locations should not overlap.
 */
static inline int
rte_cmp64(const void *src_1, const void *src_2)
{
	int ret;

	ret = rte_cmp32((const uint8_t *)src_1 + 0 * 32,
				(const uint8_t *)src_2 + 0 * 32);

	if (likely(ret == 0))
		return rte_cmp32((const uint8_t *)src_1 + 1 * 32,
				(const uint8_t *)src_2 + 1 * 32);

	return ret;
}

/**
 * Compare 128 bytes between two locations.
 * Locations should not overlap.
 */
static inline int
rte_cmp128(const void *src_1, const void *src_2)
{
	int ret;

	ret = rte_cmp64((const uint8_t *)src_1 + 0 * 64,
			(const uint8_t *)src_2 + 0 * 64);

	if (likely(ret == 0))
		return rte_cmp64((const uint8_t *)src_1 + 1 * 64,
				(const uint8_t *)src_2 + 1 * 64);

	return ret;
}

#endif /* RTE_MACHINE_CPUFLAG_AVX2 */


/**
 * Compare 48 bytes between two locations.
 * Locations should not overlap.
 */
static inline int
rte_cmp48(const void *src_1, const void *src_2)
{
	int ret;

	ret = rte_cmp32((const uint8_t *)src_1 + 0 * 32,
			(const uint8_t *)src_2 + 0 * 32);

	if (likely(ret == 0))
		return rte_cmp16((const uint8_t *)src_1 + 1 * 32,
				(const uint8_t *)src_2 + 1 * 32);
	return ret;
}

static inline int
rte_memcmp_remainder(const uint8_t *src_1u, const uint8_t *src_2u, size_t n)
{
	int ret = 1;

	/**
	 * Compare less than 16 bytes
	 */
	if (n & 0x08) {
		ret = (*(const uint64_t *)src_1u ==
				*(const uint64_t *)src_2u);
		if (likely(ret == 1)) {
			n -= 0x8;
			src_1u += 0x8;
			src_2u += 0x8;
		} else {
			goto exit;
		}
	}

	if (n & 0x04) {
		ret = (*(const uint32_t *)src_1u ==
				*(const uint32_t *)src_2u);
		if (likely(ret == 1)) {
			n -= 0x4;
			src_1u += 0x4;
			src_2u += 0x4;
		} else {
			goto exit;
		}
	}

	if (n & 0x02) {
		ret = (*(const uint16_t *)src_1u ==
				*(const const uint16_t *)src_2u);

		if (likely(ret == 1)) {
			n -= 0x2;
			src_1u += 0x2;
			src_2u += 0x2;
		} else {
			goto exit;
		}
	}

	if (n & 0x01) {
		ret = (*(const uint8_t *)src_1u ==
				*(const uint8_t *)src_2u);
		if (likely(ret == 1)) {
			return 0;
		} else {
			goto exit;
		}
	}

	return !ret;
exit:

	return src_1u < src_2u ? -1 : 1;
}

static inline int
rte_memcmp(const void *_src_1, const void *_src_2, size_t n)
{
	const uint8_t *src_1 = (const uint8_t *)_src_1;
	const uint8_t *src_2 = (const uint8_t *)_src_2;
	int ret = 0;

	if (n & 0x80)
		return rte_cmp128(src_1, src_2);

	if (n & 0x40)
		return rte_cmp64(src_1, src_2);

	if (n & 0x20) {
		ret = rte_cmp32(src_1, src_2);
		n -= 0x20;
		src_1 += 0x20;
		src_2 += 0x20;
	}

	if ((n & 0x10) && likely(ret == 0)) {
		ret = rte_cmp16(src_1, src_2);
		n -= 0x10;
		src_1 += 0x10;
		src_2 += 0x10;
	}

	if (n && likely(ret == 0))
		ret = rte_memcmp_remainder(src_1, src_2, n);

	return ret;
}

#ifdef __cplusplus
}
#endif

#endif /* _RTE_MEMCMP_X86_64_H_ */
