/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/********************************************************************//**
@file btr/btr0sea.cc
The index tree adaptive search

Created 2/17/1996 Heikki Tuuri
*************************************************************************/

#include "btr0sea.h"
#ifdef BTR_CUR_HASH_ADAPT
#include "buf0buf.h"
#include "buf0lru.h"
#include "page0page.h"
#include "page0cur.h"
#include "btr0cur.h"
#include "btr0pcur.h"
#include "btr0btr.h"
#include "srv0mon.h"

#ifdef UNIV_SEARCH_PERF_STAT
/** Number of successful adaptive hash index lookups */
ulint		btr_search_n_succ	= 0;
/** Number of failed adaptive hash index lookups */
ulint		btr_search_n_hash_fail	= 0;
#endif /* UNIV_SEARCH_PERF_STAT */

#ifdef UNIV_PFS_RWLOCK
mysql_pfs_key_t	btr_search_latch_key;
#endif /* UNIV_PFS_RWLOCK */

/** The adaptive hash index */
btr_sea btr_search;

struct ahi_node {
  /** CRC-32C of the rec prefix */
  uint32_t fold;
  /** pointer to next record in the hash bucket chain, or nullptr  */
  ahi_node *next;
  /** B-tree index leaf page record */
  const rec_t *rec;
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  /** block containing rec, or nullptr */
  buf_block_t *block;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
};

inline void btr_sea::partition::init() noexcept
{
  memset((void*) this, 0, sizeof *this);
  latch.SRW_LOCK_INIT(btr_search_latch_key);
  blocks_mutex.init();
  UT_LIST_INIT(blocks, &buf_page_t::list);
}

inline void btr_sea::partition::clear() noexcept
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(latch.is_write_locked());
  ut_ad(blocks_mutex.is_locked());
#endif
  if (buf_block_t *b= spare)
  {
    spare= nullptr;
    MEM_MAKE_ADDRESSABLE(b->page.frame, srv_page_size);
    buf_pool.free_block(b);
  }
  ut_free(table.array);
  table.array= nullptr;

  while (buf_page_t *b= UT_LIST_GET_FIRST(blocks))
  {
    UT_LIST_REMOVE(blocks, b);
    ut_ad(b->free_offset);
    b->hash= nullptr;
    MEM_MAKE_ADDRESSABLE(b->frame, srv_page_size);
    buf_pool.free_block(reinterpret_cast<buf_block_t*>(b));
  }
}

inline void btr_sea::partition::free() noexcept
{
  if (table.array)
  {
    ut_d(latch.wr_lock(SRW_LOCK_CALL));
    ut_d(blocks_mutex.wr_lock());
    clear();
    ut_d(blocks_mutex.wr_unlock());
    ut_d(latch.wr_unlock());
  }
  latch.destroy();
  blocks_mutex.destroy();
}

inline void btr_sea::partition::alloc(ulint hash_size) noexcept
{
  table.create(hash_size);
}

void btr_sea::create() noexcept
{
  parts.init();
  if (enabled)
    enable();
}

void btr_sea::alloc(ulint hash_size) noexcept
{
  parts.alloc(hash_size);
}

inline void btr_sea::clear() noexcept
{
  parts.clear();
}

void btr_sea::free() noexcept
{
  parts.free();
}

/** If the number of records on the page divided by this parameter
would have been successfully accessed using a hash index, the index
is then built on the page, assuming the global limit has been reached */
#define BTR_SEARCH_PAGE_BUILD_LIMIT	16U

/** The global limit for consecutive potentially successful hash searches,
before hash index building is started */
#define BTR_SEARCH_BUILD_LIMIT		100U

/** Compute a hash value of a record in a page.
@param rec      index record
@param index    index tree
@param n_fields number of complete fields to fold
@param n_bytes  number of bytes to fold in the last field
@return the hash value */
static uint32_t rec_fold(const rec_t *rec, const dict_index_t &index,
                         size_t n_fields, size_t n_bytes) noexcept
{
  ut_ad(page_rec_is_leaf(rec));
  ut_ad(page_rec_is_user_rec(rec));
  ut_ad(!rec_is_metadata(rec, index));
  ut_ad(index.n_uniq <= index.n_core_fields);
  size_t n_f= n_fields + !!n_bytes;
  ut_ad(n_f > 0);
  ut_ad(n_f <= index.n_core_fields);

  size_t n;

  if (index.table->not_redundant())
  {
    const unsigned n_core_null_bytes= index.n_core_null_bytes;
    const byte *nulls= rec - REC_N_NEW_EXTRA_BYTES;
    const byte *lens= --nulls - n_core_null_bytes;
    byte null_mask= 1;
    n= 0;

    const dict_field_t *field = index.fields;
    size_t len;
    do
    {
      const dict_col_t* col = field->col;
      if (col->is_nullable())
      {
        if (UNIV_UNLIKELY(!null_mask))
          null_mask= 1, nulls--;
        const int is_null{*nulls & null_mask};
        null_mask<<= 1;
        if (is_null)
        {
          len= 0;
          continue;
        }
      }

      len= field->fixed_len;

      if (!len)
      {
        len= *lens--;
        if (UNIV_UNLIKELY(len & 0x80) && DATA_BIG_COL(col))
        {
          len<<= 8;
          len|= *lens--;
          ut_ad(!(len & 0x4000));
          len&= 0x3fff;
        }
      }

      n+= len;
    }
    while (field++, --n_f);

    if (n_bytes)
      n+= std::min(n_bytes, len) - len;
  }
  else
  {
    ut_ad(n_f <= rec_get_n_fields_old(rec));
    if (rec_get_1byte_offs_flag(rec))
    {
      n= rec_1_get_field_end_info(rec, n_f - 1);
      if (!n_bytes);
      else if (!n_fields)
        n= std::min(n_bytes, n);
      else
      {
        size_t len= n - rec_1_get_field_end_info(rec, n_f - 2);
        n+= std::min(n_bytes, n - len) - len;
      }
    }
    else
    {
      n= rec_2_get_field_end_info(rec, n_f - 1);
      if (!n_bytes);
      else if (!n_fields)
        n= std::min(n_bytes, n);
      else
      {
        size_t len= n - rec_2_get_field_end_info(rec, n_f - 2);
        n+= std::min(n_bytes, n - len) - len;
      }
    }
  }

  return my_crc32c(uint32_t(ut_fold_ull(index.id)), rec, n);
}

/** Determine the number of accessed key fields.
@param[in]	n_fields	number of complete fields
@param[in]	n_bytes		number of bytes in an incomplete last field
@return	number of complete or incomplete fields */
inline MY_ATTRIBUTE((warn_unused_result))
ulint
btr_search_get_n_fields(
	ulint	n_fields,
	ulint	n_bytes)
{
	return(n_fields + (n_bytes > 0 ? 1 : 0));
}

/** Determine the number of accessed key fields.
@param[in]	cursor		b-tree cursor
@return	number of complete or incomplete fields */
inline MY_ATTRIBUTE((warn_unused_result))
ulint
btr_search_get_n_fields(
	const btr_cur_t*	cursor)
{
	return(btr_search_get_n_fields(cursor->n_fields, cursor->n_bytes));
}

void btr_sea::partition::prepare_insert() noexcept
{
  /* spare may be consumed by insert() or clear() */
  if (!spare && btr_search.enabled)
  {
    buf_block_t *block= buf_block_alloc();
    blocks_mutex.wr_lock();
    if (!spare && btr_search.enabled)
    {
      MEM_NOACCESS(block->page.frame, srv_page_size);
      spare= block;
      block= nullptr;
    }
    blocks_mutex.wr_unlock();
    if (block)
      buf_pool.free_block(block);
  }
}

/** Set index->ref_count = 0 on all indexes of a table.
@param[in,out]	table	table handler */
static void btr_search_disable_ref_count(dict_table_t *table)
{
  for (dict_index_t *index= dict_table_get_first_index(table); index;
       index= dict_table_get_next_index(index))
    index->search_info.ref_count= 0;
}

/** Lazily free detached metadata when removing the last reference. */
ATTRIBUTE_COLD static void btr_search_lazy_free(dict_index_t *index)
{
  ut_ad(index->freed());
  dict_table_t *table= index->table;
  table->autoinc_mutex.wr_lock();

  /* Perform the skipped steps of dict_index_remove_from_cache_low(). */
  UT_LIST_REMOVE(table->freed_indexes, index);
  index->lock.free();
  dict_mem_index_free(index);

  if (!UT_LIST_GET_LEN(table->freed_indexes) &&
      !UT_LIST_GET_LEN(table->indexes))
  {
    ut_ad(!table->id);
    table->autoinc_mutex.wr_unlock();
    table->autoinc_mutex.destroy();
    dict_mem_table_free(table);
    return;
  }

  table->autoinc_mutex.wr_unlock();
}

