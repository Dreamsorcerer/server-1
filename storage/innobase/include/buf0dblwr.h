/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

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

/**************************************************//**
@file include/buf0dblwr.h
Doublewrite buffer module

Created 2011/12/19 Inaam Rana
*******************************************************/

#ifndef buf0dblwr_h
#define buf0dblwr_h

#include "ut0byte.h"
#include "log0log.h"
#include "buf0types.h"
#include "log0recv.h"

/** Doublewrite system */
extern buf_dblwr_t*	buf_dblwr;

/** Create the doublewrite buffer if the doublewrite buffer header
is not present in the TRX_SYS page.
@return	whether the operation succeeded
@retval	true	if the doublewrite buffer exists or was created
@retval	false	if the creation failed (too small first data file) */
MY_ATTRIBUTE((warn_unused_result))
bool
buf_dblwr_create();

/**
At database startup initializes the doublewrite buffer memory structure if
we already have a doublewrite buffer created in the data files. If we are
upgrading to an InnoDB version which supports multiple tablespaces, then this
function performs the necessary update operations. If we are in a crash
recovery, this function loads the pages from double write buffer into memory.
@param[in]	file		File handle
@param[in]	path		Path name of file
@return DB_SUCCESS or error code */
dberr_t
buf_dblwr_init_or_load_pages(
	pfs_os_file_t	file,
	const char*	path);

/** Process and remove the double write buffer pages for all tablespaces. */
void
buf_dblwr_process();

/****************************************************************//**
frees doublewrite buffer. */
void
buf_dblwr_free();

/** Update the doublewrite buffer on write completion. */
void buf_dblwr_update(const buf_page_t &bpage);
/****************************************************************//**
Determines if a page number is located inside the doublewrite buffer.
@return TRUE if the location is inside the two blocks of the
doublewrite buffer */
ibool
buf_dblwr_page_inside(
/*==================*/
	ulint	page_no);	/*!< in: page number */

/********************************************************************//**
Flushes possible buffered writes from the doublewrite memory buffer to disk.
It is very important to call this function after a batch of writes
has been posted, and also when we may have to wait for a page latch!
Otherwise a deadlock of threads can occur. */
void
buf_dblwr_flush_buffered_writes();

/** Doublewrite control struct */
struct buf_dblwr_t{
  /** mutex protecting first_free, write_buf */
  mysql_mutex_t	mutex;
	ulint		block1;	/*!< the page number of the first
				doublewrite block (64 pages) */
	ulint		block2;	/*!< page number of the second block */
	ulint		first_free;/*!< first free position in write_buf
				measured in units of srv_page_size */
	ulint		b_reserved;/*!< number of slots currently reserved
				for batch flush. */
	os_event_t	b_event;/*!< event where threads wait for a
				batch flush to end;
				os_event_set() and os_event_reset()
				are protected by buf_dblwr_t::mutex */
	bool		batch_running;/*!< set to TRUE if currently a batch
				is being written from the doublewrite
				buffer. */
	byte*		write_buf;/*!< write buffer used in writing to the
				doublewrite buffer, aligned to an
				address divisible by srv_page_size
				(which is required by Windows aio) */

  struct element
  {
    /** block descriptor */
    buf_page_t *bpage;
    /** true=buf_pool.flush_list, false=buf_pool.LRU */
    bool lru;
    /** payload size in bytes */
    size_t size;
  };

  /** buffer blocks to be written via write_buf */
  element *buf_block_arr;

  /** Schedule a page write. If the doublewrite memory buffer is full,
  buf_dblwr_flush_buffered_writes() will be invoked to make space.
  @param bpage      buffer pool page to be written
  @param lru        true=buf_pool.LRU; false=buf_pool.flush_list
  @param size       payload size in bytes */
  void add_to_batch(buf_page_t *bpage, bool lru, size_t size);
};

#endif
