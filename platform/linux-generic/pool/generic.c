/* Copyright (c) 2013, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include "config.h"

#include <odp/api/pool.h>
#include <odp/api/shared_memory.h>
#include <odp/api/align.h>
#include <odp/api/ticketlock.h>
#include <odp/api/system_info.h>

#include <odp_pool_internal.h>
#include <odp_internal.h>
#include <odp_buffer_inlines.h>
#include <odp_packet_internal.h>
#include <odp_config_internal.h>
#include <odp_debug_internal.h>
#include <odp_ring_internal.h>

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include <odp/api/plat/ticketlock_inlines.h>
#define LOCK(a)      _odp_ticketlock_lock(a)
#define UNLOCK(a)    _odp_ticketlock_unlock(a)
#define LOCK_INIT(a) odp_ticketlock_init(a)

#define RING_SIZE_MIN  (2 * CACHE_BURST)

/* Make sure packet buffers don't cross huge page boundaries starting from this
 * page size. 2MB is typically the smallest used huge page size. */
#define FIRST_HP_SIZE (2 * 1024 * 1024)

/* Define a practical limit for contiguous memory allocations */
#define MAX_SIZE   (10 * 1024 * 1024)

ODP_STATIC_ASSERT(CONFIG_POOL_CACHE_SIZE > (2 * CACHE_BURST),
		  "cache_burst_size_too_large_compared_to_cache_size");

ODP_STATIC_ASSERT(CONFIG_PACKET_SEG_LEN_MIN >= 256,
		  "ODP Segment size must be a minimum of 256 bytes");

ODP_STATIC_ASSERT(CONFIG_PACKET_SEG_SIZE < 0xffff,
		  "Segment size must be less than 64k (16 bit offsets)");

pool_table_t *pool_tbl;
__thread pool_local_t local;

static int generic_pool_init_global(void)
{
	uint32_t i;
	odp_shm_t shm;

	shm = odp_shm_reserve("_odp_pool_table",
			      sizeof(pool_table_t),
			      ODP_CACHE_LINE_SIZE, 0);

	pool_tbl = odp_shm_addr(shm);

	if (pool_tbl == NULL)
		return -1;

	memset(pool_tbl, 0, sizeof(pool_table_t));
	pool_tbl->shm = shm;

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool_t *pool = pool_entry(i);

		LOCK_INIT(&pool->lock);
		pool->pool_hdl = pool_index_to_handle(i);
		pool->pool_idx = i;
	}

	ODP_DBG("\nPool init global\n");
	ODP_DBG("  odp_buffer_hdr_t size %zu\n", sizeof(odp_buffer_hdr_t));
	ODP_DBG("  odp_packet_hdr_t size %zu\n", sizeof(odp_packet_hdr_t));
	ODP_DBG("\n");
	return 0;
}

static int generic_pool_term_global(void)
{
	int i;
	pool_t *pool;
	int ret = 0;
	int rc = 0;

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool = pool_entry(i);

		LOCK(&pool->lock);
		if (pool->reserved) {
			ODP_ERR("Not destroyed pool: %s\n", pool->name);
			rc = -1;
		}
		UNLOCK(&pool->lock);
	}

	ret = odp_shm_free(pool_tbl->shm);
	if (ret < 0) {
		ODP_ERR("shm free failed");
		rc = -1;
	}

	return rc;
}

static int generic_pool_init_local(void)
{
	pool_t *pool;
	int i;
	int thr_id = odp_thread_id();

	memset(&local, 0, sizeof(pool_local_t));

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool           = pool_entry(i);
		local.cache[i] = &pool->local_cache[thr_id];
		local.cache[i]->num = 0;
	}

	local.thr_id = thr_id;
	return 0;
}

static void flush_cache(pool_cache_t *cache, pool_t *pool)
{
	ring_t *ring;
	uint32_t mask;
	uint32_t cache_num, i;

	ring = &pool->ring->hdr;
	mask = pool->ring_mask;
	cache_num = cache->num;

	for (i = 0; i < cache_num; i++)
		ring_enq(ring, mask, cache->buf_index[i]);

	cache->num = 0;
}