/** Disable the adaptive hash search system and empty the index. */
void btr_sea::disable() noexcept
{
	dict_table_t*	table;

	dict_sys.freeze(SRW_LOCK_CALL);

	btr_search_x_lock_all();

	if (!enabled) {
		dict_sys.unfreeze();
		btr_search_x_unlock_all();
		return;
	}

	enabled= false;

	/* Clear the index->search_info->ref_count of every index in
	the data dictionary cache. */
	for (table = UT_LIST_GET_FIRST(dict_sys.table_LRU); table;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		btr_search_disable_ref_count(table);
	}

	for (table = UT_LIST_GET_FIRST(dict_sys.table_non_LRU); table;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		btr_search_disable_ref_count(table);
	}

	dict_sys.unfreeze();

	/* Set all block->index = NULL. */
	buf_pool.clear_hash_index();

	/* Clear the adaptive hash index. */
	btr_search.parts.blocks_mutex.wr_lock();
	btr_search.clear();
	btr_search.parts.blocks_mutex.wr_unlock();

	btr_search_x_unlock_all();
}

/** Enable the adaptive hash search system.
@param resize whether buf_pool_t::resize() is the caller */
void btr_sea::enable(bool resize) noexcept
{
	if (!resize) {
		mysql_mutex_lock(&buf_pool.mutex);
		bool changed = srv_buf_pool_old_size != srv_buf_pool_size;
		mysql_mutex_unlock(&buf_pool.mutex);
		if (changed) {
			return;
		}
	}

	btr_search_x_lock_all();
	ulint hash_size = buf_pool_get_curr_size() / sizeof(void *) / 64;

	if (parts.table.array) {
		ut_ad(enabled);
		btr_search_x_unlock_all();
		return;
	}

	alloc(hash_size);

	enabled = true;
	btr_search_x_unlock_all();
}

/** Updates the search info of an index about hash successes. NOTE that info
is NOT protected by any semaphore, to save CPU time! Do not assume its fields
are consistent.
@param[in]	cursor	cursor which was just positioned */
static void btr_search_info_update_hash(const btr_cur_t *cursor)
{
	ut_ad(cursor->flag != BTR_CUR_HASH);

	dict_index_t*	index = cursor->index();
	int		cmp;

	if (dict_index_is_ibuf(index)) {
		/* So many deletes are performed on an insert buffer tree
		that we do not consider a hash index useful on it: */

		return;
	}

	uint16_t n_unique = dict_index_get_n_unique_in_tree(index);
	auto info = &index->search_info;

	if (info->n_hash_potential == 0) {

		goto set_new_recomm;
	}

	/* Test if the search would have succeeded using the recommended
	hash prefix */

	if (info->n_fields >= n_unique && cursor->up_match >= n_unique) {
increment_potential:
		info->n_hash_potential++;

		return;
	}

	cmp = ut_pair_cmp(info->n_fields, info->n_bytes,
			  cursor->low_match, cursor->low_bytes);

	if (info->left_side ? cmp <= 0 : cmp > 0) {

		goto set_new_recomm;
	}

	cmp = ut_pair_cmp(info->n_fields, info->n_bytes,
			  cursor->up_match, cursor->up_bytes);

	if (info->left_side ? cmp <= 0 : cmp > 0) {

		goto increment_potential;
	}

set_new_recomm:
	/* We have to set a new recommendation; skip the hash analysis
	for a while to avoid unnecessary CPU time usage when there is no
	chance for success */

	info->hash_analysis_reset();

	cmp = ut_pair_cmp(cursor->up_match, cursor->up_bytes,
			  cursor->low_match, cursor->low_bytes);
	info->left_side = cmp >= 0;
	info->n_hash_potential = cmp != 0;

	if (cmp == 0) {
		/* For extra safety, we set some sensible values here */
		info->n_fields = 1;
		info->n_bytes = 0;
	} else if (cmp > 0) {
		info->n_hash_potential = 1;

		if (cursor->up_match >= n_unique) {

			info->n_fields = n_unique;
			info->n_bytes = 0;

		} else if (cursor->low_match < cursor->up_match) {

			info->n_fields = static_cast<uint16_t>(
				cursor->low_match + 1);
			info->n_bytes = 0;
		} else {
			info->n_fields = static_cast<uint16_t>(
				cursor->low_match);
			info->n_bytes = static_cast<uint16_t>(
				cursor->low_bytes + 1);
		}
	} else {
		if (cursor->low_match >= n_unique) {

			info->n_fields = n_unique;
			info->n_bytes = 0;
		} else if (cursor->low_match > cursor->up_match) {

			info->n_fields = static_cast<uint16_t>(
				cursor->up_match + 1);
			info->n_bytes = 0;
		} else {
			info->n_fields = static_cast<uint16_t>(
				cursor->up_match);
			info->n_bytes = static_cast<uint16_t>(
				cursor->up_bytes + 1);
		}
	}
}

/** Update the block search info on hash successes. NOTE that info and
block->n_hash_helps, n_fields, n_bytes, left_side are NOT protected by any
semaphore, to save CPU time! Do not assume the fields are consistent.
@return TRUE if building a (new) hash index on the block is recommended
@param[in,out]	info	search info
@param[in,out]	block	buffer block */
static
bool
btr_search_update_block_hash_info(dict_index_t::ahi* info, buf_block_t* block)
{
	ut_ad(block->page.lock.have_x() || block->page.lock.have_s());

	info->last_hash_succ = FALSE;
	ut_ad(block->page.frame);

	if ((block->n_hash_helps > 0)
	    && (info->n_hash_potential > 0)
	    && (block->n_fields == info->n_fields)
	    && (block->n_bytes == info->n_bytes)
	    && (block->left_side == info->left_side)) {

		if ((block->index)
		    && (block->curr_n_fields == info->n_fields)
		    && (block->curr_n_bytes == info->n_bytes)
		    && (block->curr_left_side == info->left_side)) {

			/* The search would presumably have succeeded using
			the hash index */

			info->last_hash_succ = TRUE;
		}

		block->n_hash_helps++;
	} else {
		block->n_hash_helps = 1;
		block->n_fields = info->n_fields;
		block->n_bytes = info->n_bytes;
		block->left_side = info->left_side;
	}

	if ((block->n_hash_helps > page_get_n_recs(block->page.frame)
	     / BTR_SEARCH_PAGE_BUILD_LIMIT)
	    && (info->n_hash_potential >= BTR_SEARCH_BUILD_LIMIT)) {

		if ((!block->index)
		    || (block->n_hash_helps
			> 2U * page_get_n_recs(block->page.frame))
		    || (block->n_fields != block->curr_n_fields)
		    || (block->n_bytes != block->curr_n_bytes)
		    || (block->left_side != block->curr_left_side)) {

			/* Build a new hash index on the page */

			return(true);
		}
	}

	return(false);
}

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
/** Maximum number of records in a page */
constexpr ulint MAX_N_POINTERS = UNIV_PAGE_SIZE_MAX / REC_N_NEW_EXTRA_BYTES;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
void btr_sea::partition::insert(uint32 fold, const rec_t *rec,
                                buf_block_t *block) noexcept
