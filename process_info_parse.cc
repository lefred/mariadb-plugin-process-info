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

#include "process_info_parse.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace process_info {

bool read_proc_file(int task_fd, const char *name, std::string *result,
                    size_t limit)
{
  int fd= openat(task_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return false;

  result->clear();
  char buffer[4096];
  bool read_error= false;
  while (result->size() < limit)
  {
    size_t remaining= limit - result->size();
    ssize_t count;
    do
      count= read(fd, buffer, remaining < sizeof(buffer) ? remaining
                                                         : sizeof(buffer));
    while (count < 0 && errno == EINTR);
    if (count < 0)
    {
      read_error= true;
      break;
    }
    if (count == 0)
      break;
    result->append(buffer, static_cast<size_t>(count));
  }

  char extra;
  ssize_t count;
  do
    count= read(fd, &extra, 1);
  while (count < 0 && errno == EINTR);
  int saved_errno= errno;
  close(fd);
  errno= saved_errno;
  return !read_error && count == 0;
}

bool parse_unsigned(const std::string &value, ulonglong *result)
{
  if (value.empty() || value[0] == '-')
    return false;
  char *end;
  errno= 0;
  unsigned long long parsed= strtoull(value.c_str(), &end, 10);
  if (errno == ERANGE || end != value.c_str() + value.size())
    return false;
  *result= parsed;
  return true;
}

bool parse_signed(const std::string &value, longlong *result)
{
  if (value.empty())
    return false;
  char *end;
  errno= 0;
  long long parsed= strtoll(value.c_str(), &end, 10);
  if (errno == ERANGE || end != value.c_str() + value.size())
    return false;
  *result= parsed;
  return true;
}

bool ticks_to_ms(longlong ticks, long ticks_per_second,
                 longlong *milliseconds)
{
  if (ticks_per_second <= 0)
    return false;
  if (ticks > LLONG_MAX / 1000 || ticks < LLONG_MIN / 1000)
    return false;
  *milliseconds= ticks * 1000 / ticks_per_second;
  return true;
}

bool pages_to_bytes(ulonglong pages, long page_size, ulonglong *bytes)
{
  if (page_size <= 0)
    return false;
  if (pages > ULLONG_MAX / static_cast<ulonglong>(page_size))
    return false;
  *bytes= pages * static_cast<ulonglong>(page_size);
  return true;
}

void split_fields(const std::string &text, size_t begin,
                  std::vector<std::string> *fields)
{
  fields->clear();
  while (begin < text.size())
  {
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\n' || text[begin] == '\t'))
      ++begin;
    if (begin == text.size())
      break;
    size_t end= text.find_first_of(" \n\t", begin);
    if (end == std::string::npos)
      end= text.size();
    fields->push_back(text.substr(begin, end - begin));
    begin= end;
  }
}

bool parse_stat(const std::string &stat, long ticks_per_second,
                long page_size, Process_info *row)
{
  size_t first_space= stat.find(' ');
  size_t open_paren= stat.find('(', first_space);
  size_t close_paren= stat.rfind(')');
  if (first_space == std::string::npos || open_paren == std::string::npos ||
      close_paren == std::string::npos || close_paren <= open_paren ||
      close_paren + 2 >= stat.size() || stat[close_paren + 1] != ' ')
    return false;

  std::string pid= stat.substr(0, first_space);
  if (!parse_unsigned(pid, &row->id))
    return false;
  row->thread_name= stat.substr(open_paren + 1, close_paren - open_paren - 1);

  std::vector<std::string> fields;
  split_fields(stat, close_paren + 2, &fields);
  /*
    fields[0] is proc(5) field 3 (state). The final field used here is field
    44, so reject truncated or unexpected input before indexing.
  */
  if (fields.size() < 42 || fields[0].size() != 1)
    return false;
  row->state= fields[0][0];

  longlong cutime, cstime, cguest;
  ulonglong utime, stime, delay, rss_pages, guest;
  longlong utime_ms, stime_ms, delay_ms, guest_ms;
  if (
      /*
        utime/stime/delayacct_blkio_ticks/guest_time are documented as
        unsigned kernel clock_t counters (proc_pid_stat(5): %lu/%llu), so
        they're parsed as unsigned to reject malformed/negative input
        directly. cutime/cstime/cguest_time are documented as signed (%ld)
        and are parsed as such below, matching their pre-existing behavior.
      */
      !parse_unsigned(fields[11], &utime) ||
      !parse_unsigned(fields[12], &stime) ||
      !parse_signed(fields[13], &cutime) ||
      !parse_signed(fields[14], &cstime) ||
      !parse_signed(fields[17], &row->num_threads) ||
      !parse_unsigned(fields[20], &row->vsize_bytes) ||
      !parse_unsigned(fields[21], &rss_pages) ||
      !parse_unsigned(fields[22], &row->rsslim_bytes) ||
      !parse_unsigned(fields[36], &row->processor) ||
      !parse_unsigned(fields[39], &delay) ||
      !parse_unsigned(fields[40], &guest) ||
      !parse_signed(fields[41], &cguest) ||
      utime > static_cast<ulonglong>(LLONG_MAX) ||
      stime > static_cast<ulonglong>(LLONG_MAX) ||
      delay > static_cast<ulonglong>(LLONG_MAX) ||
      guest > static_cast<ulonglong>(LLONG_MAX) ||
      !pages_to_bytes(rss_pages, page_size, &row->rss_bytes) ||
      !ticks_to_ms(static_cast<longlong>(utime), ticks_per_second,
                   &utime_ms) ||
      !ticks_to_ms(static_cast<longlong>(stime), ticks_per_second,
                   &stime_ms) ||
      !ticks_to_ms(cutime, ticks_per_second, &row->cutime_ms) ||
      !ticks_to_ms(cstime, ticks_per_second, &row->cstime_ms) ||
      !ticks_to_ms(static_cast<longlong>(delay), ticks_per_second,
                   &delay_ms) ||
      !ticks_to_ms(static_cast<longlong>(guest), ticks_per_second,
                   &guest_ms) ||
      !ticks_to_ms(cguest, ticks_per_second, &row->cguest_time_ms))
    return false;
  row->utime_ms= static_cast<ulonglong>(utime_ms);
  row->stime_ms= static_cast<ulonglong>(stime_ms);
  row->delayacct_blkio_ms= static_cast<ulonglong>(delay_ms);
  row->guest_time_ms= static_cast<ulonglong>(guest_ms);
  return true;
}

void parse_io(const std::string &io, Process_info *row)
{
  row->read_bytes= 0;
  row->write_bytes= 0;
  std::vector<std::string> lines;
  split_fields(io, 0, &lines);
  for (size_t i= 0; i + 1 < lines.size(); i+= 2)
  {
    if (lines[i] == "read_bytes:")
      parse_unsigned(lines[i + 1], &row->read_bytes);
    else if (lines[i] == "write_bytes:")
      parse_unsigned(lines[i + 1], &row->write_bytes);
  }
}

} // namespace process_info
