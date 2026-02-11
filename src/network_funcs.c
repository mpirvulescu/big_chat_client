#include "network_funcs.h"
#include "protocol.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// these two for network_execute_discovery
static int send_discovery_request(int fd);
static int recv_discovery_response(int fd, big_discovery_res_t *dest);

int convert_address(client_context *ctx) {
  memset(&ctx->addr, 0, sizeof(ctx->addr));

  if (inet_pton(AF_INET, ctx->manager_ip,
                &(((struct sockaddr_in *)&ctx->addr)->sin_addr)) == 1) {
    ctx->addr.ss_family = AF_INET;
    return 0;
  }

  return -1;
}

// int socket_create(client_context *ctx) { ctx->active_sock_fd = socket() }

// add the socket shit from networkfuncs.h
int network_execute_discovery(client_context *ctx) {}
int network_execute_login(client_context *ctx) {}

static int send_discovery_request(int fd) {
  big_header_t request = {.version = BIG_CHAT_VERSION,
                          .type = TYPE_DISCOVERY_REQUEST,
                          .status = 0,
                          .padding = 0,
                          .body_size = 0};

  // if ()
}
static int recv_discovery_response(int fd, big_discovery_res_t *dest) {}