#else
void btr_sea::partition::insert(uint32_t fold, const rec_t *rec) noexcept
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(latch.is_write_locked());
#endif
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  ut_a(block->page.frame == page_align(rec));
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
  ut_ad(btr_search.enabled);

  hash_cell_t *cell= &table.array[table.calc_hash(fold)];

  for (ahi_node *prev= static_cast<ahi_node*>(cell->node); prev;
       prev= prev->next)
  {
    if (prev->fold == fold)
    {
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
      buf_block_t *prev_block= prev->block;
      ut_a(prev_block->page.frame == page_align(prev->rec));
      ut_a(prev_block->n_pointers-- < MAX_N_POINTERS);
      ut_a(block->n_pointers++ < MAX_N_POINTERS);

      prev->block= block;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
      prev->rec= rec;
      return;
    }
  }

  /* We have to allocate a new chain node */
  ahi_node *node;

  {
    blocks_mutex.wr_lock();
    buf_page_t *last= UT_LIST_GET_LAST(blocks);
    if (last && last->free_offset < srv_page_size - sizeof *node)
    {
      node= reinterpret_cast<ahi_node*>(last->frame + last->free_offset);
#if defined __GNUC__ && !defined __clang__
# pragma GCC diagnostic push
# if __GNUC__ < 12 || defined WITH_UBSAN
#  pragma GCC diagnostic ignored "-Wconversion"
# endif
#endif
      last->free_offset+= sizeof *node;
#if defined __GNUC__ && !defined __clang__
# pragma GCC diagnostic pop
#endif
      MEM_MAKE_ADDRESSABLE(node, sizeof *node);
    }
    else
    {
      last= &spare.load()->page;
      if (!last)
      {
        blocks_mutex.wr_unlock();
        return;
      }
      spare= nullptr;
      UT_LIST_ADD_LAST(blocks, last);
      last->free_offset= sizeof *node;
      node= reinterpret_cast<ahi_node*>(last->frame);
      MEM_UNDEFINED(last->frame, srv_page_size);
      MEM_MAKE_ADDRESSABLE(node, sizeof *node);
      MEM_NOACCESS(node + 1, srv_page_size - sizeof *node);
    }
    blocks_mutex.wr_unlock();
  }

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  ut_a(block->n_pointers++ < MAX_N_POINTERS);
  node->block= block;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
  node->rec= rec;

  node->fold= fold;
  node->next= nullptr;

  ahi_node *prev= static_cast<ahi_node*>(cell->node);
  if (!prev)
    cell->node= node;
  else
  {
    while (prev->next)
      prev= prev->next;
    prev->next= node;
  }
}

buf_block_t *btr_sea::partition::cleanup_after_erase(ahi_node *erase) noexcept
{
  ut_ad(btr_search.enabled);
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  ut_a(erase->block->page.frame == page_align(erase->rec));
  ut_a(erase->block->n_pointers-- < MAX_N_POINTERS);
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */

  blocks_mutex.wr_lock();

  buf_page_t *last= UT_LIST_GET_LAST(blocks);
  const ahi_node *const top= reinterpret_cast<ahi_node*>
    (last->frame + last->free_offset - sizeof *top);

  if (erase != top)
  {
    /* Shrink the allocation by replacing the erased element with the top. */
    *erase= *top;
    ahi_node **prev= reinterpret_cast<ahi_node**>
      (&table.cell_get(top->fold)->node);
    while (*prev != top)
      prev= &(*prev)->next;
    *prev= erase;
  }

  buf_block_t *freed= nullptr;

#if defined __GNUC__ && !defined __clang__
# pragma GCC diagnostic push
# if __GNUC__ < 12 || defined WITH_UBSAN
#  pragma GCC diagnostic ignored "-Wconversion"
# endif
#endif
  /* We may be able to shrink or free the last block */
  if (!(last->free_offset-= uint16_t(sizeof *erase)))
#if defined __GNUC__ && !defined __clang__
# pragma GCC diagnostic pop
#endif
  {
    if (spare)
    {
      freed= reinterpret_cast<buf_block_t*>(last);
      MEM_MAKE_ADDRESSABLE(last->frame, srv_page_size);
    }
    else
      spare= reinterpret_cast<buf_block_t*>(last);
    UT_LIST_REMOVE(blocks, last);
  }
  else
    MEM_NOACCESS(last->frame + last->free_offset, sizeof *erase);

  blocks_mutex.wr_unlock();
  return freed;
}

__attribute__((nonnull))
/** Delete all pointers to a page.
@param table     hash table
@param fold      fold value
@param page      page of a record to be deleted */
static void ha_remove_all_nodes_to_page(hash_table_t *table, uint32_t fold,
                                        const page_t *page)
{
  hash_cell_t *cell= table->cell_get(fold);
  static const uintptr_t page_size{srv_page_size};

rewind:
  for (ahi_node **prev= reinterpret_cast<ahi_node**>(&cell->node);
       *prev; prev= &(*prev)->next)
  {
    ahi_node *node= *prev;
    if ((uintptr_t(node->rec) ^ uintptr_t(page)) < page_size)
    {
      *prev= node->next;
      node->next= nullptr;
      if (buf_block_t *block= btr_search.parts.cleanup_after_erase(node))
        buf_pool.free_block(block);
      /* The deletion may compact the heap of nodes and move other nodes! */
      goto rewind;
    }
  }
#ifdef UNIV_DEBUG
  /* Check that all nodes really got deleted */
  for (ahi_node *node= static_cast<ahi_node*>(cell->node); node;
       node= node->next)
    ut_ad(page_align(node->rec) != page);
#endif /* UNIV_DEBUG */
}

inline bool btr_sea::partition::erase(uint32_t fold, const rec_t *rec) noexcept
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(latch.is_write_locked());
#endif
  ut_ad(btr_search.enabled);
  hash_cell_t *cell= table.cell_get(fold);

  for (ahi_node **prev= reinterpret_cast<ahi_node**>(&cell->node);
       *prev; prev= &(*prev)->next)
  {
    ahi_node *node= *prev;
    if (node->rec == rec)
    {
      *prev= node->next;
      node->next= nullptr;
      buf_block_t *block= btr_search.parts.cleanup_after_erase(node);
      latch.wr_unlock();
      if (block)
        buf_pool.free_block(block);
      return true;
    }
  }

  latch.wr_unlock();
  return false;
}

__attribute__((nonnull))
/** Looks for an element when we know the pointer to the data and
updates the pointer to data if found.
@param table     hash table
@param fold      folded value of the searched data
@param data      pointer to the data
@param new_data  new pointer to the data
@return whether the element was found */
static bool ha_search_and_update_if_found(hash_table_t *table, uint32_t fold,
                                          const rec_t *data,
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
                                          /** block containing new_data */
                                          buf_block_t *new_block,
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
                                          const rec_t *new_data)
{
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  ut_a(new_block->page.frame == page_align(new_data));
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */

  if (!btr_search.enabled)
    return false;

  for (ahi_node *node= static_cast<ahi_node*>(table->cell_get(fold)->node);
       node; node= node->next)
    if (node->rec == data)
    {
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
      ut_a(node->block->n_pointers-- < MAX_N_POINTERS);
      ut_a(new_block->n_pointers++ < MAX_N_POINTERS);
      node->block= new_block;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
      node->rec= new_data;
      return true;
    }

  return false;
}

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
# define ha_insert_for_fold(p,f,b,d) (p)->insert(f,d,b)
#else
# define ha_insert_for_fold(p,f,b,d) (p)->insert(f,d)
# define ha_search_and_update_if_found(table,fold,data,new_block,new_data) \
	ha_search_and_update_if_found(table,fold,data,new_data)
#endif

/** Updates a hash node reference when it has been unsuccessfully used in a
search which could have succeeded with the used hash parameters. This can
happen because when building a hash index for a page, we do not check
what happens at page boundaries, and therefore there can be misleading
hash nodes. Also, collisions in the fold value can lead to misleading
references. This function lazily fixes these imperfections in the hash
index.
@param[in]	cursor	cursor */
static void btr_search_update_hash_ref(const btr_cur_t* cursor) noexcept
{
	ut_ad(cursor->flag == BTR_CUR_HASH_FAIL);

	buf_block_t* const block = cursor->page_cur.block;
	ut_ad(block->page.lock.have_x() || block->page.lock.have_s());
	ut_ad(page_align(btr_cur_get_rec(cursor)) == block->page.frame);
	ut_ad(page_is_leaf(block->page.frame));
	assert_block_ahi_valid(block);

	dict_index_t* index = block->index;

	if (!index || !index->search_info.n_hash_potential) {
		return;
	}

	if (index != cursor->index()) {
		ut_ad(index->id == cursor->index()->id);
		btr_search_drop_page_hash_index(block, false);
		return;
	}

	ut_ad(block->page.id().space() == index->table->space_id);
	ut_ad(index == cursor->index());
	ut_ad(!dict_index_is_ibuf(index));
	auto part = &btr_search.parts;
	part->latch.wr_lock(SRW_LOCK_CALL);
	ut_ad(!block->index || block->index == index);

	if (block->index
	    && (block->curr_n_fields == index->search_info.n_fields)
	    && (block->curr_n_bytes == index->search_info.n_bytes)
	    && (block->curr_left_side == index->search_info.left_side)
            && !page_cur_is_before_first(&cursor->page_cur)
            && !page_cur_is_after_last(&cursor->page_cur)
	    && btr_search.enabled) {
		const rec_t* rec = btr_cur_get_rec(cursor);
		uint32_t fold = rec_fold(rec, *index, block->curr_n_fields,
					 block->curr_n_bytes);
		ha_insert_for_fold(part, fold, block, rec);
		MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
	}

	part->latch.wr_unlock();
}

