# Third-Party Notices — AxolotlDB

AxolotlDB is released under the GNU General Public License v3.0.
Copyright 2026 Youssef Ben Youssef.

This file lists open-source components fetched via CMake FetchContent.
Each is used unmodified unless noted. All licenses are compatible with GPL v3.

---

## asio (standalone, header-only)

- **Version**: 1.28.0  
- **URL**: https://github.com/chriskohlhoff/asio  
- **License**: Boost Software License 1.0 (`BSL-1.0`)  
- **Use**: Async I/O and networking primitives (planned P9 server)

> Full text: https://www.boost.org/LICENSE_1_0.txt

---

## spdlog

- **Version**: v1.12.0  
- **URL**: https://github.com/gabime/spdlog  
- **License**: MIT  
- **Use**: Structured logging (console + rotating file sink)

> Copyright (c) 2016 Gabi Melman.  
> Full text: https://github.com/gabime/spdlog/blob/v1.x/LICENSE

---

## GoogleTest

- **Version**: v1.14.0  
- **URL**: https://github.com/google/googletest  
- **License**: BSD 3-Clause ("New" or "Revised" License)  
- **Use**: Unit and integration test framework (test-only, not in production binary)

> Copyright 2008, Google Inc. All rights reserved.  
> Full text: https://github.com/google/googletest/blob/main/LICENSE

---

## nlohmann/json

- **Version**: v3.11.3  
- **URL**: https://github.com/nlohmann/json  
- **License**: MIT  
- **Use**: JSON serialisation utilities

> Copyright (c) 2013-2022 Niels Lohmann.  
> Full text: https://github.com/nlohmann/json/blob/develop/LICENSE.MIT

---

## Eigen

- **Version**: 3.4.0  
- **URL**: https://gitlab.com/libeigen/eigen  
- **License**: Mozilla Public License 2.0 (`MPL-2.0`)  
- **Use**: Linear algebra / SIMD vector distance kernels (header-only, used unmodified)

> **Note**: Eigen is used as a header-only library and is not modified.
> MPL-2.0 is file-level copyleft; using Eigen without modifying its files
> creates no obligation to release your own code under MPL-2.0.  
> Full text: https://www.mozilla.org/en-US/MPL/2.0/

---

## CLI11

- **Version**: v2.3.2  
- **URL**: https://github.com/CLIUtils/CLI11  
- **License**: BSD 3-Clause  
- **Use**: Command-line argument parsing for `axolotldb-cli`

> Copyright (c) 2017-2024 University of Cincinnati.  
> Full text: https://github.com/CLIUtils/CLI11/blob/main/LICENSE

---

## yaml-cpp

- **Version**: 0.8.0  
- **URL**: https://github.com/jbeder/yaml-cpp  
- **License**: MIT  
- **Use**: YAML configuration file parsing (`ConfigManager`)

> Copyright (c) 2008-2015 Jesse Beder.  
> Full text: https://github.com/jbeder/yaml-cpp/blob/master/LICENSE

---

*Verify versions against the `GIT_TAG` fields in [`CMakeLists.txt`](CMakeLists.txt).*
