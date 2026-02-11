#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void cleanup_client(client_context *ctx) {
  if (ctx->active_sock_fd >= 0) {
    printf("Closing connection and exiting...\n");
    close(ctx->active_sock_fd);
    ctx->active_sock_fd = -1;
  }
}

void print_usage(client_context *ctx) {
  fprintf(stderr, "Usage: %s -m <manager_server_ip> -p <manager_port> [-h]\n",
          ctx->argv[0]);
  fputs("\nOptions: \n", stderr);
  fputs("  -m <manager_ip_address> The server manager's IP address\n", stderr);
  fputs("  -p <manager_port> The server manager's port\n", stderr);
  fputs(" -h Display this help and exit\n", stderr);
}

void quit(client_context *ctx) {
  if (ctx->exit_message != NULL) {
    fputs(ctx->exit_message, stderr);
  }
  exit(ctx->exit_code);
}