/** Checks if a guessed position for a tree cursor is right. Note that if
mode is PAGE_CUR_LE, which is used in inserts, and the function returns
TRUE, then cursor->up_match and cursor->low_match both have sensible values.
@param[in,out]	cursor		guess cursor position
@param[in]	can_only_compare_to_cursor_rec
				if we do not have a latch on the page of cursor,
				but a latch corresponding search system, then
				ONLY the columns of the record UNDER the cursor
				are protected, not the next or previous record
				in the chain: we cannot look at the next or
				previous record to check our guess!
@param[in]	tuple		data tuple
@param[in]	mode		PAGE_CUR_L, PAGE_CUR_LE, PAGE_CUR_G, PAGE_CUR_GE
@return	whether a match was found */
static
bool
btr_search_check_guess(
	btr_cur_t*	cursor,
	bool		can_only_compare_to_cursor_rec,
	const dtuple_t*	tuple,
	ulint		mode)
{
	rec_t*		rec;
	ulint		n_unique;
	ulint		match;
	int		cmp;
	mem_heap_t*	heap		= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;
	bool		success		= false;
	rec_offs_init(offsets_);

	n_unique = dict_index_get_n_unique_in_tree(cursor->index());

	rec = btr_cur_get_rec(cursor);

	if (UNIV_UNLIKELY(!page_rec_is_user_rec(rec)
			  || !page_rec_is_leaf(rec))) {
		ut_ad("corrupted index" == 0);
		return false;
	} else if (cursor->index()->table->not_redundant()) {
		switch (rec_get_status(rec)) {
		case REC_STATUS_INSTANT:
		case REC_STATUS_ORDINARY:
			break;
		default:
			ut_ad("corrupted index" == 0);
			return false;
		}
	}

	match = 0;

	offsets = rec_get_offsets(rec, cursor->index(), offsets,
				  cursor->index()->n_core_fields,
				  n_unique, &heap);
	cmp = cmp_dtuple_rec_with_match(tuple, rec, offsets, &match);

	if (mode == PAGE_CUR_GE) {
		if (cmp > 0) {
			goto exit_func;
		}

		cursor->up_match = match;

		if (match >= n_unique) {
			success = true;
			goto exit_func;
		}
	} else if (mode == PAGE_CUR_LE) {
		if (cmp < 0) {
			goto exit_func;
		}

		cursor->low_match = match;

	} else if (mode == PAGE_CUR_G) {
		if (cmp >= 0) {
			goto exit_func;
		}
	} else if (mode == PAGE_CUR_L) {
		if (cmp <= 0) {
			goto exit_func;
		}
	}

	if (can_only_compare_to_cursor_rec) {
		/* Since we could not determine if our guess is right just by
		looking at the record under the cursor, return FALSE */
		goto exit_func;
	}

	match = 0;

	if ((mode == PAGE_CUR_G) || (mode == PAGE_CUR_GE)) {
		const rec_t* prev_rec = page_rec_get_prev(rec);

		if (UNIV_UNLIKELY(!prev_rec)) {
			ut_ad("corrupted index" == 0);
			goto exit_func;
		}

		if (page_rec_is_infimum(prev_rec)) {
			success = !page_has_prev(page_align(prev_rec));
			goto exit_func;
		}

		if (cursor->index()->table->not_redundant()) {
			switch (rec_get_status(prev_rec)) {
			case REC_STATUS_INSTANT:
			case REC_STATUS_ORDINARY:
				break;
			default:
				ut_ad("corrupted index" == 0);
				goto exit_func;
			}
		}

		offsets = rec_get_offsets(prev_rec, cursor->index(), offsets,
					  cursor->index()->n_core_fields,
					  n_unique, &heap);
		cmp = cmp_dtuple_rec_with_match(
			tuple, prev_rec, offsets, &match);
		if (mode == PAGE_CUR_GE) {
			success = cmp > 0;
		} else {
			success = cmp >= 0;
		}
	} else {
		ut_ad(!page_rec_is_supremum(rec));

		const rec_t* next_rec = page_rec_get_next(rec);

		if (UNIV_UNLIKELY(!next_rec)) {
			ut_ad("corrupted index" == 0);
			goto exit_func;
		}

		if (page_rec_is_supremum(next_rec)) {
			if (!page_has_next(page_align(next_rec))) {
				cursor->up_match = 0;
				success = true;
			}

			goto exit_func;
		}

		if (cursor->index()->table->not_redundant()) {
			switch (rec_get_status(next_rec)) {
			case REC_STATUS_INSTANT:
			case REC_STATUS_ORDINARY:
				break;
			default:
				ut_ad("corrupted index" == 0);
				goto exit_func;
			}
		}

		offsets = rec_get_offsets(next_rec, cursor->index(), offsets,
					  cursor->index()->n_core_fields,
					  n_unique, &heap);
		cmp = cmp_dtuple_rec_with_match(
			tuple, next_rec, offsets, &match);
		if (mode == PAGE_CUR_LE) {
			success = cmp < 0;
			cursor->up_match = match;
		} else {
			success = cmp <= 0;
		}
	}
exit_func:
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
	return(success);
}

/** Clear the adaptive hash index on all pages in the buffer pool. */
inline void buf_pool_t::clear_hash_index()
{
  ut_ad(!resizing);
  ut_ad(!btr_search.enabled);

  std::set<dict_index_t*> garbage;

  for (chunk_t *chunk= chunks + n_chunks; chunk-- != chunks; )
  {
    for (buf_block_t *block= chunk->blocks, * const end= block + chunk->size;
         block != end; block++)
    {
      dict_index_t *index= block->index;
      assert_block_ahi_valid(block);

      /* We can clear block->index and block->n_pointers when
      holding all AHI latches exclusively; see the comments in buf0buf.h */

      if (!index)
      {
# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
        ut_a(!block->n_pointers);
# endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
        continue;
      }

      ut_d(const auto s= block->page.state());
      /* Another thread may have set the state to
      REMOVE_HASH in buf_LRU_block_remove_hashed().

      The state change in buf_pool_t::realloc() is not observable
      here, because in that case we would have !block->index.

      In the end, the entire adaptive hash index will be removed. */
      ut_ad(s >= buf_page_t::UNFIXED || s == buf_page_t::REMOVE_HASH);
# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
      block->n_pointers= 0;
# endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
      if (index->freed())
        garbage.insert(index);
      block->index= nullptr;
    }
  }

  for (dict_index_t *index : garbage)
    btr_search_lazy_free(index);
}

/** Get a buffer block from an adaptive hash index pointer.
This function does not return if the block is not identified.
@param ptr  pointer to within a page frame
@return pointer to block, never NULL */
inline buf_block_t* buf_pool_t::block_from_ahi(const byte *ptr) const
{
  chunk_t::map *chunk_map = chunk_t::map_ref;
  ut_ad(chunk_t::map_ref == chunk_t::map_reg);
  ut_ad(!resizing);

  chunk_t::map::const_iterator it= chunk_map->upper_bound(ptr);
  ut_a(it != chunk_map->begin());

  chunk_t *chunk= it == chunk_map->end()
    ? chunk_map->rbegin()->second
    : (--it)->second;

  const size_t offs= size_t(ptr - chunk->blocks->page.frame) >>
    srv_page_size_shift;
  ut_a(offs < chunk->size);

  buf_block_t *block= &chunk->blocks[offs];
  /* buf_pool_t::chunk_t::init() invokes buf_block_init() so that
  block[n].frame == block->page.frame + n * srv_page_size.  Check it. */
  ut_ad(block->page.frame == page_align(ptr));
  /* Read the state of the block without holding hash_lock.
  A state transition to REMOVE_HASH is possible during
  this execution. */
  ut_ad(block->page.state() >= buf_page_t::REMOVE_HASH);

  return block;
}

/** Fold a prefix given as the number of fields of a tuple.
@param tuple   index record
@param cursor  B-tree cursor
@return the folded value */
inline uint32_t dtuple_fold(const dtuple_t* tuple, const btr_cur_t *cursor)
{
  ut_ad(tuple);
  ut_ad(tuple->magic_n == DATA_TUPLE_MAGIC_N);
  ut_ad(dtuple_check_typed(tuple));

  const auto comp= cursor->index()->table->not_redundant();
  uint32_t fold= uint32_t(ut_fold_ull(cursor->index()->id));

  for (unsigned i= 0; i < cursor->n_fields; i++)
  {
    const dfield_t *field= dtuple_get_nth_field(tuple, i);
    const void *data= dfield_get_data(field);
    size_t len= dfield_get_len(field);
    if (len == UNIV_SQL_NULL)
    {
      if (UNIV_UNLIKELY(!comp))
      {
        len= dtype_get_sql_null_size(dfield_get_type(field), 0);
        data= field_ref_zero;
      }
      else
        continue;
    }
    fold= my_crc32c(fold, data, len);
  }

  if (size_t n_bytes= cursor->n_bytes)
  {
    const dfield_t *field= dtuple_get_nth_field(tuple, cursor->n_fields);
    const void *data= dfield_get_data(field);
    size_t len= dfield_get_len(field);
    if (len == UNIV_SQL_NULL)
    {
      if (UNIV_UNLIKELY(!comp))
      {
        len= dtype_get_sql_null_size(dfield_get_type(field), 0);
        data= field_ref_zero;
      }
      else
        return fold;
    }
    fold= my_crc32c(fold, data, std::min(n_bytes, len));
  }

  return fold;
}