static int generic_pool_term_local(void)
{
	int i;

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool_t *pool = pool_entry(i);

		flush_cache(local.cache[i], pool);
	}

	return 0;
}

static pool_t *reserve_pool(uint32_t ring_size)
{
	int i;
	pool_t *pool;
	char ring_name[ODP_POOL_NAME_LEN];
	uint32_t ring_shm_size;

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool = pool_entry(i);

		LOCK(&pool->lock);
		if (pool->reserved == 0) {
			pool->reserved = 1;
			UNLOCK(&pool->lock);
			snprintf(ring_name, ODP_POOL_NAME_LEN,
				 "pool_ring_%d", i);
			ring_shm_size = sizeof(pool_ring_t) +
					sizeof(pool->ring->buf[0]) * ring_size;
			pool->ring_shm =
				odp_shm_reserve(ring_name,
						ring_shm_size,
						ODP_CACHE_LINE_SIZE, 0);
			if (odp_unlikely(pool->ring_shm == ODP_SHM_INVALID)) {
				ODP_ERR("Unable to alloc pool ring %d\n", i);
				LOCK(&pool->lock);
				pool->reserved = 0;
				UNLOCK(&pool->lock);
				break;
			}
			pool->ring = odp_shm_addr(pool->ring_shm);
			return pool;
		}
		UNLOCK(&pool->lock);
	}

	return NULL;
}

static void init_buffers(pool_t *pool)
{
	uint32_t i;
	odp_buffer_hdr_t *buf_hdr;
	odp_packet_hdr_t *pkt_hdr;
	odp_shm_info_t shm_info;
	void *addr;
	void *uarea = NULL;
	uint8_t *data;
	uint32_t offset;
	ring_t *ring;
	uint32_t mask;
	int type;
	uint32_t seg_size;
	uint64_t page_size;
	int skipped_blocks = 0;

	if (odp_shm_info(pool->shm, &shm_info))
		ODP_ABORT("Shm info failed\n");

	page_size = shm_info.page_size;
	ring = &pool->ring->hdr;
	mask = pool->ring_mask;
	type = pool->params.type;

	for (i = 0; i < pool->num + skipped_blocks ; i++) {
		addr    = &pool->base_addr[i * pool->block_size];
		buf_hdr = addr;
		pkt_hdr = addr;
		/* Skip packet buffers which cross huge page boundaries. Some
		 * NICs cannot handle buffers which cross page boundaries. */
		if (pool->params.type == ODP_POOL_PACKET &&
		    page_size >= FIRST_HP_SIZE) {
			uint64_t first_page;
			uint64_t last_page;

			first_page = ((uint64_t)(uintptr_t)addr &
					~(page_size - 1));
			last_page = (((uint64_t)(uintptr_t)addr +
					pool->block_size - 1) &
					~(page_size - 1));
			if (last_page != first_page) {
				skipped_blocks++;
				continue;
			}
		}
		if (pool->uarea_size)
			uarea = &pool->uarea_base_addr[(i - skipped_blocks) *
						       pool->uarea_size];
		data = buf_hdr->data;

		if (type == ODP_POOL_PACKET)
			data = pkt_hdr->data;

		offset = pool->headroom;

		/* move to correct align */
		while (((uintptr_t)&data[offset]) % pool->align != 0)
			offset++;

		memset(buf_hdr, 0, (uintptr_t)data - (uintptr_t)buf_hdr);

		seg_size = pool->headroom + pool->seg_len + pool->tailroom;

		/* Initialize buffer metadata */
		buf_hdr->size = seg_size;
		buf_hdr->type = type;
		buf_hdr->event_type = type;
		buf_hdr->event_subtype = ODP_EVENT_NO_SUBTYPE;
		buf_hdr->pool_hdl = pool->pool_hdl;
		buf_hdr->pool_ptr = pool;
		buf_hdr->uarea_addr = uarea;
		/* Show user requested size through API */
		buf_hdr->segcount = 1;
		buf_hdr->num_seg  = 1;
		buf_hdr->next_seg = NULL;
		buf_hdr->last_seg = buf_hdr;

		/* Pointer to data start (of the first segment) */
		buf_hdr->seg[0].hdr       = buf_hdr;
		buf_hdr->seg[0].data      = &data[offset];
		buf_hdr->seg[0].len       = pool->seg_len;

		odp_atomic_init_u32(&buf_hdr->ref_cnt, 0);

		/* Store base values for fast init */
		buf_hdr->base_data = buf_hdr->seg[0].data;
		buf_hdr->buf_end   = &data[offset + pool->seg_len +
				     pool->tailroom];

		/* Store buffer index into the global pool */
		ring_enq(ring, mask, i);
	}
}

