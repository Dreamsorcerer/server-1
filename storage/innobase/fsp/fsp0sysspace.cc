/*****************************************************************************

Copyright (c) 2013, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2022, MariaDB Corporation.

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
@file fsp/fsp0space.cc
Multi file, shared, system tablespace implementation.

Created 2012-11-16 by Sunny Bains as srv/srv0space.cc
Refactored 2013-7-26 by Kevin Lewis
*******************************************************/

#include "fsp0sysspace.h"
#include "srv0start.h"
#include "trx0sys.h"
#include "dict0load.h"
#include "mem0mem.h"
#include "os0file.h"
#include "row0mysql.h"
#include "buf0dblwr.h"
#include "log.h"

/** The server header file is included to access opt_initialize global variable.
If server passes the option for create/open DB to SE, we should remove such
direct reference to server header and global variable */
#include "mysqld.h"

/** The control info of the system tablespace. */
SysTablespace srv_sys_space;

/** The control info of a temporary table shared tablespace. */
SysTablespace srv_tmp_space;

/** If the last data file is auto-extended, we add this many pages to it
at a time. We have to make this public because it is a config variable. */
uint sys_tablespace_auto_extend_increment;

/** Convert a numeric string that optionally ends in G or M or K,
    to a number containing megabytes.
@param[in]	str	String with a quantity in bytes
@param[out]	megs	The number in megabytes
@return next character in string */
char*
SysTablespace::parse_units(
	char*	ptr,
	ulint*	megs)
{
	char*		endp;

	*megs = strtoul(ptr, &endp, 10);

	ptr = endp;

	switch (*ptr) {
	case 'G': case 'g':
		*megs *= 1024;
		/* fall through */
	case 'M': case 'm':
		++ptr;
		break;
	case 'K': case 'k':
		*megs /= 1024;
		++ptr;
		break;
	default:
		*megs /= 1024 * 1024;
		break;
	}

	return(ptr);
}