/** Tries to guess the right search position based on the hash search info
of the index. Note that if mode is PAGE_CUR_LE, which is used in inserts,
and the function returns TRUE, then cursor->up_match and cursor->low_match
both have sensible values.
@param[in,out]	index		index
@param[in]	tuple		logical record
@param[in]	mode		PAGE_CUR_L, ....
@param[in]	latch_mode	BTR_SEARCH_LEAF, ...
@param[out]	cursor		tree cursor
@param[in]	mtr		mini-transaction
@return whether the search succeeded */
TRANSACTIONAL_TARGET
bool
btr_search_guess_on_hash(
	dict_index_t*	index,
	const dtuple_t*	tuple,
	ulint		mode,
	ulint		latch_mode,
	btr_cur_t*	cursor,
	mtr_t*		mtr)
{
	ut_ad(mtr->is_active());
	ut_ad(index->is_btree() || index->is_ibuf());

	/* Note that, for efficiency, the search_info may not be protected by
	any latch here! */

	if (latch_mode > BTR_MODIFY_LEAF
	    || !index->search_info.last_hash_succ
	    || !index->search_info.n_hash_potential
	    || (tuple->info_bits & REC_INFO_MIN_REC_FLAG)) {
		return false;
	}

	ut_ad(index->is_btree());
        ut_ad(!index->table->is_temporary());

	ut_ad(latch_mode == BTR_SEARCH_LEAF || latch_mode == BTR_MODIFY_LEAF);
	compile_time_assert(ulint{BTR_SEARCH_LEAF} == ulint{RW_S_LATCH});
	compile_time_assert(ulint{BTR_MODIFY_LEAF} == ulint{RW_X_LATCH});

	cursor->n_fields = index->search_info.n_fields;
	cursor->n_bytes = index->search_info.n_bytes;

	if (dtuple_get_n_fields(tuple) < btr_search_get_n_fields(cursor)) {
		return false;
	}

	const index_id_t index_id = index->id;

#ifdef UNIV_SEARCH_PERF_STAT
	index->search_info.n_hash_succ++;
#endif
	const uint32_t fold = dtuple_fold(tuple, cursor);

	cursor->fold = fold;
	cursor->flag = BTR_CUR_HASH;

	auto part = &btr_search.parts;

	part->latch.rd_lock(SRW_LOCK_CALL);

	if (!btr_search.enabled) {
ahi_release_and_fail:
		part->latch.rd_unlock();
fail:
		cursor->flag = BTR_CUR_HASH_FAIL;

#ifdef UNIV_SEARCH_PERF_STAT
		++index->search_info.n_hash_fail;
		if (index->search_info.n_hash_succ > 0) {
			--index->search_info.n_hash_succ;
		}
#endif /* UNIV_SEARCH_PERF_STAT */

		index->search_info.last_hash_succ = FALSE;
		return false;
	}

	const ahi_node* node
		= static_cast<ahi_node*>(part->table.cell_get(fold)->node);

	for (; node; node = node->next) {
		if (node->fold == fold) {
			goto found;
		}
	}

	goto ahi_release_and_fail;

found:
	const rec_t* rec = node->rec;
	buf_block_t* block = buf_pool.block_from_ahi(rec);
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	ut_a(block == node->block);
#endif
	buf_pool_t::hash_chain& chain = buf_pool.page_hash.cell_get(
		block->page.id().fold());
	bool got_latch;
	{
		transactional_shared_lock_guard<page_hash_latch> g{
			buf_pool.page_hash.lock_get(chain)};
		got_latch = (latch_mode == BTR_SEARCH_LEAF)
			? block->page.lock.s_lock_try()
			: block->page.lock.x_lock_try();
	}

	if (!got_latch) {
		goto ahi_release_and_fail;
	}

	const auto state = block->page.state();
	if (UNIV_UNLIKELY(state < buf_page_t::UNFIXED)) {
		ut_ad(state == buf_page_t::REMOVE_HASH);
block_and_ahi_release_and_fail:
		if (latch_mode == BTR_SEARCH_LEAF) {
			block->page.lock.s_unlock();
		} else {
			block->page.lock.x_unlock();
		}
		goto ahi_release_and_fail;
	}

	ut_ad(state < buf_page_t::READ_FIX || state >= buf_page_t::WRITE_FIX);
	ut_ad(state < buf_page_t::READ_FIX || latch_mode == BTR_SEARCH_LEAF);

	if (index != block->index && index_id == block->index->id) {
		ut_a(block->index->freed());
		goto block_and_ahi_release_and_fail;
	}

	block->page.fix();
	buf_page_make_young_if_needed(&block->page);
	static_assert(ulint{MTR_MEMO_PAGE_S_FIX} == ulint{BTR_SEARCH_LEAF},
		      "");
	static_assert(ulint{MTR_MEMO_PAGE_X_FIX} == ulint{BTR_MODIFY_LEAF},
		      "");

	part->latch.rd_unlock();

	++buf_pool.stat.n_page_gets;

	mtr->memo_push(block, mtr_memo_type_t(latch_mode));

	ut_ad(page_rec_is_user_rec(rec));

	btr_cur_position(index, const_cast<rec_t*>(rec), block, cursor);

	/* Check the validity of the guess within the page */

	/* If we only have the latch on search system, not on the
	page, it only protects the columns of the record the cursor
	is positioned on. We cannot look at the next of the previous
	record to determine if our guess for the cursor position is
	right. */
	if (index_id != btr_page_get_index_id(block->page.frame)
	    || !btr_search_check_guess(cursor, false, tuple, mode)) {
		mtr->release_last_page();
		goto fail;
	}

	if (index->search_info.n_hash_potential < BTR_SEARCH_BUILD_LIMIT + 5) {
		index->search_info.n_hash_potential++;
	}

	index->search_info.last_hash_succ = TRUE;

#ifdef UNIV_SEARCH_PERF_STAT
	btr_search_n_succ++;
#endif
	return true;
}

/** Drop any adaptive hash index entries that point to an index page.
@param[in,out]	block	block containing index page, s- or x-latched, or an
			index page for which we know that
			block->buf_fix_count == 0 or it is an index page which
			has already been removed from the buf_pool.page_hash
			i.e.: it is in state BUF_BLOCK_REMOVE_HASH
@param[in]	garbage_collect	drop ahi only if the index is marked
				as freed */
