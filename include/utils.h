#ifndef UTILS_H
#define UTILS_H

#include "types.h" // Provides the client_context name
#include <stddef.h>

/* --- Portable Endianness & Byte Swapping --- */
#if defined(__linux__) || defined(__linux)
    #include <endian.h>
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    #include <sys/endian.h>
#elif defined(__APPLE__) || defined(__MACH__)
    #include <libkern/OSByteOrder.h>
    #define htobe16(x) OSSwapHostToBigInt16(x)
    #define be16toh(x) OSSwapBigToHostInt16(x)
    #define htobe32(x) OSSwapHostToBigInt32(x)
    #define be32toh(x) OSSwapBigToHostInt32(x)
    #define htobe64(x) OSSwapHostToBigInt64(x)
    #define be64toh(x) OSSwapBigToHostInt64(x)
#endif

void cleanup_client(client_context *ctx);
void print_usage(client_context *ctx);
__attribute__((noreturn)) void fatal_error(client_context *ctx);
void get_user_input(char *dest, size_t size, const char *prompt);
__attribute__((noreturn)) void quit(client_context *ctx);

#endif