static bool shm_is_from_huge_pages(odp_shm_t shm)
{
	odp_shm_info_t info;
	uint64_t huge_page_size = odp_sys_huge_page_size();

	if (huge_page_size == 0)
		return 0;

	if (odp_shm_info(shm, &info)) {
		ODP_ERR("Failed to fetch shm info\n");
		return 0;
	}

	return (info.page_size >= huge_page_size);
}

static odp_pool_t pool_create(const char *name, odp_pool_param_t *params,
			      uint32_t shmflags)
{
	pool_t *pool;
	uint32_t uarea_size, headroom, tailroom;
	odp_shm_t shm;
	uint32_t seg_len, align, num, hdr_size, block_size;
	uint32_t max_len;
	uint32_t ring_size;
	uint32_t num_extra = 0;
	int name_len;
	const char *postfix = "_uarea";
	char uarea_name[ODP_POOL_NAME_LEN + sizeof(postfix)];

	if (params == NULL) {
		ODP_ERR("No params");
		return ODP_POOL_INVALID;
	}

	align = 0;

	if (params->type == ODP_POOL_BUFFER)
		align = params->buf.align;

	if (align < ODP_CONFIG_BUFFER_ALIGN_MIN)
		align = ODP_CONFIG_BUFFER_ALIGN_MIN;

	/* Validate requested buffer alignment */
	if (align > ODP_CONFIG_BUFFER_ALIGN_MAX ||
	    align != ROUNDDOWN_POWER2(align, align)) {
		ODP_ERR("Bad align requirement");
		return ODP_POOL_INVALID;
	}

	headroom    = 0;
	tailroom    = 0;
	seg_len     = 0;
	max_len     = 0;
	uarea_size  = 0;

	switch (params->type) {
	case ODP_POOL_BUFFER:
		num  = params->buf.num;
		seg_len = params->buf.size;
		break;

	case ODP_POOL_PACKET:
		if (params->pkt.headroom > CONFIG_PACKET_HEADROOM) {
			ODP_ERR("Packet headroom size not supported.");
			return ODP_POOL_INVALID;
		}

		seg_len = CONFIG_PACKET_MAX_SEG_LEN;
		max_len = CONFIG_PACKET_MAX_LEN;

		if (params->pkt.len &&
		    params->pkt.len < CONFIG_PACKET_MAX_SEG_LEN)
			seg_len = params->pkt.len;
		if (params->pkt.seg_len && params->pkt.seg_len > seg_len)
			seg_len = params->pkt.seg_len;
		if (seg_len < CONFIG_PACKET_SEG_LEN_MIN)
			seg_len = CONFIG_PACKET_SEG_LEN_MIN;

		/* Make sure that at least one 'max_len' packet can fit in the
		 * pool. */
		if (params->pkt.max_len != 0)
			max_len = params->pkt.max_len;
		if ((max_len + seg_len - 1) / seg_len > CONFIG_PACKET_MAX_SEGS)
			seg_len = (max_len + CONFIG_PACKET_MAX_SEGS - 1) /
				CONFIG_PACKET_MAX_SEGS;
		if (seg_len > CONFIG_PACKET_MAX_SEG_LEN) {
			ODP_ERR("Pool unable to store 'max_len' packet");
			return ODP_POOL_INVALID;
		}

		headroom    = CONFIG_PACKET_HEADROOM;
		tailroom    = CONFIG_PACKET_TAILROOM;
		num         = params->pkt.num;
		uarea_size  = params->pkt.uarea_size;
		break;

	case ODP_POOL_TIMEOUT:
		num = params->tmo.num;
		break;

	default:
		ODP_ERR("Bad pool type");
		return ODP_POOL_INVALID;
	}

	if (uarea_size)
		uarea_size = ROUNDUP_CACHE_LINE(uarea_size);

	if (num <= RING_SIZE_MIN)
		ring_size = RING_SIZE_MIN;
	else
		ring_size = ROUNDUP_POWER2_U32(num);

	pool = reserve_pool(ring_size);

	if (pool == NULL) {
		ODP_ERR("No more free pools");
		return ODP_POOL_INVALID;
	}

	if (name == NULL) {
		pool->name[0] = 0;
	} else {
		strncpy(pool->name, name,
			ODP_POOL_NAME_LEN - 1);
		pool->name[ODP_POOL_NAME_LEN - 1] = 0;
	}

	name_len = strlen(pool->name);
	memcpy(uarea_name, pool->name, name_len);
	strcpy(&uarea_name[name_len], postfix);

	pool->params = *params;

	hdr_size = sizeof(odp_packet_hdr_t);
	hdr_size = ROUNDUP_CACHE_LINE(hdr_size);

	block_size = ROUNDUP_CACHE_LINE(hdr_size + align + headroom + seg_len +
					tailroom);

	/* Allocate extra memory for skipping packet buffers which cross huge
	 * page boundaries. */
	if (params->type == ODP_POOL_PACKET) {
		num_extra = (((uint64_t)(num * block_size) +
				FIRST_HP_SIZE - 1) / FIRST_HP_SIZE);
		num_extra += (((uint64_t)(num_extra * block_size) +
				FIRST_HP_SIZE - 1) / FIRST_HP_SIZE);
	}

	pool->ring_mask      = ring_size - 1;
	pool->num            = num;
	pool->align          = align;
	pool->headroom       = headroom;
	pool->seg_len        = seg_len;
	pool->max_len        = max_len;
	pool->tailroom       = tailroom;
	pool->block_size     = block_size;
	pool->uarea_size     = uarea_size;
	pool->shm_size       = (num + num_extra) * block_size;
	pool->uarea_shm_size = num * uarea_size;
	pool->ext_desc       = NULL;
	pool->ext_destroy    = NULL;

	shm = odp_shm_reserve(pool->name, pool->shm_size,
			      ODP_PAGE_SIZE, shmflags);

	pool->shm = shm;

	if (shm == ODP_SHM_INVALID) {
		ODP_ERR("Shm reserve failed");
		goto error;
	}

	pool->mem_from_huge_pages = shm_is_from_huge_pages(pool->shm);

	pool->base_addr = odp_shm_addr(pool->shm);

	pool->uarea_shm = ODP_SHM_INVALID;
	if (uarea_size) {
		shm = odp_shm_reserve(uarea_name, pool->uarea_shm_size,
				      ODP_PAGE_SIZE, shmflags);

		pool->uarea_shm = shm;

		if (shm == ODP_SHM_INVALID) {
			ODP_ERR("Shm reserve failed (uarea)");
			goto error;
		}

		pool->uarea_base_addr = odp_shm_addr(pool->uarea_shm);
	}

	ring_init(&pool->ring->hdr);
	init_buffers(pool);

	return pool->pool_hdl;

error:
	if (pool->shm != ODP_SHM_INVALID)
		odp_shm_free(pool->shm);

	if (pool->uarea_shm != ODP_SHM_INVALID)
		odp_shm_free(pool->uarea_shm);

	LOCK(&pool->lock);
	pool->reserved = 0;
	UNLOCK(&pool->lock);
	return ODP_POOL_INVALID;
}

