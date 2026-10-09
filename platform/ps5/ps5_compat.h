/*
 * ps5-native-app-boilerplate - ProsperoLight component, trimmed for steamlink-ps5.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Force-included (-include) when compiling third-party networking code: a PS5
 * socket is a libSceNet handle, so the descriptor calls below must go through
 * the adapter in ps5_sockets.c rather than libc.
 */

#pragma once

#include <fcntl.h>
#include <poll.h>
#include <sys/select.h>
/* The SDK's <arpa/inet.h> does not pull these in; ihslib's POSIX backends expect it to. */
#include <netinet/in.h>
#include <sys/socket.h>

#ifndef PS5_SOCKET_ADAPTER_IMPLEMENTATION
#define close ps5_socket_close
#define fcntl ps5_socket_fcntl
#define ioctl ps5_socket_ioctl
#define poll ps5_socket_poll
#define select ps5_socket_select
int ps5_socket_close(int socket);
int ps5_socket_fcntl(int socket, int command, ...);
int ps5_socket_ioctl(int socket, unsigned long request, ...);
int ps5_socket_poll(struct pollfd *descriptors, nfds_t count, int timeout_ms);
int ps5_socket_select(int descriptor_count, fd_set *read_set, fd_set *write_set,
                      fd_set *error_set, struct timeval *timeout);
#endif
