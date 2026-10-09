/*
 * steamlink-ps5 - Logging: a file under the data directory plus the kernel log.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <string>

namespace slp
{
// printf-style; one line per call, timestamped. Thread-safe.
void log(const char *format, ...) __attribute__((format(printf, 1, 2)));
// The most recent line, for the status screen.
std::string log_last_line();
} // namespace slp