static int check_params(odp_pool_param_t *params)
{
	odp_pool_capability_t capa;

	if (odp_pool_capability(&capa) < 0)
		return -1;

	switch (params->type) {
	case ODP_POOL_BUFFER:
		if (params->buf.size > capa.buf.max_size) {
			printf("buf.size too large %u\n", params->buf.size);
			return -1;
		}

		if (params->buf.align > capa.buf.max_align) {
			printf("buf.align too large %u\n", params->buf.align);
			return -1;
		}

		break;

	case ODP_POOL_PACKET:
		if (params->pkt.len > capa.pkt.max_len) {
			printf("pkt.len too large %u\n", params->pkt.len);
			return -1;
		}

		if (params->pkt.max_len > capa.pkt.max_len) {
			printf("pkt.max_len too large %u\n",
			       params->pkt.max_len);
			return -1;
		}

		if (params->pkt.seg_len > capa.pkt.max_seg_len) {
			printf("pkt.seg_len too large %u\n",
			       params->pkt.seg_len);
			return -1;
		}

		if (params->pkt.uarea_size > capa.pkt.max_uarea_size) {
			printf("pkt.uarea_size too large %u\n",
			       params->pkt.uarea_size);
			return -1;
		}

		break;

	case ODP_POOL_TIMEOUT:
		break;

	default:
		printf("bad pool type %i\n", params->type);
		return -1;
	}

	return 0;
}