void btr_search_drop_page_hash_index(buf_block_t* block,
				     bool garbage_collect) noexcept
{
	ulint			n_fields;
	ulint			n_bytes;

retry:
	if (!block->index) {
		return;
	}

	ut_d(const auto state = block->page.state());
	ut_ad(state == buf_page_t::REMOVE_HASH
	      || state >= buf_page_t::UNFIXED);
	ut_ad(state == buf_page_t::REMOVE_HASH
	      || !(~buf_page_t::LRU_MASK & state)
	      || block->page.lock.have_any());
	ut_ad(state < buf_page_t::READ_FIX || state >= buf_page_t::WRITE_FIX);
	ut_ad(page_is_leaf(block->page.frame));

	/* We must not dereference block->index here, because it could be freed
	if (!index->table->get_ref_count() && !dict_sys.frozen()).
	Determine the ahi_slot based on the block contents. */

	const index_id_t	index_id
		= btr_page_get_index_id(block->page.frame);

	auto part = &btr_search.parts;

	part->latch.rd_lock(SRW_LOCK_CALL);

	dict_index_t* index = block->index;
	bool is_freed = index && index->freed();

	if (is_freed) {
		part->latch.rd_unlock();
		part->latch.wr_lock(SRW_LOCK_CALL);
		if (index != block->index) {
			part->latch.wr_unlock();
			goto retry;
		}
	} else if (garbage_collect) {
		part->latch.rd_unlock();
		return;
	}

	assert_block_ahi_valid(block);

	if (!index || !btr_search.enabled) {
		if (is_freed) {
			part->latch.wr_unlock();
		} else {
			part->latch.rd_unlock();
		}
		return;
	}

	ut_ad(!index->table->is_temporary());
	ut_ad(btr_search.enabled);

	ut_ad(block->page.id().space() == index->table->space_id);
	ut_a(index_id == index->id);
	ut_ad(!dict_index_is_ibuf(index));

	n_fields = block->curr_n_fields;
	n_bytes = block->curr_n_bytes;

	/* NOTE: The AHI fields of block must not be accessed after
	releasing search latch, as the index page might only be s-latched! */

	if (!is_freed) {
		part->latch.rd_unlock();
	}

	ut_a(n_fields > 0 || n_bytes > 0);

	const page_t* const page = block->page.frame;
	ulint n_recs = page_get_n_recs(page);
	if (!n_recs) {
		ut_ad("corrupted adaptive hash index" == 0);
		return;
	}

	/* Calculate and cache fold values into an array for fast deletion
	from the hash index */

	const rec_t *rec = page_get_infimum_rec(page);
	rec = page_rec_get_next_low(rec, page_is_comp(page));

	uint32_t* folds;
	ulint n_cached = 0;
	ulint prev_fold = 0;

	if (rec && rec_is_metadata(rec, *index)) {
		rec = page_rec_get_next_low(rec, page_is_comp(page));
		if (!--n_recs) {
			/* The page only contains the hidden metadata record
			for instant ALTER TABLE that the adaptive hash index
			never points to. */
			folds = nullptr;
			goto all_deleted;
		}
	}

	folds = static_cast<uint32_t*>(
		ut_malloc_nokey(n_recs * sizeof *folds));

	while (rec) {
		if (n_cached >= n_recs) {
			ut_ad(page_rec_is_supremum(rec));
			break;
		}
		ut_ad(page_rec_is_user_rec(rec));
		const uint32_t fold = rec_fold(rec, *index, n_fields, n_bytes);

		if (fold == prev_fold && prev_fold != 0) {

			goto next_rec;
		}

		/* Remove all hash nodes pointing to this page from the
		hash chain */
		folds[n_cached++] = fold;

next_rec:
		rec = page_rec_get_next_low(rec, page_rec_is_comp(rec));
		if (!rec || page_rec_is_supremum(rec)) {
			break;
		}
		prev_fold = fold;
	}

all_deleted:
	if (!is_freed) {
		part->latch.wr_lock(SRW_LOCK_CALL);

		if (UNIV_UNLIKELY(!block->index)) {
			/* Someone else has meanwhile dropped the
			hash index */
			goto cleanup;
		}

		ut_a(block->index == index);
	}

	if (block->curr_n_fields != n_fields
	    || block->curr_n_bytes != n_bytes) {

		/* Someone else has meanwhile built a new hash index on the
		page, with different parameters */

		part->latch.wr_unlock();

		ut_free(folds);
		goto retry;
	}

	for (ulint i = 0; i < n_cached; i++) {
		ha_remove_all_nodes_to_page(&part->table, folds[i], page);
	}

	switch (index->search_info.ref_count--) {
	case 0:
		ut_error;
	case 1:
		if (index->freed()) {
			btr_search_lazy_free(index);
		}
	}

	block->index = nullptr;

	MONITOR_INC(MONITOR_ADAPTIVE_HASH_PAGE_REMOVED);
	MONITOR_INC_VALUE(MONITOR_ADAPTIVE_HASH_ROW_REMOVED, n_cached);

cleanup:
	assert_block_ahi_valid(block);
	part->latch.wr_unlock();

	ut_free(folds);
}

/** Drop possible adaptive hash index entries when a page is evicted
from the buffer pool or freed in a file, or the index is being dropped.
@param[in]	page_id		page id */
void btr_search_drop_page_hash_when_freed(const page_id_t page_id)
{
	buf_block_t*	block;
	mtr_t		mtr;

	mtr_start(&mtr);

	/* If the caller has a latch on the page, then the caller must
	have a x-latch on the page and it must have already dropped
	the hash index for the page. Because of the x-latch that we
	are possibly holding, we cannot s-latch the page, but must
	(recursively) x-latch it, even though we are only reading. */

	block = buf_page_get_gen(page_id, 0, RW_X_LATCH, NULL,
				 BUF_PEEK_IF_IN_POOL, &mtr);

	if (block && block->index) {
		/* In all our callers, the table handle should
		be open, or we should be in the process of
		dropping the table (preventing eviction). */
		DBUG_ASSERT(block->index->table->get_ref_count()
			    || dict_sys.locked());
		btr_search_drop_page_hash_index(block, false);
	}

	mtr_commit(&mtr);
}

/** Build a hash index on a page with the given parameters. If the page already
has a hash index with different parameters, the old hash index is removed.
If index is non-NULL, this function checks if n_fields and n_bytes are
sensible, and does not build a hash index if not.
@param[in,out]	index		index for which to build.
@param[in,out]	block		index page, s-/x- latched.
@param[in,out]	ahi_latch	the adaptive search latch
@param[in]	n_fields	hash this many full fields
@param[in]	n_bytes		hash this many bytes of the next field
@param[in]	left_side	hash for searches from left side */
static
void
btr_search_build_page_hash_index(
	dict_index_t*	index,
	buf_block_t*	block,
	uint16_t	n_fields,
	uint16_t	n_bytes,
	bool		left_side)
{
	const rec_t*	rec;
	ulint		n_cached;
	ulint		n_recs;
	uint32_t*	folds;
	const rec_t**	recs;

	ut_ad(!index->table->is_temporary());

	if (!btr_search.enabled) {
		return;
	}

	ut_ad(index);
	ut_ad(block->page.id().space() == index->table->space_id);
	ut_ad(!dict_index_is_ibuf(index));
	ut_ad(page_is_leaf(block->page.frame));

	ut_ad(block->page.lock.have_x() || block->page.lock.have_s());
	ut_ad(block->page.id().page_no() >= 3);

	btr_search.parts.latch.rd_lock(SRW_LOCK_CALL);

	const bool enabled = btr_search.enabled;
	const bool rebuild = enabled && block->index
		&& (block->curr_n_fields != n_fields
		    || block->curr_n_bytes != n_bytes
		    || block->curr_left_side != left_side);

	btr_search.parts.latch.rd_unlock();

	if (!enabled) {
		return;
	}

	if (rebuild) {
		btr_search_drop_page_hash_index(block, false);
	}

	/* Check that the values for hash index build are sensible */

	if (n_fields == 0 && n_bytes == 0) {

		return;
	}

	if (dict_index_get_n_unique_in_tree(index)
	    < btr_search_get_n_fields(n_fields, n_bytes)) {
		return;
	}

	page_t*		page	= buf_block_get_frame(block);
	n_recs = page_get_n_recs(page);

	if (n_recs == 0) {

		return;
	}

	rec = page_rec_get_next_const(page_get_infimum_rec(page));
        if (!rec) return;

	if (rec_is_metadata(rec, *index)) {
		rec = page_rec_get_next_const(rec);
		if (!rec || !--n_recs) return;
	}
	if (page_rec_is_supremum(rec)) return;

	/* Calculate and cache fold values and corresponding records into
	an array for fast insertion to the hash index */

	folds = static_cast<uint32_t*>(ut_malloc_nokey(n_recs * sizeof *folds));
	recs = static_cast<const rec_t**>(
		ut_malloc_nokey(n_recs * sizeof *recs));

	n_cached = 0;

	ut_a(index->id == btr_page_get_index_id(page));

	uint32_t fold = rec_fold(rec, *index, n_fields, n_bytes);

	if (left_side) {
		folds[n_cached] = fold;
		recs[n_cached] = rec;
		n_cached++;
	}

	while (const rec_t* next_rec = page_rec_get_next_const(rec)) {
		if (page_rec_is_supremum(next_rec)) {

			if (!left_side) {

				folds[n_cached] = fold;
				recs[n_cached] = rec;
				n_cached++;
			}

			break;
		}

		uint32_t next_fold = rec_fold(next_rec, *index, n_fields,
					      n_bytes);

		if (fold != next_fold) {
			/* Insert an entry into the hash index */

			if (left_side) {

				folds[n_cached] = next_fold;
				recs[n_cached] = next_rec;
				n_cached++;
			} else {
				folds[n_cached] = fold;
				recs[n_cached] = rec;
				n_cached++;
			}
		}

		rec = next_rec;
		fold = next_fold;
	}

	btr_search.parts.prepare_insert();

	btr_search.parts.latch.wr_lock(SRW_LOCK_CALL);

	if (!btr_search.enabled) {
		goto exit_func;
	}

	/* This counter is decremented every time we drop page
	hash index entries and is incremented here. Since we can
	rebuild hash index for a page that is already hashed, we
	have to take care not to increment the counter in that
	case. */
	if (!block->index) {
		assert_block_ahi_empty(block);
		index->search_info.ref_count++;
	} else if (block->curr_n_fields != n_fields
		   || block->curr_n_bytes != n_bytes
		   || block->curr_left_side != left_side) {
		goto exit_func;
	}

	block->n_hash_helps = 0;

	block->curr_n_fields = n_fields & dict_index_t::MAX_N_FIELDS;
	block->curr_n_bytes = n_bytes & ((1U << 15) - 1);
	block->curr_left_side = left_side;
	block->index = index;

	for (ulint i = 0; i < n_cached; i++) {
		ha_insert_for_fold(&btr_search.parts,
				   folds[i], block, recs[i]);
	}

	MONITOR_INC(MONITOR_ADAPTIVE_HASH_PAGE_ADDED);
	MONITOR_INC_VALUE(MONITOR_ADAPTIVE_HASH_ROW_ADDED, n_cached);
exit_func:
	assert_block_ahi_valid(block);
	btr_search.parts.latch.wr_unlock();

	ut_free(folds);
	ut_free(recs);
}

