#ifndef UTILS_H
#define UTILS_H

#include "client.h"
#include <stddef.h>

#if defined(__FreeBSD__)
#include <sys/endian.h>
#else
#include <endian.h>
#include <byteswap.h>
#endif

void cleanup_client(client_context *ctx);

void print_usage(client_context *ctx);

__attribute__((noreturn)) void fatal_error(client_context *ctx);

void get_user_input(char *dest, size_t size, const char *prompt);

__attribute__((noreturn)) void quit(client_context *ctx);

#endif /* UTILS_H */