static odp_pool_t generic_pool_create(const char *name,
				      odp_pool_param_t *params)
{
	uint32_t shm_flags = 0;

	if (check_params(params))
		return ODP_POOL_INVALID;

	if (params && (params->type == ODP_POOL_PACKET))
		shm_flags = ODP_SHM_PROC;

	return pool_create(name, params, shm_flags);
}

static int generic_pool_destroy(odp_pool_t pool_hdl)
{
	pool_t *pool = pool_entry_from_hdl(pool_hdl);
	int i;

	if (pool == NULL)
		return -1;

	LOCK(&pool->lock);

	if (pool->reserved == 0) {
		UNLOCK(&pool->lock);
		ODP_ERR("Pool not created\n");
		return -1;
	}

	/* Destroy external DPDK mempool */
	if (pool->ext_destroy) {
		pool->ext_destroy(pool->ext_desc);
		pool->ext_destroy = NULL;
		pool->ext_desc = NULL;
	}

	/* Make sure local caches are empty */
	for (i = 0; i < ODP_THREAD_COUNT_MAX; i++)
		flush_cache(&pool->local_cache[i], pool);

	odp_shm_free(pool->shm);

	if (pool->uarea_shm != ODP_SHM_INVALID)
		odp_shm_free(pool->uarea_shm);

	pool->reserved = 0;
	odp_shm_free(pool->ring_shm);
	pool->ring = NULL;
	UNLOCK(&pool->lock);

	return 0;
}

static odp_pool_t generic_pool_lookup(const char *name)
{
	uint32_t i;
	pool_t *pool;

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool = pool_entry(i);

		LOCK(&pool->lock);
		if (strcmp(name, pool->name) == 0) {
			/* found it */
			UNLOCK(&pool->lock);
			return pool->pool_hdl;
		}
		UNLOCK(&pool->lock);
	}

	return ODP_POOL_INVALID;
}

static int generic_pool_info(odp_pool_t pool_hdl, odp_pool_info_t *info)
{
	pool_t *pool = pool_entry_from_hdl(pool_hdl);

	if (pool == NULL || info == NULL)
		return -1;

	info->name = pool->name;
	info->params = pool->params;

	info->min_data_addr = (uint64_t) pool->base_addr;
	info->max_data_addr = (uint64_t) pool->base_addr + pool->shm_size - 1;

	return 0;
}