/** Parse the input params and populate member variables.
@param[in]	filepath	path to data files
@param[in]	supports_raw	true if the tablespace supports raw devices
@return true on success parse */
bool
SysTablespace::parse_params(
	const char*	filepath_spec,
	bool		supports_raw)
{
	char*	filepath;
	ulint	size;
	char*	input_str;
	ulint	n_files = 0;

	ut_ad(m_last_file_size_max == 0);
	ut_ad(!m_auto_extend_last_file);

	char*	new_str = mem_strdup(filepath_spec);
	char*	str = new_str;

	input_str = str;

	/*---------------------- PASS 1 ---------------------------*/
	/* First calculate the number of data files and check syntax:
	filepath:size[K |M | G];filepath:size[K |M | G]... .
	Note that a Windows path may contain a drive name and a ':'. */
	while (*str != '\0') {
		filepath = str;

		while ((*str != ':' && *str != '\0')
		       || (*str == ':'
			   && (*(str + 1) == '\\' || *(str + 1) == '/'
			       || *(str + 1) == ':'))) {
			str++;
		}

		if (*str == '\0') {
			ut_free(new_str);

			ib::error()
				<< "syntax error in file path or size"
				" specified is less than 1 megabyte";
			return(false);
		}

		str++;

		str = parse_units(str, &size);

		if (0 == strncmp(str, ":autoextend",
				 (sizeof ":autoextend") - 1)) {

			str += (sizeof ":autoextend") - 1;

			if (0 == strncmp(str, ":max:",
					 (sizeof ":max:") - 1)) {

				str += (sizeof ":max:") - 1;

				str = parse_units(str, &size);
			}

			if (*str != '\0') {
				ut_free(new_str);
				ib::error()
					<< "syntax error in file path or"
					<< " size specified is less than"
					<< " 1 megabyte";
				return(false);
			}
		}

		if (::strlen(str) >= 6
		    && *str == 'n'
		    && *(str + 1) == 'e'
		    && *(str + 2) == 'w') {

			if (!supports_raw) {
				ib::error()
					<< "Tablespace doesn't support raw"
					" devices";
				ut_free(new_str);
				return(false);
			}

			str += 3;
		}

		if (*str == 'r' && *(str + 1) == 'a' && *(str + 2) == 'w') {
			str += 3;

			if (!supports_raw) {
				ib::error()
					<< "Tablespace doesn't support raw"
					" devices";
				ut_free(new_str);
				return(false);
			}
		}

		if (size == 0) {

			ut_free(new_str);

			ib::error()
				<< "syntax error in file path or size"
				" specified is less than 1 megabyte";

			return(false);
		}

		++n_files;

		if (*str == ';') {
			str++;
		} else if (*str != '\0') {
			ut_free(new_str);

			ib::error()
				<< "syntax error in file path or size"
				" specified is less than 1 megabyte";
			return(false);
		}
	}

	if (n_files == 0) {

		/* filepath_spec must contain at least one data file
		definition */

		ut_free(new_str);

		ib::error()
			<< "syntax error in file path or size specified"
			" is less than 1 megabyte";

		return(false);
	}

	/*---------------------- PASS 2 ---------------------------*/
	/* Then store the actual values to our arrays */
	str = input_str;
	ulint order = 0;

	while (*str != '\0') {
		filepath = str;

		/* Note that we must step over the ':' in a Windows filepath;
		a Windows path normally looks like C:\ibdata\ibdata1:1G, but
		a Windows raw partition may have a specification like
		\\.\C::1Gnewraw or \\.\PHYSICALDRIVE2:1Gnewraw */

		while ((*str != ':' && *str != '\0')
		       || (*str == ':'
			   && (*(str + 1) == '\\' || *(str + 1) == '/'
			       || *(str + 1) == ':'))) {
			str++;
		}

		if (*str == ':') {
			/* Make filepath a null-terminated string */
			*str = '\0';
			str++;
		}

		str = parse_units(str, &size);

		if (0 == strncmp(str, ":autoextend",
				 (sizeof ":autoextend") - 1)) {

			m_auto_extend_last_file = true;

			str += (sizeof ":autoextend") - 1;

			if (0 == strncmp(str, ":max:",
					 (sizeof ":max:") - 1)) {

				str += (sizeof ":max:") - 1;

				str = parse_units(str, &m_last_file_size_max);
			}

			if (*str != '\0') {
				ut_free(new_str);
				ib::error() << "syntax error in file path or"
					" size specified is less than 1"
					" megabyte";
				return(false);
			}
		}

		m_files.push_back(Datafile(flags(), uint32_t(size), order));
		m_files.back().make_filepath(path(),
					     {filepath, strlen(filepath)},
					     NO_EXT);

		if (::strlen(str) >= 6
		    && *str == 'n'
		    && *(str + 1) == 'e'
		    && *(str + 2) == 'w') {

			ut_a(supports_raw);

			str += 3;

			/* Initialize new raw device only during initialize */
			/* JAN: TODO: MySQL 5.7 used opt_initialize */
			m_files.back().m_type =
			opt_bootstrap ? SRV_NEW_RAW : SRV_OLD_RAW;
		}

		if (*str == 'r' && *(str + 1) == 'a' && *(str + 2) == 'w') {

			ut_a(supports_raw);

			str += 3;

			/* Initialize new raw device only during initialize */
			if (m_files.back().m_type == SRV_NOT_RAW) {
				/* JAN: TODO: MySQL 5.7 used opt_initialize */
				m_files.back().m_type =
				opt_bootstrap ? SRV_NEW_RAW : SRV_OLD_RAW;
			}
		}

		if (*str == ';') {
			++str;
		}
		order++;
	}

	ut_ad(n_files == ulint(m_files.size()));

	ut_free(new_str);

	return(true);
}