void btr_cur_t::search_info_update() const noexcept
{
	/* NOTE that the following two function calls do NOT protect
	info or block->n_fields etc. with any semaphore, to save CPU time!
	We cannot assume the fields are consistent when we return from
	those functions! */

	btr_search_info_update_hash(this);

	bool build_index = btr_search_update_block_hash_info(
		&index()->search_info, page_cur.block);

	if (build_index || flag == BTR_CUR_HASH_FAIL) {
		btr_search.parts.prepare_insert();
	}

	if (flag == BTR_CUR_HASH_FAIL) {
		/* Update the hash node reference, if appropriate */
#ifdef UNIV_SEARCH_PERF_STAT
		btr_search_n_hash_fail++;
#endif /* UNIV_SEARCH_PERF_STAT */
		btr_search_update_hash_ref(this);
	}

	if (build_index) {
		/* Note that since we did not protect block->n_fields etc.
		with any semaphore, the values can be inconsistent. We have
		to check inside the function call that they make sense. */
		btr_search_build_page_hash_index(index(), page_cur.block,
						 page_cur.block->n_fields,
						 page_cur.block->n_bytes,
						 page_cur.block->left_side);
	}
}

/** Move or delete hash entries for moved records, usually in a page split.
If new_block is already hashed, then any hash index for block is dropped.
If new_block is not hashed, and block is hashed, then a new hash index is
built to new_block with the same parameters as block.
@param[in,out]	new_block	destination page
@param[in,out]	block		source page (subject to deletion later) */
void
btr_search_move_or_delete_hash_entries(
	buf_block_t*	new_block,
	buf_block_t*	block)
{
	ut_ad(block->page.lock.have_x());
	ut_ad(new_block->page.lock.have_x());

	if (!btr_search.enabled) {
		return;
	}

	dict_index_t* index = block->index;
	if (!index) {
		index = new_block->index;
	} else {
		ut_ad(!new_block->index || index == new_block->index);
	}
	assert_block_ahi_valid(block);
	assert_block_ahi_valid(new_block);

	if (new_block->index) {
drop_exit:
		btr_search_drop_page_hash_index(block, false);
		return;
	}

	if (!index) {
		return;
	}

	srw_spin_lock* ahi_latch = &btr_search.parts.latch;
	ahi_latch->rd_lock(SRW_LOCK_CALL);

	if (index->freed()) {
		ahi_latch->rd_unlock();
		goto drop_exit;
	}

	if (block->index) {
		uint16_t n_fields = block->curr_n_fields;
		uint16_t n_bytes = block->curr_n_bytes;
		bool left_side = block->curr_left_side;

		new_block->n_fields = block->curr_n_fields;
		new_block->n_bytes = block->curr_n_bytes;
		new_block->left_side = left_side;

		ahi_latch->rd_unlock();

		ut_a(n_fields > 0 || n_bytes > 0);

		btr_search_build_page_hash_index(
			index, new_block,
			n_fields, n_bytes, left_side);
		ut_ad(n_fields == block->curr_n_fields);
		ut_ad(n_bytes == block->curr_n_bytes);
		ut_ad(left_side == block->curr_left_side);
		return;
	}

	ahi_latch->rd_unlock();
}

/** Updates the page hash index when a single record is deleted from a page.
@param[in]	cursor	cursor which was positioned on the record to delete
			using btr_cur_search_, the record is not yet deleted.*/
void btr_search_update_hash_on_delete(btr_cur_t *cursor)
{
	buf_block_t*	block;
	const rec_t*	rec;
	dict_index_t*	index;

	ut_ad(page_is_leaf(btr_cur_get_page(cursor)));

	if (!btr_search.enabled) {
		return;
	}

	block = btr_cur_get_block(cursor);

	ut_ad(block->page.lock.have_x());

	assert_block_ahi_valid(block);
	index = block->index;

	if (!index) {

		return;
	}

	ut_ad(!cursor->index()->table->is_temporary());

	if (index != cursor->index()) {
		btr_search_drop_page_hash_index(block, false);
		return;
	}

	ut_ad(block->page.id().space() == index->table->space_id);
	ut_a(index == cursor->index());
	ut_a(block->curr_n_fields > 0 || block->curr_n_bytes > 0);
	ut_ad(!dict_index_is_ibuf(index));

	rec = btr_cur_get_rec(cursor);

	uint32_t fold = rec_fold(rec, *index,
                                 block->curr_n_fields, block->curr_n_bytes);

	auto part = &btr_search.parts;

	part->latch.wr_lock(SRW_LOCK_CALL);
	assert_block_ahi_valid(block);

	if (block->index && btr_search.enabled) {
		ut_a(block->index == index);

		if (part->erase(fold, rec)) {
			MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_REMOVED);
		} else {
			MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_REMOVE_NOT_FOUND);
		}
	} else {
		part->latch.wr_unlock();
	}
}

/** Updates the page hash index when a single record is inserted on a page.
@param[in]	cursor	cursor which was positioned to the place to insert
			using btr_cur_search_, and the new record has been
			inserted next to the cursor. */
void btr_search_update_hash_node_on_insert(btr_cur_t *cursor)
{
	buf_block_t*	block;
	dict_index_t*	index;
	rec_t*		rec;

	if (!btr_search.enabled) {
		return;
	}

	rec = btr_cur_get_rec(cursor);

	block = btr_cur_get_block(cursor);

	ut_ad(block->page.lock.have_x());

	index = block->index;

	if (!index) {

		return;
	}

	ut_ad(!cursor->index()->table->is_temporary());

	if (index != cursor->index()) {
		ut_ad(index->id == cursor->index()->id);
		btr_search_drop_page_hash_index(block, false);
		return;
	}

	ut_a(cursor->index() == index);
	ut_ad(!dict_index_is_ibuf(index));
	btr_search.parts.latch.wr_lock(SRW_LOCK_CALL);

	if (!block->index || !btr_search.enabled) {

		goto func_exit;
	}

	ut_a(block->index == index);

	if ((cursor->flag == BTR_CUR_HASH)
	    && (cursor->n_fields == block->curr_n_fields)
	    && (cursor->n_bytes == block->curr_n_bytes)
	    && !block->curr_left_side) {
		if (const rec_t *new_rec = page_rec_get_next_const(rec)) {
			if (ha_search_and_update_if_found(
				&btr_search.parts.table,
				cursor->fold, rec, block, new_rec)) {
				MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_UPDATED);
			}
		} else {
			ut_ad("corrupted page" == 0);
		}

func_exit:
		assert_block_ahi_valid(block);
		btr_search.parts.latch.wr_unlock();
	} else {
		btr_search.parts.latch.wr_unlock();
		btr_search_update_hash_on_insert(cursor);
	}
}

/** Updates the page hash index when a single record is inserted on a page.
@param[in,out]	cursor		cursor which was positioned to the
				place to insert using btr_cur_search_...,
				and the new record has been inserted next
				to the cursor */
