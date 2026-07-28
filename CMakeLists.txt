# Copyright (c) 2019,2024,2025,2026 MariaDB Corporation
# Copyright (c) 2026 lefred (Frédéric Descamps)
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.

IF(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  MYSQL_ADD_PLUGIN(process_info process_info.cc process_info_parse.cc
                   MODULE_ONLY RECOMPILE_FOR_EMBEDDED)

  # Standalone unit tests for the procfs parsing helpers. This target has no
  # MariaDB server header dependency, so it is excluded from the default
  # build (the full server build is expensive to trigger for a parser
  # change) and built/run on demand:
  #   cmake --build . --target process_info_parse_test
  #   ctest -R process_info_parse_test
  ADD_EXECUTABLE(process_info_parse_test EXCLUDE_FROM_ALL
                 process_info_parse.cc process_info_parse_test.cc)
  TARGET_COMPILE_FEATURES(process_info_parse_test PRIVATE cxx_std_11)
  ADD_TEST(NAME process_info_parse_test COMMAND process_info_parse_test)
ENDIF()
