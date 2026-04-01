#define _DEFAULT_SOURCE
#define _BSD_SOURCE

// System headers FIRST
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

// Project headers AFTER
#include "network_funcs.h"
#include "channels.h"
#include "protocol.h"
#include "ui.h"
#include "utils.h"
#include "client.h"

// these two for network_execute_discovery
static void send_discovery_request(client_context *ctx);
static void recv_discovery_response(client_context *ctx,
                                    big_discovery_res_t *dest);

// helpers for account creation
static void send_account_creation_request(client_context *ctx);
static void recv_account_creation_response(client_context *ctx);

// helpers for login/logout
static void send_login_logout_request(client_context *ctx, uint8_t status_flag);
static void recv_login_logout_response(client_context *ctx);

// helpers
static ssize_t recv_all(int sock, void *buffer, size_t length);
static ssize_t send_all(int sock, const void *buffer, size_t length);


// ✅ FIXED FUNCTION
int convert_address(client_context *ctx) {
  memset(&ctx->addr, 0, sizeof(ctx->addr));

  struct sockaddr_in *ipv4 = (struct sockaddr_in *)(void *)&ctx->addr;

  if (inet_pton(AF_INET, ctx->manager_ip, &ipv4->sin_addr) == 1) {
    ctx->addr.ss_family = AF_INET;
    return 0;
  }

  return -1;
}

void fill_authentication_credentials(client_context *ctx, big_auth_t *auth) {
  memset(auth, 0, sizeof(big_auth_t));
  strncpy(auth->username, ctx->username, USERNAME_LENGTH - 1);
  strncpy(auth->password, ctx->password, PASSWORD_LENGTH - 1);
}

void socket_create(client_context *ctx) {
  ctx->active_sock_fd = socket(ctx->addr.ss_family, SOCK_STREAM, 0);

  if (ctx->active_sock_fd == -1) {
    ctx->error_message = "Fatal: Could not create socket.\n";
    fatal_error(ctx);
  }
}

void socket_connect(client_context *ctx, uint16_t port) {
  char addr_str[INET_ADDRSTRLEN];
  in_port_t net_port;
  socklen_t addr_len;

  struct sockaddr_in *ipv4_ptr = (struct sockaddr_in *)(void *)&ctx->addr;

  if (inet_ntop(AF_INET, &(ipv4_ptr->sin_addr), addr_str, sizeof(addr_str)) ==
      NULL) {
    ctx->error_message = "Fatal: internal address error.\n";
    fatal_error(ctx);
  }

  net_port = htons(port);
  ipv4_ptr->sin_port = net_port;
  addr_len = sizeof(struct sockaddr_in);

  if (connect(ctx->active_sock_fd, (struct sockaddr *)ipv4_ptr, addr_len) ==
      -1) {
    ctx->error_message = "Fatal: Could not connect to server.\n";
    fatal_error(ctx);
  }
}