void btr_search_update_hash_on_insert(btr_cur_t *cursor)
{
	buf_block_t*	block;
	dict_index_t*	index;
	const rec_t*	rec;
	const rec_t*	ins_rec;
	const rec_t*	next_rec;
	ulint		n_fields;
	ulint		n_bytes;

	ut_ad(page_is_leaf(btr_cur_get_page(cursor)));

	if (!btr_search.enabled) {
		return;
	}

	block = btr_cur_get_block(cursor);

	ut_ad(block->page.lock.have_x());
	assert_block_ahi_valid(block);

	index = block->index;

	if (!index) {

		return;
	}

	ut_ad(block->page.id().space() == index->table->space_id);

	rec = btr_cur_get_rec(cursor);

	ut_ad(!cursor->index()->table->is_temporary());

	if (index != cursor->index()) {
		ut_ad(index->id == cursor->index()->id);
drop:
		btr_search_drop_page_hash_index(block, false);
		return;
	}

	ut_a(index == cursor->index());
	ut_ad(!dict_index_is_ibuf(index));

	n_fields = block->curr_n_fields;
	n_bytes = block->curr_n_bytes;
	const bool left_side = block->curr_left_side;

	ins_rec = page_rec_get_next_const(rec);
	if (UNIV_UNLIKELY(!ins_rec)) goto drop;
	next_rec = page_rec_get_next_const(ins_rec);
	if (UNIV_UNLIKELY(!next_rec)) goto drop;

	uint32_t ins_fold = rec_fold(ins_rec, *index, n_fields, n_bytes);
	uint32_t next_fold = 0, fold;

	if (!page_rec_is_supremum(next_rec)) {
		next_fold = rec_fold(next_rec, *index, n_fields, n_bytes);
	}

	btr_sea::partition* const part= &btr_search.parts;
	bool locked = false;
	part->prepare_insert();

	if (!page_rec_is_infimum(rec) && !rec_is_metadata(rec, *index)) {
		fold = rec_fold(rec, *index, n_fields, n_bytes);
	} else {
		if (left_side) {
			locked = true;
			btr_search.parts.latch.wr_lock(SRW_LOCK_CALL);

			if (!btr_search.enabled || !block->index) {
				goto function_exit;
			}

			ha_insert_for_fold(part, ins_fold, block, ins_rec);
			MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
		}

		goto check_next_rec;
	}

	if (fold != ins_fold) {

		if (!locked) {
			locked = true;
			btr_search.parts.latch.wr_lock(SRW_LOCK_CALL);

			if (!btr_search.enabled || !block->index) {
				goto function_exit;
			}
		}

		if (!left_side) {
			ha_insert_for_fold(part, fold, block, rec);
		} else {
			ha_insert_for_fold(part, ins_fold, block, ins_rec);
		}
		MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
	}

check_next_rec:
	if (page_rec_is_supremum(next_rec)) {

		if (!left_side) {
			if (!locked) {
				locked = true;
				btr_search.parts.latch.wr_lock(
					SRW_LOCK_CALL);

				if (!btr_search.enabled || !block->index) {
					goto function_exit;
				}
			}

			ha_insert_for_fold(part, ins_fold, block, ins_rec);
			MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
		}

		goto function_exit;
	}

	if (ins_fold != next_fold) {
		if (!locked) {
			locked = true;
			btr_search.parts.latch.wr_lock(SRW_LOCK_CALL);

			if (!btr_search.enabled || !block->index) {
				goto function_exit;
			}
		}

		if (!left_side) {
			ha_insert_for_fold(part, ins_fold, block, ins_rec);
		} else {
			ha_insert_for_fold(part, next_fold, block, next_rec);
		}
		MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
	}

function_exit:
	if (locked) {
		btr_search.parts.latch.wr_unlock();
	}
}

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
__attribute__((nonnull))
/** @return whether a range of the cells is valid */
static bool ha_validate(const hash_table_t *table,
                        ulint start_index, ulint end_index)
{
  ut_a(start_index <= end_index);
  ut_a(end_index < table->n_cells);

  bool ok= true;

  for (ulint i= start_index; i <= end_index; i++)
  {
    for (auto node= static_cast<const ahi_node*>(table->array[i].node); node;
         node= node->next)
    {
      if (table->calc_hash(node->fold) != i) {
        ib::error() << "Hash table node fold value " << node->fold
		    << " does not match the cell number " << i;
	ok= false;
      }
    }
  }

  return ok;
}

/** Validates the search system.
@param thd   connection, for checking if CHECK TABLE has been killed
@return true if ok */
bool btr_search_validate(THD *thd)
{
	ahi_node*	node;
	bool		ok		= true;
	ulint		i;
	ulint		cell_count;

	btr_search_x_lock_all();
	if (!btr_search.enabled || (thd && thd_kill_level(thd))) {
func_exit:
		btr_search_x_unlock_all();
		return ok;
	}

	/* How many cells to check before temporarily releasing
	search latches. */
	ulint		chunk_size = 10000;

	mysql_mutex_lock(&buf_pool.mutex);

	auto &part = btr_search.parts;

	cell_count = part.table.n_cells;

	for (i = 0; i < cell_count; i++) {
		/* We release search latches every once in a while to
		give other queries a chance to run. */
		if ((i != 0) && ((i % chunk_size) == 0)) {

			mysql_mutex_unlock(&buf_pool.mutex);
			btr_search_x_unlock_all();

			std::this_thread::yield();

			btr_search_x_lock_all();

			if (!btr_search.enabled
			    || (thd && thd_kill_level(thd))) {
				goto func_exit;
			}

			mysql_mutex_lock(&buf_pool.mutex);

			ulint curr_cell_count = part.table.n_cells;

			if (cell_count != curr_cell_count) {

				cell_count = curr_cell_count;

				if (i >= cell_count) {
					break;
				}
			}
		}

		node = static_cast<ahi_node*>(part.table.array[i].node);

		for (; node != NULL; node = node->next) {
			const buf_block_t*	block
				= buf_pool.block_from_ahi(node->rec);
			index_id_t		page_index_id;

			if (UNIV_LIKELY(block->page.in_file())) {
				/* The space and offset are only valid
				for file blocks.  It is possible that
				the block is being freed
				(BUF_BLOCK_REMOVE_HASH, see the
				assertion and the comment below) */
				const page_id_t id(block->page.id());
				if (const buf_page_t* hash_page
				    = buf_pool.page_hash.get(
					    id, buf_pool.page_hash.cell_get(
						    id.fold()))) {
					ut_ad(hash_page == &block->page);
					goto state_ok;
				}
			}

			/* When a block is being freed,
			buf_LRU_search_and_free_block() first removes
			the block from buf_pool.page_hash by calling
			buf_LRU_block_remove_hashed_page(). Then it
			invokes btr_search_drop_page_hash_index(). */
			ut_a(block->page.state() == buf_page_t::REMOVE_HASH);
state_ok:
			ut_ad(!dict_index_is_ibuf(block->index));
			ut_ad(block->page.id().space()
			      == block->index->table->space_id);

			const page_t* page = block->page.frame;

			page_index_id = btr_page_get_index_id(page);

			const uint32_t fold = rec_fold(
				node->rec, *block->index,
				block->curr_n_fields,
				block->curr_n_bytes);

			if (node->fold != fold) {
				ok = FALSE;

				ib::error() << "Error in an adaptive hash"
					<< " index pointer to page "
					<< block->page.id()
					<< ", ptr mem address "
					<< reinterpret_cast<const void*>(
						node->rec)
					<< ", index id " << page_index_id
					<< ", node fold " << node->fold
					<< ", rec fold " << fold;
				ut_ad(0);
			}
		}
	}

	for (i = 0; i < cell_count; i += chunk_size) {
		/* We release search latches every once in a while to
		give other queries a chance to run. */
		if (i != 0) {
			mysql_mutex_unlock(&buf_pool.mutex);
			btr_search_x_unlock_all();

			std::this_thread::yield();

			btr_search_x_lock_all();

			if (!btr_search.enabled
			    || (thd && thd_kill_level(thd))) {
				goto func_exit;
			}

			mysql_mutex_lock(&buf_pool.mutex);

			ulint curr_cell_count = part.table.n_cells;

			if (cell_count != curr_cell_count) {

				cell_count = curr_cell_count;

				if (i >= cell_count) {
					break;
				}
			}
		}

		ulint end_index = ut_min(i + chunk_size - 1, cell_count - 1);

		if (!ha_validate(&part.table, i, end_index)) {
			ok = false;
		}
	}

	mysql_mutex_unlock(&buf_pool.mutex);
	goto func_exit;
}

#ifdef UNIV_DEBUG
bool btr_search_check_marked_free_index(const buf_block_t *block)
{
  btr_search_s_lock_all();
  bool is_freed= block->index && block->index->freed();
  btr_search_s_unlock_all();
  return is_freed;
}
#endif /* UNIV_DEBUG */
#endif /* defined UNIV_AHI_DEBUG || defined UNIV_DEBUG */
#endif /* BTR_CUR_HASH_ADAPT */
