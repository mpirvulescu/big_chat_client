#include "network_funcs.h"
#include "protocol.h"
#include "utils.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// helper for fatal errors
static void fatal_error(client_context *ctx, char *msg);

// these two for network_execute_discovery
static void send_discovery_request(client_context *ctx);
static void recv_discovery_response(client_context *ctx,
                                    big_discovery_res_t *dest);

// helpers for account creation
static void send_account_creation_request(client_context *ctx);
static void recv_account_creation_response(client_context *ctx);

int convert_address(client_context *ctx) {
  memset(&ctx->addr, 0, sizeof(ctx->addr));

  if (inet_pton(AF_INET, ctx->manager_ip,
                &(((struct sockaddr_in *)&ctx->addr)->sin_addr)) == 1) {
    ctx->addr.ss_family = AF_INET;
    return 0;
  }

  return -1;
}

void socket_create(client_context *ctx) {
  ctx->active_sock_fd = socket(ctx->addr.ss_family, SOCK_STREAM, 0);

  if (ctx->active_sock_fd == -1) {
    perror("socket");
    fatal_error(ctx, "Fatal: Could not create socket.\n");
  }
}

void socket_connect(client_context *ctx, uint16_t port) {
  char addr_str[INET_ADDRSTRLEN];
  in_port_t net_port;
  socklen_t addr_len;

  // get the pointer to the IPv4 address
  struct sockaddr_in *ipv4_ptr = (struct sockaddr_in *)&ctx->addr;

  // convert IP to string for logging
  if (inet_ntop(AF_INET, &(ipv4_ptr->sin_addr), addr_str, sizeof(addr_str)) ==
      NULL) {
    perror("inet_ntop");
    fatal_error(ctx, "Fatal: internal address error.\n");
  }

  printf("Connecting to: %s:%u\n", addr_str, port);

  // convert port to network byte order
  net_port = htons(port);
  ipv4_ptr->sin_port = net_port;
  addr_len = sizeof(struct sockaddr_in);

  // connection call
  if (connect(ctx->active_sock_fd, (struct sockaddr *)ipv4_ptr, addr_len) ==
      -1) {
    fprintf(stderr, "Error: connect (%d): %s\n", errno, strerror(errno));
    fatal_error(ctx, "Fatal: Could not connect to server.\n");
  }

  printf("Successfully connected to: %s:%u\n", addr_str, port);
}

void network_execute_discovery(client_context *ctx) {
  printf("\n--- Phase 1: Server Discovery ---\n");

  // setup connection to manager
  if (convert_address(ctx) != 0) {
    fatal_error(ctx, "Invalid Manager IP format.\n");
  }

  socket_create(ctx);

  // connect using manager port
  socket_connect(ctx, ctx->manager_port);

  // protocol exchange
  send_discovery_request(ctx);

  // ai helped with this
  big_discovery_res_t response;
  recv_discovery_response(ctx, &response);

  // handle the jump to server
  struct in_addr node_ip_addr;
  node_ip_addr.s_addr = response.ip_address;

  char node_ip_str[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &node_ip_addr, node_ip_str, sizeof(node_ip_str)) ==
      NULL) {
    fatal_error(ctx, "Discovery Error: Invalid IP received from Manager.\n");
  }

  printf("Redirecting to Chat Node %d at %s\n", response.server_id,
         node_ip_str);

  // overwrite manager IP with new chat node IP
  snprintf(ctx->manager_ip, sizeof(ctx->manager_ip), "%s", node_ip_str);

  // clean up manager socket
  close(ctx->active_sock_fd);
  ctx->active_sock_fd = -1;

  // update the state
  ctx->state = STATE_AWAITING_USER_INFO;
}

void network_execute_account_creation(client_context *ctx) {
  printf("\n--- Phase 2: Account Registration ---\n");

  // re-establish connection (discovery closed it, IP is updated)
  if (convert_address(ctx) != 0) {
    fatal_error(ctx, "Invalid Server IP format.\n");
  }

  socket_create(ctx);

  // assume chat listens on the same port as manager for now
  socket_connect(ctx, ctx->manager_port);

  // send the payload
  send_account_creation_request(ctx);

  // wait for ACK/response
  recv_account_creation_response(ctx);

  // cleanup (We close after transaction, or keep open?)
  //  close it to for now
  close(ctx->active_sock_fd);
  ctx->active_sock_fd = -1;

  printf("Registration Successful. Account created.\n");
}

// void network_execute_login(client_context *ctx) {}