/** Frees the memory allocated by the parse method. */
void
SysTablespace::shutdown()
{
	Tablespace::shutdown();

	m_auto_extend_last_file = 0;
	m_last_file_size_max = 0;
	m_created_new_raw = 0;
	m_is_tablespace_full = false;
	m_sanity_checks_done = false;
}

/** Verify the size of the physical file.
@param[in]	file	data file object
@return DB_SUCCESS if OK else error code. */
dberr_t
SysTablespace::check_size(
	Datafile&	file)
{
	os_offset_t	size = os_file_get_size(file.m_handle);
	ut_a(size != (os_offset_t) -1);

	/* Under some error conditions like disk full scenarios
	or file size reaching filesystem limit the data file
	could contain an incomplete extent at the end. When we
	extend a data file and if some failure happens, then
	also the data file could contain an incomplete extent.
	So we need to round the size downward to a  megabyte.*/

	const uint32_t	rounded_size_pages = static_cast<uint32_t>(
		size >> srv_page_size_shift);

	/* If last file */
	if (&file == &m_files.back() && m_auto_extend_last_file) {

		if (file.m_size > rounded_size_pages
		    || (m_last_file_size_max > 0
			&& m_last_file_size_max < rounded_size_pages)) {
			ib::error() << "The Auto-extending data file '"
				    << file.filepath()
				    << "' is of a different size "
				    << rounded_size_pages
				    << " pages than specified"
				" by innodb_data_file_path";
			return(DB_ERROR);
		}

		file.m_size = rounded_size_pages;
	}

	if (rounded_size_pages != file.m_size) {
		ib::error() << "The data file '"
			<< file.filepath() << "' is of a different size "
			<< rounded_size_pages << " pages"
			" than the " << file.m_size << " pages specified by"
			" innodb_data_file_path";
		return(DB_ERROR);
	}

	return(DB_SUCCESS);
}

/** Set the size of the file.
@param[in]	file	data file object
@return DB_SUCCESS or error code */
dberr_t
SysTablespace::set_size(
	Datafile&	file)
{
	ut_ad(!srv_read_only_mode || m_ignore_read_only);
	const ib::bytes_iec b{uint64_t{file.m_size} << srv_page_size_shift};

	/* We created the data file and now write it full of zeros */
	ib::info() << "Setting file '" << file.filepath() << "' size to " << b
		<< ". Physically writing the file full; Please wait ...";

	bool	success = os_file_set_size(
		file.m_filepath, file.m_handle,
		static_cast<os_offset_t>(file.m_size) << srv_page_size_shift);

	if (success) {
		ib::info() << "File '" << file.filepath() << "' size is now "
			<< b
			<< ".";
	} else {
		ib::error() << "Could not set the file size of '"
			<< file.filepath() << "'. Probably out of disk space";

		return(DB_ERROR);
	}

	return(DB_SUCCESS);
}

/** Create a data file.
@param[in]	file	data file object
@return DB_SUCCESS or error code */
dberr_t
SysTablespace::create_file(
	Datafile&	file)
{
	dberr_t	err = DB_SUCCESS;

	ut_a(!file.m_exists);
	ut_ad(!srv_read_only_mode || m_ignore_read_only);

	switch (file.m_type) {
	case SRV_NEW_RAW:

		/* The partition is opened, not created; then it is
		written over */
		m_created_new_raw = true;

		/* Fall through. */

	case SRV_OLD_RAW:

		srv_start_raw_disk_in_use = TRUE;

		/* Fall through. */

	case SRV_NOT_RAW:
		err = file.open_or_create(
			!m_ignore_read_only && srv_read_only_mode);
		break;
	}

	if (err != DB_SUCCESS) {
		return err;
	}

	switch (file.m_type) {
	case SRV_OLD_RAW:
		break;
	case SRV_NOT_RAW:
#ifndef _WIN32
		if (!space_id() && my_disable_locking
		    && os_file_lock(file.m_handle, file.m_filepath)) {
			err = DB_ERROR;
			break;
		}
#endif
		/* fall through */
	case SRV_NEW_RAW:
		err = set_size(file);
	}

	return(err);
}