static int generic_pool_capability(odp_pool_capability_t *capa)
{
	uint32_t max_seg_len = CONFIG_PACKET_MAX_SEG_LEN;

	memset(capa, 0, sizeof(odp_pool_capability_t));

	capa->max_pools = ODP_CONFIG_POOLS;

	/* Buffer pools */
	capa->buf.max_pools = ODP_CONFIG_POOLS;
	capa->buf.max_align = ODP_CONFIG_BUFFER_ALIGN_MAX;
	capa->buf.max_size  = MAX_SIZE;
	capa->buf.max_num   = 0;

	/* Packet pools */
	capa->pkt.max_pools        = ODP_CONFIG_POOLS;
	capa->pkt.max_len          = CONFIG_PACKET_MAX_LEN;
	capa->pkt.max_num	   = 0;
	capa->pkt.min_headroom     = CONFIG_PACKET_HEADROOM;
	capa->pkt.max_headroom     = CONFIG_PACKET_HEADROOM;
	capa->pkt.min_tailroom     = CONFIG_PACKET_TAILROOM;
	capa->pkt.max_segs_per_pkt = CONFIG_PACKET_MAX_SEGS;
	capa->pkt.min_seg_len      = CONFIG_PACKET_SEG_LEN_MIN;
	capa->pkt.max_seg_len      = max_seg_len;
	capa->pkt.max_uarea_size   = MAX_SIZE;

	/* Timeout pools */
	capa->tmo.max_pools = ODP_CONFIG_POOLS;
	capa->tmo.max_num   = 0;

	return 0;
}

static void generic_pool_print(odp_pool_t pool_hdl)
{
	pool_t *pool;

	pool = pool_entry_from_hdl(pool_hdl);

	printf("\nPool info\n");
	printf("---------\n");
	printf("  pool            %" PRIu64 "\n",
	       odp_pool_to_u64(pool->pool_hdl));
	printf("  name            %s\n", pool->name);
	printf("  pool type       %s\n",
	       pool->params.type == ODP_POOL_BUFFER ? "buffer" :
	       (pool->params.type == ODP_POOL_PACKET ? "packet" :
	       (pool->params.type == ODP_POOL_TIMEOUT ? "timeout" :
		"unknown")));
	printf("  pool shm        %" PRIu64 "\n",
	       odp_shm_to_u64(pool->shm));
	printf("  user area shm   %" PRIu64 "\n",
	       odp_shm_to_u64(pool->uarea_shm));
	printf("  num             %u\n", pool->num);
	printf("  align           %u\n", pool->align);
	printf("  headroom        %u\n", pool->headroom);
	printf("  seg len         %u\n", pool->seg_len);
	printf("  max data len    %u\n", pool->max_len);
	printf("  tailroom        %u\n", pool->tailroom);
	printf("  block size      %u\n", pool->block_size);
	printf("  uarea size      %u\n", pool->uarea_size);
	printf("  shm size        %u\n", pool->shm_size);
	printf("  base addr       %p\n", pool->base_addr);
	printf("  uarea shm size  %u\n", pool->uarea_shm_size);
	printf("  uarea base addr %p\n", pool->uarea_base_addr);
	printf("\n");
}

static void generic_pool_param_init(odp_pool_param_t *params)
{
	memset(params, 0, sizeof(odp_pool_param_t));
	params->pkt.headroom = CONFIG_PACKET_HEADROOM;
}

static uint64_t generic_pool_to_u64(odp_pool_t hdl)
{
	return _odp_pri(hdl);
}

pool_module_t generic_pool = {
	.base = {
		.name = "generic_pool",
		.init_local = generic_pool_init_local,
		.term_local = generic_pool_term_local,
		.init_global = generic_pool_init_global,
		.term_global = generic_pool_term_global,
		},
	.capability = generic_pool_capability,
	.create = generic_pool_create,
	.destroy = generic_pool_destroy,
	.lookup = generic_pool_lookup,
	.info = generic_pool_info,
	.print = generic_pool_print,
	.to_u64 = generic_pool_to_u64,
	.param_init = generic_pool_param_init,
};

ODP_MODULE_CONSTRUCTOR(generic_pool)
{
	odp_module_constructor(&generic_pool);
	odp_subsystem_register_module(pool, &generic_pool);
}
