/*
 * Steam-Link-PS5 - libc entry points the title's runtime does not provide.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Third-party C code (ihslib, mbedtls) reaches these through libc macros; the
 * clean-room runtime shim resolves them to nothing, so they are defined here.
 */

#include <stdio.h>
#include <stdlib.h>

int sceKernelDebugOutText(int channel, const char *text);

/* assert(): report to the kernel log, then stop. */
__attribute__((noreturn)) void __assert(const char *function, const char *file, int line,
                                        const char *expression)
{
    char text[512];
    snprintf(text, sizeof text, "[SLP5] assertion failed: %s (%s:%d, %s)\n", expression, file, line,
             function ? function : "?");
    sceKernelDebugOutText(0, text);
    abort();
}