/** Open a data file.
@param[in]	file	data file object
@return DB_SUCCESS or error code */
dberr_t
SysTablespace::open_file(
	Datafile&	file)
{
	dberr_t	err = DB_SUCCESS;

	ut_a(file.m_exists);

	switch (file.m_type) {
	case SRV_NEW_RAW:
		/* The partition is opened, not created; then it is
		written over */
		m_created_new_raw = true;

		/* Fall through */

	case SRV_OLD_RAW:
		srv_start_raw_disk_in_use = TRUE;

		if (srv_read_only_mode && !m_ignore_read_only) {
			ib::error() << "Can't open a raw device '"
				<< file.m_filepath << "' when"
				" --innodb-read-only is set";

			return(DB_ERROR);
		}

		/* Fall through */

	case SRV_NOT_RAW:
		err = file.open_or_create(
			!m_ignore_read_only && srv_read_only_mode);

		if (err != DB_SUCCESS) {
			return(err);
		}
		break;
	}

	switch (file.m_type) {
	case SRV_NEW_RAW:
		/* Set file size for new raw device. */
		err = set_size(file);
		break;

	case SRV_NOT_RAW:
#ifndef _WIN32
		if (!space_id() && (m_ignore_read_only || !srv_read_only_mode)
		    && my_disable_locking
		    && os_file_lock(file.m_handle, file.m_filepath)) {
			err = DB_ERROR;
			break;
		}
#endif
		/* Check file size for existing file. */
		err = check_size(file);
		break;

	case SRV_OLD_RAW:
		err = DB_SUCCESS;
		break;

	}

	if (err != DB_SUCCESS) {
		file.close();
	}

	return(err);
}

/** Check the tablespace header for this tablespace.
@return DB_SUCCESS or error code */
inline dberr_t SysTablespace::read_lsn_and_check_flags()
{
	dberr_t	err;

	files_t::iterator it = m_files.begin();

	ut_a(it->m_exists);

	if (it->m_handle == OS_FILE_CLOSED) {

		err = it->open_or_create(
			m_ignore_read_only ?  false : srv_read_only_mode);

		if (err != DB_SUCCESS) {
			return(err);
		}
	}

	err = it->read_first_page(
		m_ignore_read_only && srv_read_only_mode);

	if (err != DB_SUCCESS) {
		return(err);
	}

	ut_a(it->order() == 0);

	if (srv_operation  <= SRV_OPERATION_EXPORT_RESTORED) {
		buf_dblwr.init_or_load_pages(it->handle(), it->filepath());
	}

	/* Check the contents of the first page of the
	first datafile. */
	err = it->validate_first_page(it->m_first_page);
	const page_t *first_page = it->m_first_page;

	if (err != DB_SUCCESS) {
		mysql_mutex_lock(&recv_sys.mutex);
		first_page = recv_sys.dblwr.find_page(
			page_id_t(space_id(), 0), LSN_MAX);
		mysql_mutex_unlock(&recv_sys.mutex);
		if (!first_page) {
			err = DB_CORRUPTION;
		} else {
			err = it->read_first_page_flags(first_page);
			if (err == DB_SUCCESS) {
				err = it->validate_first_page(first_page);
			}
		}
	}

	/* Make sure the tablespace space ID matches the
	space ID on the first page of the first datafile. */
	if (err != DB_SUCCESS || space_id() != it->m_space_id) {
		sql_print_error("InnoDB: The data file '%s'"
				" has the wrong space ID."
				" It should be " UINT32PF ", but " UINT32PF
				" was found", it->filepath(),
				space_id(), it->m_space_id);
		it->close();
		return err;
	}

	if (srv_force_recovery != 6
	    && srv_operation == SRV_OPERATION_NORMAL
	    && !log_sys.next_checkpoint_lsn
	    && log_sys.format == log_t::FORMAT_3_23) {

		log_sys.latch.wr_lock(SRW_LOCK_CALL);
		/* Prepare for possible upgrade from 0-sized ib_logfile0. */
		log_sys.next_checkpoint_lsn = mach_read_from_8(
			first_page + 26/*FIL_PAGE_FILE_FLUSH_LSN*/);
		if (log_sys.next_checkpoint_lsn < 8204) {
			/* Before MDEV-14425, InnoDB had a minimum LSN
			of 8192+12=8204. Likewise, mariadb-backup
			--prepare would create an empty ib_logfile0
			after applying the log. We will allow an
			upgrade from such an empty log. */
			sql_print_error("InnoDB: ib_logfile0 is "
					"empty, and LSN is unknown.");
			err = DB_CORRUPTION;
		} else {
			log_sys.last_checkpoint_lsn =
				recv_sys.lsn = recv_sys.file_checkpoint =
				log_sys.next_checkpoint_lsn;
			log_sys.set_recovered_lsn(log_sys.next_checkpoint_lsn);
			log_sys.next_checkpoint_no = 0;
		}

		log_sys.latch.wr_unlock();
	}

	it->close();
	return err;
}

