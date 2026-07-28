/* Copyright (c) 2026 lefred (Frédéric Descamps)

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

/*
  Standalone unit tests for the pure-parsing helpers in process_info_parse.h.
  Deliberately has no MariaDB server header dependency so it can be built and
  run outside of a MariaDB source tree, e.g.:

    g++ -std=c++11 -Wall -Wextra -o process_info_parse_test \
        process_info_parse.cc process_info_parse_test.cc
    ./process_info_parse_test
*/

#include "process_info_parse.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

int g_failures= 0;

void check(bool ok, const char *what)
{
  if (!ok)
  {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

using process_info::parse_signed;
using process_info::parse_unsigned;
using process_info::ticks_to_ms;
using process_info::pages_to_bytes;
using process_info::split_fields;
using process_info::parse_stat;
using process_info::parse_io;
using process_info::read_proc_file;
using process_info::Process_info;

void test_parse_unsigned()
{
  ulonglong v= 0;
  check(parse_unsigned("0", &v) && v == 0, "parse_unsigned zero");
  check(parse_unsigned("12345", &v) && v == 12345, "parse_unsigned normal");
  check(parse_unsigned("18446744073709551615", &v) && v == ULLONG_MAX,
        "parse_unsigned max");
  check(!parse_unsigned("", &v), "parse_unsigned empty rejected");
  check(!parse_unsigned("-1", &v), "parse_unsigned negative rejected");
  check(!parse_unsigned("12a", &v), "parse_unsigned trailing garbage rejected");
  check(!parse_unsigned("18446744073709551616", &v),
        "parse_unsigned overflow rejected");
}

void test_parse_signed()
{
  longlong v= 0;
  check(parse_signed("0", &v) && v == 0, "parse_signed zero");
  check(parse_signed("-42", &v) && v == -42, "parse_signed negative");
  check(parse_signed("42", &v) && v == 42, "parse_signed positive");
  check(!parse_signed("", &v), "parse_signed empty rejected");
  check(!parse_signed("4x2", &v), "parse_signed trailing garbage rejected");
  check(!parse_signed("99999999999999999999999999", &v),
        "parse_signed overflow rejected");
}

void test_ticks_to_ms()
{
  longlong ms= -1;
  check(ticks_to_ms(100, 100, &ms) && ms == 1000, "ticks_to_ms normal");
  check(ticks_to_ms(0, 100, &ms) && ms == 0, "ticks_to_ms zero");
  check(!ticks_to_ms(1, 0, &ms), "ticks_to_ms rejects zero ticks_per_second");
  check(!ticks_to_ms(1, -1, &ms),
        "ticks_to_ms rejects negative ticks_per_second");
  check(!ticks_to_ms(LLONG_MAX, 100, &ms),
        "ticks_to_ms rejects overflowing positive ticks");
  check(!ticks_to_ms(LLONG_MIN, 100, &ms),
        "ticks_to_ms rejects overflowing negative ticks");
}

void test_pages_to_bytes()
{
  ulonglong bytes= 0;
  check(pages_to_bytes(4, 4096, &bytes) && bytes == 16384,
        "pages_to_bytes normal");
  check(!pages_to_bytes(1, 0, &bytes), "pages_to_bytes rejects zero page size");
  check(!pages_to_bytes(1, -1, &bytes),
        "pages_to_bytes rejects negative page size");
  check(!pages_to_bytes(ULLONG_MAX, 4096, &bytes),
        "pages_to_bytes rejects overflow");
}

void test_split_fields()
{
  std::vector<std::string> fields;
  split_fields("a  b\tc\n\nd", 0, &fields);
  check(fields.size() == 4 && fields[0] == "a" && fields[1] == "b" &&
        fields[2] == "c" && fields[3] == "d",
        "split_fields collapses mixed whitespace runs");

  fields.clear();
  split_fields("   ", 0, &fields);
  check(fields.empty(), "split_fields on all-whitespace input is empty");

  fields.clear();
  split_fields("xx a b", 2, &fields);
  check(fields.size() == 2 && fields[0] == "a" && fields[1] == "b",
        "split_fields honors begin offset");
}

std::string make_stat_line(const std::string &comm)
{
  /*
    proc(5) fields 3..44 (index 0..41 once comm is stripped): state through
    cguest_time. Chosen so utime/stime/cutime/cstime/num_threads/vsize/
    rss/rsslim/processor/delay/guest/cguest are independently verifiable.
  */
  const char *rest[]= {
    "S", "1", "1", "1", "0", "-1", "4194304",       /* state..flags     */
    "100", "0", "0", "0",                           /* minflt..cmajflt  */
    "300", "50", "10", "5",                          /* utime..cstime    */
    "20", "0", "4", "0", "12345",                    /* priority..starttime */
    "999999", "4096", "18446744073709551615",        /* vsize, rss, rsslim */
    "1", "1", "0", "0", "0", "0", "0", "0", "0",      /* startcode..sigcatch */
    "0", "0", "0", "17",                              /* wchan..exit_signal */
    "2", "20", "0",                                   /* processor, rt_priority, policy */
    "7", "6", "3"                                     /* delay, guest, cguest */
  };
  std::string line= "1234 (" + comm + ") ";
  for (size_t i= 0; i < sizeof(rest) / sizeof(rest[0]); ++i)
  {
    if (i)
      line+= ' ';
    line+= rest[i];
  }
  return line;
}

void test_parse_stat_valid()
{
  Process_info row;
  std::string stat= make_stat_line("some (thread) name");
  check(parse_stat(stat, 100, 4096, &row), "parse_stat accepts valid line");
  check(row.id == 1234, "parse_stat id");
  check(row.thread_name == "some (thread) name",
        "parse_stat preserves parens/spaces in comm");
  check(row.state == 'S', "parse_stat state");
  check(row.utime_ms == 3000, "parse_stat utime_ms");
  check(row.stime_ms == 500, "parse_stat stime_ms");
  check(row.cutime_ms == 100, "parse_stat cutime_ms");
  check(row.cstime_ms == 50, "parse_stat cstime_ms");
  check(row.num_threads == 4, "parse_stat num_threads");
  check(row.vsize_bytes == 999999, "parse_stat vsize_bytes");
  check(row.rss_bytes == 4096ULL * 4096ULL,
        "parse_stat converts rss pages to bytes using page size");
  check(row.rsslim_bytes == ULLONG_MAX, "parse_stat rsslim_bytes");
  check(row.processor == 2, "parse_stat processor");
  check(row.delayacct_blkio_ms == 70, "parse_stat delayacct_blkio_ms");
  check(row.guest_time_ms == 60, "parse_stat guest_time_ms");
  check(row.cguest_time_ms == 30, "parse_stat cguest_time_ms");
}

void test_parse_stat_invalid()
{
  Process_info row;

  check(!parse_stat("not a stat line", 100, 4096, &row),
        "parse_stat rejects line without parens");

  check(!parse_stat("1234 (ok S 1 1", 100, 4096, &row),
        "parse_stat rejects unterminated comm");

  std::string truncated= "1234 (ok) S 1 1 1\n";
  check(!parse_stat(truncated, 100, 4096, &row),
        "parse_stat rejects too few fields");

  std::string bad_state= make_stat_line("ok");
  bad_state.replace(bad_state.find("(ok) ") + 5, 1, "SS");
  check(!parse_stat(bad_state, 100, 4096, &row),
        "parse_stat rejects multi-character state");

  check(!parse_stat(make_stat_line("ok"), 0, 4096, &row),
        "parse_stat rejects zero ticks_per_second");

  check(!parse_stat(make_stat_line("ok"), 100, 0, &row),
        "parse_stat rejects zero page size");
}

void test_parse_stat_signedness()
{
  Process_info row;

  /*
    utime/stime/delayacct_blkio_ticks/guest_time are documented as unsigned
    (proc_pid_stat(5): %lu/%llu), so a negative value must be rejected
    outright rather than silently accepted.
  */
  std::string negative_utime= make_stat_line("ok");
  negative_utime.replace(negative_utime.find("300"), 1, "-3");
  check(!parse_stat(negative_utime, 100, 4096, &row),
        "parse_stat rejects negative utime");

  std::string negative_delay= make_stat_line("ok");
  negative_delay.replace(negative_delay.find("7 6 3"), 1, "-7");
  check(!parse_stat(negative_delay, 100, 4096, &row),
        "parse_stat rejects negative delayacct_blkio_ticks");

  /*
    cutime/cstime/cguest_time are documented as signed (%ld); a negative
    cguest_time must still be accepted and stored as-is.
  */
  std::string negative_cguest= make_stat_line("ok");
  size_t last_field= negative_cguest.rfind(" 3");
  negative_cguest.replace(last_field, 2, " -3");
  check(parse_stat(negative_cguest, 100, 4096, &row) &&
        row.cguest_time_ms == -30,
        "parse_stat accepts negative cguest_time");
}

void test_parse_io()
{
  Process_info row;
  parse_io("rchar: 111\nwchar: 222\nread_bytes: 4096\nwrite_bytes: 8192\n",
           &row);
  check(row.read_bytes == 4096, "parse_io read_bytes");
  check(row.write_bytes == 8192, "parse_io write_bytes");

  Process_info missing;
  parse_io("rchar: 111\n", &missing);
  check(missing.read_bytes == 0 && missing.write_bytes == 0,
        "parse_io defaults to zero when counters absent");
}

void test_read_proc_file()
{
  char dir_template[]= "/tmp/process_info_parse_test.XXXXXX";
  char *dir_path= mkdtemp(dir_template);
  check(dir_path != nullptr, "read_proc_file test directory created");
  if (!dir_path)
    return;

  int dir_fd= open(dir_path, O_RDONLY | O_DIRECTORY);
  check(dir_fd >= 0, "read_proc_file opened test directory for openat");

  const char *name= "content";
  const char *content= "hello procfs";
  std::string file_path= std::string(dir_path) + "/" + name;
  int file_fd= open(file_path.c_str(), O_CREAT | O_WRONLY, 0600);
  check(file_fd >= 0, "read_proc_file test file created");
  check(write(file_fd, content, std::strlen(content)) ==
        static_cast<ssize_t>(std::strlen(content)),
        "read_proc_file test file written");
  close(file_fd);

  std::string result;
  check(read_proc_file(dir_fd, name, &result, 4096) && result == content,
        "read_proc_file reads full content under a generous limit");
  check(!read_proc_file(dir_fd, name, &result, 4),
        "read_proc_file rejects content larger than the limit");
  check(!read_proc_file(dir_fd, "does-not-exist", &result, 4096),
        "read_proc_file rejects a missing file");

  close(dir_fd);
  unlink(file_path.c_str());
  rmdir(dir_path);
}

} // namespace

int main()
{
  test_parse_unsigned();
  test_parse_signed();
  test_ticks_to_ms();
  test_pages_to_bytes();
  test_split_fields();
  test_parse_stat_valid();
  test_parse_stat_invalid();
  test_parse_stat_signedness();
  test_parse_io();
  test_read_proc_file();

  if (g_failures)
  {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("all checks passed\n");
  return EXIT_SUCCESS;
}
