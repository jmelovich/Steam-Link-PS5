/*
 * steamlink-ps5 - Data directory, log file and client identity.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "app_storage.hpp"
#include "app_log.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C"
{
    int sceKernelDebugOutText(int channel, const char *text);
    int ps5_get_random(void *output, size_t length); // platform/ps5/ps5_entropy.c
}

namespace slp
{
namespace
{
std::string g_data_dir;
Identity g_identity{};
int g_log_fd = -1;
std::mutex g_log_mutex;
std::string g_last_line;

bool writable_directory(const std::string &path)
{
    mkdir(path.c_str(), 0777);
    const std::string probe = path + "/.probe";
    const int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    close(fd);
    unlink(probe.c_str());
    return true;
}

void load_or_create_identity()
{
    const std::string path = g_data_dir + "/identity.bin";
    int fd = open(path.c_str(), O_RDONLY);
    if (fd >= 0)
    {
        const ssize_t got = read(fd, &g_identity, sizeof g_identity);
        close(fd);
        if (got == static_cast<ssize_t>(sizeof g_identity) && g_identity.device_id != 0)
        {
            log("identity loaded, device id %llu", static_cast<unsigned long long>(g_identity.device_id));
            return;
        }
    }
    random_bytes(&g_identity, sizeof g_identity);
    g_identity.device_id |= 1; // never zero
    fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        write(fd, &g_identity, sizeof g_identity);
        close(fd);
        log("identity created, device id %llu", static_cast<unsigned long long>(g_identity.device_id));
    }
    else
    {
        log("identity could not be saved (%s); pairing will not persist", std::strerror(errno));
    }
}
} // namespace

void storage_init()
{
    // ShadowMount+ 1.7beta4 and newer mount /data into the sandbox; /download0 is
    // always there when param.json declares downloadDataSize.
    for (const char *candidate : {"/data/steamlink-ps5", "/download0/steamlink-ps5", "/app0/steamlink-ps5"})
    {
        if (writable_directory(candidate))
        {
            g_data_dir = candidate;
            break;
        }
    }
    if (!g_data_dir.empty())
        g_log_fd = open((g_data_dir + "/log.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    log("data directory: %s", g_data_dir.empty() ? "(none writable)" : g_data_dir.c_str());
    if (!g_data_dir.empty())
        load_or_create_identity();
    else
        random_bytes(&g_identity, sizeof g_identity);
}

const char *data_dir()
{
    return g_data_dir.c_str();
}

const Identity &identity()
{
    return g_identity;
}

void random_bytes(void *destination, std::size_t length)
{
    if (ps5_get_random(destination, length) != 0)
    {
        // Last resort: never leave the buffer untouched.
        auto *bytes = static_cast<unsigned char *>(destination);
        std::uint64_t seed = static_cast<std::uint64_t>(time(nullptr)) ^ reinterpret_cast<std::uintptr_t>(destination);
        for (std::size_t i = 0; i < length; i++)
        {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            bytes[i] = static_cast<unsigned char>(seed >> 56);
        }
    }
}

void log(const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (length < 0)
        return;

    // Seconds since the title started: the console's clock is not worth a localtime_r.
    char line[1100];
    static const std::time_t started = time(nullptr);
    const long elapsed = static_cast<long>(time(nullptr) - started);
    const int line_length = std::snprintf(line, sizeof line, "[SLP5] %5lds %s\n", elapsed, text);

    std::lock_guard lock{g_log_mutex};
    sceKernelDebugOutText(0, line);
    if (g_log_fd >= 0)
        write(g_log_fd, line, static_cast<size_t>(line_length));
    g_last_line = text;
}

std::string log_last_line()
{
    std::lock_guard lock{g_log_mutex};
    return g_last_line;
}
} // namespace slp