/** Check if a file can be opened in the correct mode.
@param[in]	file	data file object
@param[out]	reason	exact reason if file_status check failed.
@return DB_SUCCESS or error code. */
dberr_t
SysTablespace::check_file_status(
	const Datafile&		file,
	file_status_t&		reason)
{
	os_file_stat_t	stat;

	memset(&stat, 0x0, sizeof(stat));

	dberr_t	err = os_file_get_status(
		file.m_filepath, &stat, true,
		m_ignore_read_only ? false : srv_read_only_mode);

	reason = FILE_STATUS_VOID;
	/* File exists but we can't read the rw-permission settings. */
	switch (err) {
	case DB_FAIL:
		ib::error() << "os_file_get_status() failed on '"
			<< file.filepath()
			<< "'. Can't determine file permissions";
		err = DB_ERROR;
		reason = FILE_STATUS_RW_PERMISSION_ERROR;
		break;

	case DB_SUCCESS:
		/* Note: stat.rw_perm is only valid for "regular" files */

		if (stat.type == OS_FILE_TYPE_FILE) {
			if (!stat.rw_perm) {
				ib::error() << "The data file"
					    << " '" << file.filepath()
					    << ((!srv_read_only_mode
						 || m_ignore_read_only)
						? "' must be writable"
						: "' must be readable");

				err = DB_ERROR;
				reason = FILE_STATUS_READ_WRITE_ERROR;
			}

		} else {
			/* Not a regular file, bail out. */
			ib::error() << "The data file '" << file.filepath()
				    << "' is not a regular file.";

			err = DB_ERROR;
			reason = FILE_STATUS_NOT_REGULAR_FILE_ERROR;
		}
		break;

	case DB_NOT_FOUND:
		break;

	default:
		ut_ad(0);
	}

	return(err);
}

/** Note that the data file was not found.
@param[in]	file		data file object
@param[out]	create_new_db	true if a new instance to be created
@return DB_SUCESS or error code */
dberr_t
SysTablespace::file_not_found(
	Datafile&	file,
	bool*	create_new_db)
{
	file.m_exists = false;

	if (m_ignore_read_only) {
	} else if (srv_read_only_mode) {
		ib::error() << "Can't create file '" << file.filepath()
			<< "' when --innodb-read-only is set";
		return(DB_ERROR);
	} else if (srv_force_recovery && space_id() == TRX_SYS_SPACE) {
		ib::error() << "Can't create file '" << file.filepath()
			<< "' when --innodb-force-recovery is set";
		return DB_ERROR;
	}

	if (&file == &m_files.front()) {

		/* First data file. */
		ut_a(!*create_new_db);
		*create_new_db = TRUE;

		if (space_id() == TRX_SYS_SPACE) {
			ib::info() << "The first data file '"
				<< file.filepath() << "' did not exist."
				" A new tablespace will be created!";
		}

	} else {
		ib::info() << "Need to create a new data file '"
			   << file.filepath() << "'.";
	}

	/* Set the file create mode. */
	switch (file.m_type) {
	case SRV_NOT_RAW:
		file.set_open_flags(OS_FILE_CREATE);
		break;

	case SRV_NEW_RAW:
	case SRV_OLD_RAW:
		file.set_open_flags(OS_FILE_OPEN_RAW);
		break;
	}

	return(DB_SUCCESS);
}

