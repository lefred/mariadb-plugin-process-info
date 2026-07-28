# AI-assisted changes

**Date:** 2026-07-28
**Model:** Claude Sonnet 5 (`claude-sonnet-5`), via Claude Code

The changes below were made by Claude Code at the user's request, following
an earlier AI code review of the plugin. All four issues raised by that
review were fixed.

## 1. `RSS` renamed to `RSS_BYTES` and converted from pages to bytes

`RSS` reported `/proc/[pid]/stat`'s resident-set size in pages while every
other size column in the table is suffixed `_BYTES` and reports bytes
(`VSIZE_BYTES`, `RSSLIM_BYTES`, ...). The raw page count is now converted to
bytes using `sysconf(_SC_PAGESIZE)` (guarded against a non-positive page
size and multiplication overflow) and the column is renamed `RSS_BYTES` for
consistency.

Files: `process_info.cc`, `process_info_parse.{h,cc}`, `README.md`,
`mysql-test/process_info/process_info.{test,result}`.

## 2. `GUEST_TIME_MS` validation and typing made unsigned

`GUEST_TIME_MS` was parsed as signed with no non-negativity check and stored
in an unsigned column, even though `proc_pid_stat(5)` documents
`guest_time` as an unsigned kernel `clock_t` counter (`%lu`). It is now
parsed as unsigned (rejecting malformed/negative input directly),
range-checked before the existing tick-to-millisecond conversion, and stored
in a `ULonglong` column.

An initial version of this fix also changed `CGUEST_TIME_MS` to unsigned,
reasoning that both fields "can never legitimately be negative." That
reasoning didn't hold up: `proc_pid_stat(5)` documents `cguest_time` as
**signed** (`%ld`), the same as `cutime`/`cstime`, unlike `guest_time`. See
item 5, which reverts `CGUEST_TIME_MS` to signed and fixes the fields that
actually had this bug.

Files: `process_info.cc`, `process_info_parse.cc`.

## 3. `THREAD_NAME` stored with a binary charset

A Linux thread's `comm` name can contain arbitrary bytes (anything but
`NUL` or `/`), which are not guaranteed to be valid in the connection/system
charset. Storing them with `system_charset_info` risked silent
charset-conversion mangling or truncation warnings for such names. The
column is now stored with `&my_charset_bin` so the raw bytes are copied
as-is.

Files: `process_info.cc`.

## 4. Parsing logic extracted and unit tested

`parse_unsigned`, `parse_signed`, `ticks_to_ms`, `pages_to_bytes`,
`split_fields`, `parse_stat`, `parse_io`, and `read_proc_file` were moved
out of `process_info.cc` into `process_info_parse.h` / `process_info_parse.cc`,
which have no MariaDB server header dependency. This makes them buildable
and testable outside a MariaDB source tree. `process_info_parse_test.cc`
adds assertion-based coverage for valid input, malformed/truncated
`/proc/[pid]/stat` and `io` content, numeric overflow, and bounded-read
behavior (verified locally with `g++ -std=c++11 -Wall -Wextra`, all checks
passing).

The test is wired into the MariaDB CMake build as an `EXCLUDE_FROM_ALL`
target so it doesn't slow down or risk breaking the main server build:

```sh
cmake --build . --target process_info_parse_test
ctest -R process_info_parse_test
```

or, standalone:

```sh
g++ -std=c++11 -Wall -Wextra -o process_info_parse_test \
    process_info_parse.cc process_info_parse_test.cc
./process_info_parse_test
```

Files: `process_info_parse.h` (new), `process_info_parse.cc` (new),
`process_info_parse_test.cc` (new), `process_info.cc`, `CMakeLists.txt`,
`README.md`.

## 5. `UTIME_MS`, `STIME_MS`, `DELAYACC_BLKIO_MS` made unsigned; `CGUEST_TIME_MS` reverted to signed

This note originally said these fields, plus `CUTIME_MS` and `CSTIME_MS`,
were left signed with no non-negativity check "matching their pre-existing
behavior" and out of scope. Checking each field's type against
`proc_pid_stat(5)` showed that was only half right:

| Field | `proc_pid_stat(5)` type | Action |
| --- | --- | --- |
| `utime` (14) | `%lu` unsigned | now parsed unsigned, stored `ULonglong` |
| `stime` (15) | `%lu` unsigned | now parsed unsigned, stored `ULonglong` |
| `cutime` (16) | `%ld` signed | unchanged (already correct) |
| `cstime` (17) | `%ld` signed | unchanged (already correct) |
| `delayacct_blkio_ticks` (42) | `%llu` unsigned | now parsed unsigned, stored `ULonglong` |
| `cguest_time` (44) | `%ld` signed | reverted to signed (see item 2) |

`UTIME_MS`, `STIME_MS`, and `DELAYACC_BLKIO_MS` were parsed with
`parse_signed` and stored in signed columns despite the kernel documenting
them as unsigned — the same class of bug fixed for `GUEST_TIME_MS` in item
2. They're now parsed as unsigned (rejecting malformed/negative input
directly), range-checked before the tick-to-millisecond conversion, and
stored in `ULonglong` columns, following the same pattern as
`GUEST_TIME_MS`. `CUTIME_MS`/`CSTIME_MS` needed no change: the kernel
documents them as signed, so leaving them as-is was correct.
`process_info_parse_test.cc` gained `test_parse_stat_signedness()`, covering
rejection of negative `utime`/`delayacct_blkio_ticks` and acceptance of a
negative `cguest_time`.

Files: `process_info.cc`, `process_info_parse.{h,cc}`,
`process_info_parse_test.cc`.