static void send_discovery_request(client_context *ctx) {
  big_header_t req = {.version = BIG_CHAT_VERSION,
                      .type = TYPE_DISCOVERY_REQUEST,
                      .status = 0,
                      .padding = 0,
                      .body_size = 0};

  if (send(ctx->active_sock_fd, &req, sizeof(req), 0) != sizeof(req)) {
    perror("send");
    fatal_error(ctx, "Network Error: Failed to send discovery request.\n");
  }
}

static void recv_discovery_response(client_context *ctx,
                                    big_discovery_res_t *dest) {
  big_header_t hdr;

  // read header
  ssize_t recvd = recv(ctx->active_sock_fd, &hdr, sizeof(hdr), MSG_WAITALL);

  if (recvd == 0) {
    fatal_error(ctx, "Server closed connection unexpectedly.\n");
  }

  if (recvd != sizeof(hdr)) {
    fatal_error(ctx, "Failed to receive protocol header.\n");
  }

  // validate packet
  if (hdr.type != TYPE_DISCOVERY_RESPONSE) {
    fatal_error(ctx, "Protocol Error: Invalid response type from Manager.\n");
  }

  // convert from network order to host order
  uint32_t body_len = ntohl(hdr.body_size);
  if (body_len != sizeof(big_discovery_res_t)) {
    fprintf(stderr, "Protocol Error: Expected body size %zu, got %u\n",
            sizeof(big_discovery_res_t), body_len);
    fatal_error(ctx, "Invalid discovery response size.\n");
  }

  // read body into dest
  recvd =
      recv(ctx->active_sock_fd, dest, sizeof(big_discovery_res_t), MSG_WAITALL);

  if (recvd != sizeof(big_discovery_res_t)) {
    fatal_error(ctx, "Failed to receive discovery body.\n");
  }
}

static void send_account_creation_request(client_context *ctx) {
  big_create_account_req_t body = {0};

  strncpy(body.username, ctx->username, sizeof(body.username));
  strncpy(body.password, ctx->password, sizeof(body.password));

  body.client_id = 0; // 0 for new account?
  body.status = 0x01; // as per protocol

  big_header_t req = {
      .version = BIG_CHAT_VERSION,
      .type = TYPE_ACCOUNT_CREATE_REQUEST,
      .status = 0,
      .padding = 0,
      .body_size = htonl(sizeof(body)) // worry about endianness i think
  };

  // send header
  if (send(ctx->active_sock_fd, &req, sizeof(req), 0) != sizeof(req)) {
    fatal_error(ctx, "Network Error: Failed to send register header.\n");
  }

  // send body
  if (send(ctx->active_sock_fd, &body, sizeof(body), 0) != sizeof(body)) {
    fatal_error(ctx, "Network Error: Failed to send register body.\n");
  }
}

static void recv_account_creation_response(client_context *ctx) {
  big_header_t hdr;

  ssize_t recvd = recv(ctx->active_sock_fd, &hdr, sizeof(hdr), MSG_WAITALL);

  if (recvd <= 0) {
    fatal_error(ctx, "Server disconnected during registration.\n");
  }

  if (recvd != sizeof(hdr)) {
    fatal_error(ctx, "Incomplete register response.\n");
  }

  if (hdr.type != TYPE_ACCOUNT_CREATE_RESPONSE) {
    fatal_error(ctx, "Protocol Error: Unexpected response type.\n");
  }

  // check status byte (0x00 is ok, anything else is error)
  if (hdr.status != 0x00) {
    fprintf(stderr, "Server Error Code: 0x%02X\n", hdr.status);
    // REMEMBER TO CLEAN UP THE MAGIC HEX NUMS
    if (hdr.status == 0x20) { // NOLINT
      fatal_error(ctx, "Registration Failed: Server Receiver Error.\n");
    }
  }

  // consume any body if the server sent one (protocol says response might have
  // body)
  uint32_t bsize = ntohl(hdr.body_size);
  if (bsize > 0) {
    char *junk = malloc(bsize);

    if (junk == NULL) {
      fatal_error(ctx, "Fatal: Out of memory.\n");
      return; // darcys build needs this for some reason
    }

    // Cast bsize to size_t to match the function signature exactly
    ssize_t recvd_body =
        recv(ctx->active_sock_fd, junk, (size_t)bsize, MSG_WAITALL);

    if (recvd_body != (ssize_t)bsize) {
      free(junk);
      fatal_error(ctx, "Failed to read response body.\n");
      return;
    }
    free(junk);
  }
}

static void fatal_error(client_context *ctx, char *msg) {
  ctx->exit_code = EXIT_FAILURE;
  ctx->exit_message = msg;
  quit(ctx);
}