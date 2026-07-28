/* Copyright (c) 2019,2024,2025,2026 MariaDB Corporation
   Copyright (c) 2026 lefred (Frédéric Descamps)

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#define MYSQL_SERVER 1

#include <my_global.h>
#include <mysql/plugin.h>
#include <sql_acl.h>
#include <sql_class.h>
#include <sql_i_s.h>

#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include "process_info_parse.h"

namespace {

using process_info::Process_info;

namespace Fields {

using Show::CEnd;
using Show::Column;
using Show::SLonglong;
using Show::ULonglong;
using Show::Varchar;

static ST_FIELD_INFO process_info_fields[]=
{
  Column("ID",                  ULonglong(20), NOT_NULL),
  Column("THREAD_NAME",         Varchar(120),  NOT_NULL),
  Column("STATE",               Varchar(1),    NOT_NULL),
  Column("UTIME_MS",            ULonglong(),   NOT_NULL),
  Column("STIME_MS",            ULonglong(),   NOT_NULL),
  Column("CUTIME_MS",           SLonglong(),   NOT_NULL),
  Column("CSTIME_MS",           SLonglong(),   NOT_NULL),
  Column("NUM_THREADS",         SLonglong(),   NOT_NULL),
  Column("VSIZE_BYTES",         ULonglong(),   NOT_NULL),
  Column("RSS_BYTES",           ULonglong(),   NOT_NULL),
  Column("RSSLIM_BYTES",        ULonglong(),   NOT_NULL),
  Column("PROCESSOR",           ULonglong(),   NOT_NULL),
  Column("DELAYACC_BLKIO_MS",   ULonglong(),   NOT_NULL),
  Column("GUEST_TIME_MS",       ULonglong(),   NOT_NULL),
  Column("CGUEST_TIME_MS",      SLonglong(),   NOT_NULL),
  Column("READ_BYTES",          ULonglong(),   NOT_NULL),
  Column("WRITE_BYTES",         ULonglong(),   NOT_NULL),
  CEnd()
};

} // namespace Fields

static int store_row(THD *thd, TABLE *table, const Process_info &row)
{
  restore_record(table, s->default_values);
  table->field[0]->store(row.id, true);
  /*
    Linux thread names (comm) may contain arbitrary bytes (anything but NUL
    or '/') that are not valid in the connection/system charset. Store them
    as binary so the server copies the raw bytes instead of silently
    replacing or truncating on a charset-conversion failure.
  */
  table->field[1]->store(row.thread_name.data(), row.thread_name.size(),
                         &my_charset_bin);
  table->field[2]->store(&row.state, 1, system_charset_info);
  table->field[3]->store(row.utime_ms, true);
  table->field[4]->store(row.stime_ms, true);
  table->field[5]->store(row.cutime_ms, false);
  table->field[6]->store(row.cstime_ms, false);
  table->field[7]->store(row.num_threads, false);
  table->field[8]->store(row.vsize_bytes, true);
  table->field[9]->store(row.rss_bytes, true);
  table->field[10]->store(row.rsslim_bytes, true);
  table->field[11]->store(row.processor, true);
  table->field[12]->store(row.delayacct_blkio_ms, true);
  table->field[13]->store(row.guest_time_ms, true);
  table->field[14]->store(row.cguest_time_ms, false);
  table->field[15]->store(row.read_bytes, true);
  table->field[16]->store(row.write_bytes, true);
  return schema_table_store_record(thd, table);
}

static int process_info_fill(THD *thd, TABLE_LIST *tables, COND *)
{
  /* Match MariaDB's process-inspection policy and the component's intent. */
  if (check_global_access(thd, PROCESS_ACL, false))
    return 1;

  int root_fd= open("/proc/self/task",
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (root_fd < 0)
    return 0; /* Linux procfs may be unavailable in a restricted container. */

  int scan_fd= dup(root_fd);
  DIR *directory= scan_fd < 0 ? NULL : fdopendir(scan_fd);
  if (!directory)
  {
    if (scan_fd >= 0)
      close(scan_fd);
    close(root_fd);
    return 0;
  }

  long ticks_per_second= sysconf(_SC_CLK_TCK);
  long page_size= sysconf(_SC_PAGESIZE);
  int result= 0;
  for (dirent *entry= readdir(directory); entry; entry= readdir(directory))
  {
    const char *p= entry->d_name;
    if (!*p)
      continue;
    for (; *p >= '0' && *p <= '9'; ++p)
      ;
    if (*p)
      continue;

    int task_fd= openat(root_fd, entry->d_name,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (task_fd < 0)
      continue;

    std::string stat;
    Process_info row;
    bool valid= process_info::read_proc_file(task_fd, "stat", &stat,
                                             64 * 1024) &&
                process_info::parse_stat(stat, ticks_per_second, page_size,
                                         &row);
    if (valid)
    {
      std::string io;
      if (process_info::read_proc_file(task_fd, "io", &io, 64 * 1024))
        process_info::parse_io(io, &row);
      else
        row.read_bytes= row.write_bytes= 0;
    }
    close(task_fd);

    if (valid && store_row(thd, tables->table, row))
    {
      result= 1;
      break;
    }
  }

  closedir(directory);
  close(root_fd);
  return result;
}

static int process_info_init(void *p)
{
  ST_SCHEMA_TABLE *schema= static_cast<ST_SCHEMA_TABLE *>(p);
  schema->fields_info= Fields::process_info_fields;
  schema->fill_table= process_info_fill;
  return 0;
}

static st_mysql_information_schema process_info_descriptor=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

} // namespace

maria_declare_plugin(process_info)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &process_info_descriptor,
  "PROCESS_INFO",
  "lefred",
  "Linux server thread resource usage",
  PLUGIN_LICENSE_GPL,
  process_info_init,
  NULL,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_BETA
}
maria_declare_plugin_end;
