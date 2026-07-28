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

#ifndef PROCESS_INFO_PARSE_H
#define PROCESS_INFO_PARSE_H

/*
  This header has no MariaDB server header dependency on purpose: it is
  reused by a standalone unit test binary (process_info_parse_test.cc) that
  is built and run without a MariaDB source tree. When compiled as part of
  the plugin, MYSQL_SERVER is already defined and my_global.h has already
  supplied ulonglong/longlong, so the fallback typedefs below are skipped.
*/
#ifndef MYSQL_SERVER
#include <cstdint>
using ulonglong= uint64_t;
using longlong= int64_t;
#endif

#include <string>
#include <vector>

namespace process_info {

struct Process_info
{
  ulonglong id= 0;
  std::string thread_name;
  char state= 0;
  ulonglong utime_ms= 0;
  ulonglong stime_ms= 0;
  longlong cutime_ms= 0;
  longlong cstime_ms= 0;
  longlong num_threads= 0;
  ulonglong vsize_bytes= 0;
  ulonglong rss_bytes= 0;
  ulonglong rsslim_bytes= 0;
  ulonglong processor= 0;
  ulonglong delayacct_blkio_ms= 0;
  ulonglong guest_time_ms= 0;
  longlong cguest_time_ms= 0;
  ulonglong read_bytes= 0;
  ulonglong write_bytes= 0;
};

/*
  Reads at most `limit` bytes from `name`, opened relative to `task_fd` with
  O_NOFOLLOW so the caller can safely pass readdir() results. Returns false
  on read error or if the file holds more than `limit` bytes.
*/
bool read_proc_file(int task_fd, const char *name, std::string *result,
                    size_t limit);

bool parse_unsigned(const std::string &value, ulonglong *result);
bool parse_signed(const std::string &value, longlong *result);

bool ticks_to_ms(longlong ticks, long ticks_per_second,
                 longlong *milliseconds);
bool pages_to_bytes(ulonglong pages, long page_size, ulonglong *bytes);

void split_fields(const std::string &text, size_t begin,
                  std::vector<std::string> *fields);

/*
  Parses a /proc/[pid]/stat (or /proc/[pid]/task/[tid]/stat) line. `row` is
  populated with everything except read_bytes/write_bytes on success.
*/
bool parse_stat(const std::string &stat, long ticks_per_second,
                long page_size, Process_info *row);

/* Parses a /proc/[pid]/task/[tid]/io file, defaulting counters to 0. */
void parse_io(const std::string &io, Process_info *row);

} // namespace process_info

#endif // PROCESS_INFO_PARSE_H
