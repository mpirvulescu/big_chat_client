#define _POSIX_C_SOURCE 200809L

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

#include "network_funcs.h"
#include "channels.h"
#include "protocol.h"
#include "ui.h"
#include "utils.h"
#include "client.h"

// helpers
static ssize_t recv_all(int sock, void *buffer, size_t length);
static ssize_t send_all(int sock, const void *buffer, size_t length);

// Socket

int convert_address(client_context *ctx) {
  memset(&ctx->addr, 0, sizeof(ctx->addr));
  struct sockaddr_in *ipv4 = (struct sockaddr_in *)(void *)&ctx->addr;

  if (inet_pton(AF_INET, ctx->manager_ip, &ipv4->sin_addr) == 1) {
    ctx->addr.ss_family = AF_INET;
    return 0;
  }
  return -1;
}

void socket_create(client_context *ctx) {
  ctx->active_sock_fd = socket(ctx->addr.ss_family, SOCK_STREAM, 0);
  if (ctx->active_sock_fd == -1) fatal_error(ctx);
}

void socket_connect(client_context *ctx, uint16_t port) {
  struct sockaddr_in *ipv4 = (struct sockaddr_in *)(void *)&ctx->addr;
  ipv4->sin_port = htons(port);

  if (connect(ctx->active_sock_fd, (struct sockaddr *)ipv4,
              sizeof(struct sockaddr_in)) == -1) {
    fatal_error(ctx);
  }
}

void fill_authentication_credentials(client_context *ctx, big_auth_t *auth) {
  memset(auth, 0, sizeof(*auth));
  strncpy(auth->username, ctx->username, USERNAME_LENGTH - 1);
  strncpy(auth->password, ctx->password, PASSWORD_LENGTH - 1);
}

// Discovery

void network_execute_discovery(client_context *ctx) {
  convert_address(ctx);
  socket_create(ctx);
  socket_connect(ctx, ctx->manager_port);

  // simple dummy discovery (server should respond properly)
  big_header_t hdr = {
      .version = BIG_CHAT_VERSION,
      .type = TYPE_DISCOVERY_REQUEST,
      .status = 0,
      .reserved = 0,
      .body = htonl(0)};

  send_all(ctx->active_sock_fd, &hdr, sizeof(hdr));

  big_header_t resp;
  if (recv_all(ctx->active_sock_fd, &resp, sizeof(resp)) <= 0) {
    fatal_error(ctx);
  }
}

// account creation

void network_execute_account_creation(client_context *ctx) {
  big_create_account_req_t body;
  memset(&body, 0, sizeof(body));

  fill_authentication_credentials(ctx, &body.authentication);

  big_header_t hdr = {
      .version = BIG_CHAT_VERSION,
      .type = TYPE_ACCOUNT_CREATE_REQUEST,
      .status = 0,
      .reserved = 0,
      .body = htonl(sizeof(body))};

  send_all(ctx->active_sock_fd, &hdr, sizeof(hdr));
  send_all(ctx->active_sock_fd, &body, sizeof(body));

  big_header_t resp;
  recv_all(ctx->active_sock_fd, &resp, sizeof(resp));
}

// Login

void network_execute_login(client_context *ctx) {
  big_login_logout_req_t body;
  memset(&body, 0, sizeof(body));

  fill_authentication_credentials(ctx, &body.authentication);
  body.status = 1;

  big_header_t hdr = {
      .version = BIG_CHAT_VERSION,
      .type = TYPE_LOGIN_OR_LOGOUT_REQUEST,
      .status = 0,
      .reserved = 0,
      .body = htonl(sizeof(body))};

  send_all(ctx->active_sock_fd, &hdr, sizeof(hdr));
  send_all(ctx->active_sock_fd, &body, sizeof(body));

  big_header_t resp;
  recv_all(ctx->active_sock_fd, &resp, sizeof(resp));
}

// Channel phase

channel_list_choice_t network_execute_channel_phase(client_context *ctx) {
  network_execute_channel_list(ctx);
  return ui_screen_channel_list(ctx);
}

// Logout

void network_execute_logout(client_context *ctx) {
  big_login_logout_req_t body;
  memset(&body, 0, sizeof(body));

  fill_authentication_credentials(ctx, &body.authentication);
  body.status = 0;

  big_header_t hdr = {
      .version = BIG_CHAT_VERSION,
      .type = TYPE_LOGIN_OR_LOGOUT_REQUEST,
      .status = 0,
      .reserved = 0,
      .body = htonl(sizeof(body))};

  send_all(ctx->active_sock_fd, &hdr, sizeof(hdr));
  send_all(ctx->active_sock_fd, &body, sizeof(body));
}

// Helpers

static ssize_t recv_all(int sock, void *buffer, size_t length) {
  size_t total = 0;
  while (total < length) {
    ssize_t n = recv(sock, (char *)buffer + total, length - total, 0);
    if (n <= 0) return n;
    total += n;
  }
  return total;
}

static ssize_t send_all(int sock, const void *buffer, size_t length) {
  size_t total = 0;
  while (total < length) {
    ssize_t n = send(sock, (char *)buffer + total, length - total, 0);
    if (n <= 0) return n;
    total += n;
  }
  return total;
}