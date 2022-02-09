/* Copyright (c) 2011, 2021, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include "my_global.h"
#include "my_thread.h"
#include "table_users.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"
#include "pfs_account.h"
#include "pfs_user.h"
#include "pfs_visitor.h"
#include "pfs_memory.h"
#include "pfs_status.h"
#include "field.h"

THR_LOCK table_users::m_table_lock;

PFS_engine_table_share
table_users::m_share=
{
  { C_STRING_WITH_LEN("users") },
  &pfs_truncatable_acl,
  table_users::create,
  NULL, /* write_row */
  table_users::delete_all_rows,
  cursor_by_user::get_row_count,
  sizeof(PFS_simple_index), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE users("
                      "USER CHAR(" USERNAME_CHAR_LENGTH_STR ") collate utf8_bin default null comment 'The connection''s client user name for the connection, or NULL if an internal thread.',"
                      "CURRENT_CONNECTIONS bigint not null comment 'Current connections for the user.',"
                      "TOTAL_CONNECTIONS bigint not null comment 'Total connections for the user.')") },
  false  /* perpetual */
};

PFS_engine_table* table_users::create()
{
  return new table_users();
}

int
table_users::delete_all_rows(void)
{
  reset_events_waits_by_thread();
  reset_events_waits_by_account();
  reset_events_waits_by_user();
  reset_events_stages_by_thread();
  reset_events_stages_by_account();
  reset_events_stages_by_user();
  reset_events_statements_by_thread();
  reset_events_statements_by_account();
  reset_events_statements_by_user();
  reset_events_transactions_by_thread();
  reset_events_transactions_by_account();
  reset_events_transactions_by_user();
  reset_memory_by_thread();
  reset_memory_by_account();
  reset_memory_by_user();
  reset_status_by_thread();
  reset_status_by_account();
  reset_status_by_user();
  purge_all_account();
  purge_all_user();
  return 0;
}

table_users::table_users()
  : cursor_by_user(& m_share),
  m_row_exists(false)
{}

void table_users::make_row(PFS_user *pfs)
{
  pfs_optimistic_state lock;

  m_row_exists= false;
  pfs->m_lock.begin_optimistic_lock(&lock);

  if (m_row.m_user.make_row(pfs))
    return;

  PFS_connection_stat_visitor visitor;
  PFS_connection_iterator::visit_user(pfs,
                                      true,  /* accounts */
                                      true,  /* threads */
                                      false, /* THDs */
                                      & visitor);

  if (! pfs->m_lock.end_optimistic_lock(& lock))
    return;

  m_row.m_connection_stat.set(& visitor.m_stat);
  m_row_exists= true;
}

int table_users::read_row_values(TABLE *table,
                                 unsigned char *buf,
                                 Field **fields,
                                 bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  assert(table->s->null_bytes == 1);
  buf[0]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* USER */
        m_row.m_user.set_field(f);
        break;
      case 1: /* CURRENT_CONNECTIONS */
      case 2: /* TOTAL_CONNECTIONS */
        m_row.m_connection_stat.set_field(f->field_index - 1, f);
        break;
      default:
        assert(false);
      }
    }
  }
  return 0;
}

