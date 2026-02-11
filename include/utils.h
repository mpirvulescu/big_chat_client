#ifndef UTILS_H
#define UTILS_H

#include "client.h"

void cleanup_client(client_context *ctx);

void print_usage(client_context *ctx);

void get_user_input(char *dest, size_t size, const char *prompt);

void quit(client_context *ctx);

#endif /* UTILS_H */