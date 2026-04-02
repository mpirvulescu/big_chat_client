#ifndef UTILS_H
#define UTILS_H

#include "types.h" // Provides the client_context name
#include <stddef.h>

#ifdef __FreeBSD__
#include <sys/endian.h>
#else
#include <byteswap.h>
#include <endian.h>
#endif

void cleanup_client(client_context *ctx);
void print_usage(client_context *ctx);
__attribute__((noreturn)) void fatal_error(client_context *ctx);
void get_user_input(char *dest, size_t size, const char *prompt);
__attribute__((noreturn)) void quit(client_context *ctx);

#endif