/** Note that the data file was found.
@param[in,out]	file	data file object
@return true if a new instance to be created */
bool
SysTablespace::file_found(
	Datafile&	file)
{
	/* Note that the file exists and can be opened
	in the appropriate mode. */
	file.m_exists = true;

	/* Set the file open mode */
	switch (file.m_type) {
	case SRV_NOT_RAW:
		file.set_open_flags(
			&file == &m_files.front()
			? OS_FILE_OPEN_RETRY : OS_FILE_OPEN);
		break;

	case SRV_NEW_RAW:
	case SRV_OLD_RAW:
		file.set_open_flags(OS_FILE_OPEN_RAW);
		break;
	}

	/* Need to create the system tablespace for new raw device. */
	return(file.m_type == SRV_NEW_RAW);
}

/** Check the data file specification.
@param[out] create_new_db	true if a new database is to be created
@param[in] min_expected_size	Minimum expected tablespace size in bytes
@return DB_SUCCESS if all OK else error code */
dberr_t
SysTablespace::check_file_spec(
	bool*	create_new_db,
	ulint	min_expected_size)
{
	*create_new_db = FALSE;

	if (m_files.size() >= 1000) {
		ib::error() << "There must be < 1000 data files "
			" but " << m_files.size() << " have been"
			" defined.";

		return(DB_ERROR);
	}

	if (!m_auto_extend_last_file
	    && get_sum_of_sizes()
	    < (min_expected_size >> srv_page_size_shift)) {
		ib::error() << "Tablespace size must be at least "
			<< (min_expected_size >> 20) << " MB";
		return(DB_ERROR);
	}

	dberr_t	err = DB_SUCCESS;

	ut_a(!m_files.empty());

	/* If there is more than one data file and the last data file
	doesn't exist, that is OK. We allow adding of new data files. */

	files_t::iterator	begin = m_files.begin();
	files_t::iterator	end = m_files.end();

	for (files_t::iterator it = begin; it != end; ++it) {

		file_status_t reason_if_failed;
		err = check_file_status(*it, reason_if_failed);

		if (err == DB_NOT_FOUND) {

			err = file_not_found(*it, create_new_db);

			if (err != DB_SUCCESS) {
				break;
			}

		} else if (err != DB_SUCCESS) {
			if (reason_if_failed == FILE_STATUS_READ_WRITE_ERROR) {
				ib::error() << "The data file '"
					    << it->filepath()
					    << ((!srv_read_only_mode
						 || m_ignore_read_only)
						? "' must be writable"
						: "' must be readable");
			}

			ut_a(err != DB_FAIL);
			break;

		} else if (*create_new_db) {
			ib::error() << "The data file '"
				    << begin->filepath()
				    << "' was not found but"
				" one of the other data files '"
				    << it->filepath() << "' exists.";

			err = DB_ERROR;
			break;

		} else {
			*create_new_db = file_found(*it);
		}
	}

	return(err);
}

