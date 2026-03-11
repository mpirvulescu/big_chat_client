#include "channels.h"
#include "client.h"
#include "network_funcs.h"
#include "protocol.h"
#include "utils.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// main helper functions
static void send_channel_list_request(client_context *ctx);
static void recv_channel_list_response(client_context *ctx);

// helper functions for main helper functions
static void prepare_and_send_header_for_channel_list_req(client_context *ctx);
static void send_body_for_channel_list_req(client_context *ctx,
                                           big_channel_list_t *body);

static void receive_header_for_channel_list(client_context *ctx,
                                            big_header_t *header);
static void receive_body_for_channel_list(client_context *ctx,
                                          uint32_t total_size);

void network_execute_channel_list(client_context *ctx) {
  printf("channel list\n");
  send_channel_list_request(ctx);
  recv_channel_list_response(ctx);
}

static void send_channel_list_request(client_context *ctx) {
  big_channel_list_t body;
  memset(&body, 0, sizeof(body));

  fill_authentication_credentials(ctx, &body.authentication);

  body.channel_id_length = 0;

  prepare_and_send_header_for_channel_list_req(ctx);

  send_body_for_channel_list_req(ctx, &body);
}

static void prepare_and_send_header_for_channel_list_req(client_context *ctx) {
  big_header_t req_header = {.version = BIG_CHAT_VERSION,
                             .type = TYPE_LIST_ALL_CHANNELS_REQUEST,
                             .status = 0,
                             .reserved = 0,
                             .body = htonl(sizeof(big_auth_t) + 1)};

  if (send(ctx->active_sock_fd, &req_header, sizeof(req_header), 0) !=
      sizeof(req_header)) {
    ctx->error_message =
        "Network Error: Failed to send channel list request header.\n";
    fatal_error(ctx);
  }
}

static void send_body_for_channel_list_req(client_context *ctx,
                                           big_channel_list_t *body) {
  size_t req_body_len = sizeof(big_auth_t) + 1;

  if (send(ctx->active_sock_fd, body, req_body_len, 0) !=
      (ssize_t)req_body_len) {
    ctx->error_message =
        "Network Error: Failed to send channel list request body.\n";
    fatal_error(ctx);
  }
}

static void recv_channel_list_response(client_context *ctx) {
  big_header_t recv_header;

  // get the 8-byte header
  receive_header_for_channel_list(ctx, &recv_header);

  // get the body size and fetch the rest
  uint32_t body_len = ntohl(recv_header.body);
  receive_body_for_channel_list(ctx, body_len);
}

static void receive_header_for_channel_list(client_context *ctx,
                                            big_header_t *header) {
  ssize_t recvd =
      recv(ctx->active_sock_fd, header, sizeof(big_header_t), MSG_WAITALL);

  if (recvd <= 0) {
    ctx->error_message = "Server closed connection unexpectedly.\n";
    fatal_error(ctx);
  }

  if (recvd != sizeof(big_header_t)) {
    ctx->error_message = "Failed to receive protocol header.\n";
    fatal_error(ctx);
  }

  if (header->type != TYPE_LIST_ALL_CHANNELS_RESPONSE) {
    ctx->error_message =
        "Protocol Error: Expected Channels Read Response (0x2B).\n";
    fatal_error(ctx);
  }

  if (header->status != STATUS_OK) {
    fprintf(stderr, "Server Error Code: 0x%02X\n", header->status);
    ctx->error_message = "Channel List Failed: Server returned error.\n";
    fatal_error(ctx);
  }

  // uint32_t body_len = ntohl(header.body);
  // if(body_len < sizeof(big_channel_list_t)) {
  //     fprintf(stderr,
  //         "Protocol Error: Body too small for channel list (%u < %zu)\n",
  //         body_len, sizeof(big_channel_list_t));
  //     ctx->error_message = "Invalid channel list response size\n";
  //     fatal_error(ctx);
  // }
}

static void receive_body_for_channel_list(client_context *ctx,
                                          uint32_t total_size) {
  if (total_size < sizeof(big_auth_t) + 1) {
    ctx->error_message = "Protocol Error: Body too small for channel list.\n";
    fatal_error(ctx);
  }

  big_channel_list_t *full_list = malloc(total_size);
  if (!full_list) {
    ctx->error_message = "Fatal: Out of memory.\n";
    fatal_error(ctx);
  }

  ssize_t recvd = recv(ctx->active_sock_fd, full_list, total_size, MSG_WAITALL);
  if (recvd != (ssize_t)total_size) {
    free(full_list);
    ctx->error_message = "Failed to receive complete channel list body.\n";
    fatal_error(ctx);
  }

  free(full_list);
}