/** Open or create the data files
@param[in]  is_temp		whether this is a temporary tablespace
@param[in]  create_new_db	whether we are creating a new database
@param[out] sum_new_sizes	sum of sizes of the new files added
@return DB_SUCCESS or error code */
dberr_t
SysTablespace::open_or_create(
	bool	is_temp,
	bool	create_new_db,
	ulint*	sum_new_sizes)
{
	dberr_t		err	= DB_SUCCESS;
	fil_space_t*	space	= NULL;

	ut_ad(!m_files.empty());

	if (sum_new_sizes) {
		*sum_new_sizes = 0;
	}

	files_t::iterator	begin = m_files.begin();
	files_t::iterator	end = m_files.end();

	ut_ad(begin->order() == 0);

	for (files_t::iterator it = begin; it != end; ++it) {

		if (it->m_exists) {
			err = open_file(*it);

			/* For new raw device increment new size. */
			if (sum_new_sizes && it->m_type == SRV_NEW_RAW) {

				*sum_new_sizes += it->m_size;
			}

		} else {
			err = create_file(*it);

			if (sum_new_sizes) {
				*sum_new_sizes += it->m_size;
			}

			/* Set the correct open flags now that we have
			successfully created the file. */
			if (err == DB_SUCCESS) {
				/* We ignore new_db OUT parameter here
				as the information is known at this stage */
				file_found(*it);
			}
		}

		if (err != DB_SUCCESS) {
			return(err);
		}

	}

	if (!create_new_db && space_id() == TRX_SYS_SPACE) {
		/* Validate the header page in the first datafile. */
		err = read_lsn_and_check_flags();
		if (err != DB_SUCCESS) {
			return(err);
		}
	}

	/* Close the curent handles, add space and file info to the
	fil_system cache and the Data Dictionary, and re-open them
	in file_system cache so that they stay open until shutdown. */
	mysql_mutex_lock(&fil_system.mutex);
	ulint	node_counter = 0;
	for (files_t::iterator it = begin; it != end; ++it) {
		it->close();
		it->m_exists = true;

		if (it != begin) {
		} else if (is_temp) {
			ut_ad(space_id() == SRV_TMP_SPACE_ID);
			space = fil_space_t::create(
				SRV_TMP_SPACE_ID, flags(),
				FIL_TYPE_TEMPORARY, NULL);
			ut_ad(space == fil_system.temp_space);
			if (!space) {
				err = DB_ERROR;
				break;
			}
			ut_ad(!space->is_compressed());
			ut_ad(space->full_crc32());
		} else {
			ut_ad(space_id() == TRX_SYS_SPACE);
			space = fil_space_t::create(
				TRX_SYS_SPACE, it->flags(),
				FIL_TYPE_TABLESPACE, NULL);
			ut_ad(space == fil_system.sys_space);
			if (!space) {
				err = DB_ERROR;
				break;
			}
		}

		uint32_t max_size = (++node_counter == m_files.size()
				    ? (m_last_file_size_max == 0
				       ? UINT32_MAX
				       : uint32_t(m_last_file_size_max))
				    : it->m_size);

		space->add(it->m_filepath, OS_FILE_CLOSED, it->m_size,
			   it->m_type != SRV_NOT_RAW, true, max_size);
	}

	mysql_mutex_unlock(&fil_system.mutex);
	return(err);
}

/** Normalize the file size, convert from megabytes to number of pages. */
void
SysTablespace::normalize_size()
{
	files_t::iterator	end = m_files.end();

	for (files_t::iterator it = m_files.begin(); it != end; ++it) {

		it->m_size <<= (20U - srv_page_size_shift);
	}

	m_last_file_size_max <<= (20U - srv_page_size_shift);
}


/**
@return next increment size */
uint32_t SysTablespace::get_increment() const
{
  if (m_last_file_size_max == 0)
    return get_autoextend_increment();

  if (!is_valid_size())
  {
     ib::error() << "The last data file has a size of " << last_file_size()
                 << " but the max size allowed is "
                 << m_last_file_size_max;
  }

  return std::min(uint32_t(m_last_file_size_max) - last_file_size(),
                  get_autoextend_increment());
}


/**
@return true if configured to use raw devices */
bool
SysTablespace::has_raw_device()
{
	files_t::iterator	end = m_files.end();

	for (files_t::iterator it = m_files.begin(); it != end; ++it) {

		if (it->is_raw_device()) {
			return(true);
		}
	}

	return(false);
}
