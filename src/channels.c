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

static ssize_t recv_all(int sock, void *buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    ssize_t n = recv(sock, (char *)buf + total, len - total, 0);
    if (n <= 0) {
      return n;
    }
    total += n;
  }
  return (ssize_t)total;
}

// main helper functions
static void send_channel_list_request(client_context *ctx);
static void recv_channel_list_response(client_context *ctx);

static void send_channel_join_request(client_context *ctx, uint8_t channel_id);
static void recv_channel_join_response(client_context *ctx);

// helper functions for main helper functions
static void prepare_and_send_header_for_channel_list_req(client_context *ctx);
static void send_body_for_channel_list_req(client_context *ctx,
                                           big_channel_list_t *body);

static void receive_header_for_channel_list(client_context *ctx,
                                            big_header_t *header);
static void receive_body_for_channel_list(client_context *ctx,
                                          uint32_t total_size);

void network_execute_channel_list(client_context *ctx) {
  // printf("channel list\n");
  send_channel_list_request(ctx);
  recv_channel_list_response(ctx);
}

void network_fetch_own_account_id(client_context *ctx) {
  big_user_info_t req;
  memset(&req, 0, sizeof(req));
  fill_authentication_credentials(ctx, &req.authentication);

  /* We want to look up OUR OWN username */
  strncpy(req.target_username, ctx->username, USERNAME_LENGTH);
  req.user_id = 0; /* Let the server fill this in */

  big_header_t hdr = {.version = BIG_CHAT_VERSION,
                      .type = TYPE_GET_USER_INFO_REQUEST,
                      .status = 0,
                      .reserved = 0,
                      .body = htonl(sizeof(req))};

  if (send(ctx->active_sock_fd, &hdr, sizeof(hdr), 0) != sizeof(hdr)) {
    ctx->error_message = "Failed to request own user info.\n";
    fatal_error(ctx);
  }
  if (send(ctx->active_sock_fd, &req, sizeof(req), 0) != sizeof(req)) {
    ctx->error_message = "Failed to send user info body.\n";
    fatal_error(ctx);
  }

  /* Wait for the 0x13 Response */
  big_header_t resp_hdr;
  if (recv_all(ctx->active_sock_fd, &resp_hdr, sizeof(resp_hdr)) !=
      sizeof(resp_hdr)) {
    ctx->error_message = "Failed to receive user info response.\n";
    fatal_error(ctx);
  }

  uint32_t body_size = ntohl(resp_hdr.body);

  fprintf(stderr,
          "DEBUG: resp type=0x%02X status=0x%02X body_size=%u expected=%zu\n",
          resp_hdr.type, resp_hdr.status, body_size, sizeof(big_user_info_t));

  if (resp_hdr.type != TYPE_GET_USER_INFO_RESPONSE ||
      resp_hdr.status != STATUS_OK) {
    /* If the server doesn't support 0x12, we drain and fallback to 0 */
    char *junk = malloc(body_size);
    if (junk) {
      recv_all(ctx->active_sock_fd, junk, body_size);
      free(junk);
    }
    ctx->account_id = 0;
    return;
  }

  if (body_size == sizeof(big_user_info_t)) {
    big_user_info_t resp;
    recv_all(ctx->active_sock_fd, &resp, sizeof(resp));
    /* SUCCESS! We have rescued our Account ID */
    ctx->account_id = resp.user_id;
  } else {
    /* Handle unexpected body sizes safely */
    char *junk = malloc(body_size);
    if (junk) {
      recv_all(ctx->active_sock_fd, junk, body_size);
      free(junk);
    }
    ctx->account_id = 0;
  }
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
                             .body = htonl(sizeof(big_channel_list_t))};

  if (send(ctx->active_sock_fd, &req_header, sizeof(req_header), 0) !=
      sizeof(req_header)) {
    ctx->error_message =
        "Network Error: Failed to send channel list request header.\n";
    fatal_error(ctx);
  }
}

static void send_body_for_channel_list_req(client_context *ctx,
                                           big_channel_list_t *body) {
  size_t req_body_len = sizeof(big_channel_list_t);

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
  ssize_t recvd = recv_all(ctx->active_sock_fd, header, sizeof(big_header_t));

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
    // fprintf(stderr, "Server Error Code: 0x%02X\n", header->status);
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
  if (total_size < sizeof(big_channel_list_t)) {
    ctx->error_message = "Protocol Error: Body too small for channel list.\n";
    fatal_error(ctx);
  }

  big_channel_list_t *full_list = malloc(total_size);
  if (!full_list) {
    ctx->error_message = "Fatal: Out of memory.\n";
    fatal_error(ctx);
  }

  ssize_t recvd = recv_all(ctx->active_sock_fd, full_list, total_size);
  if (recvd != (ssize_t)total_size) {
    free(full_list);
    ctx->error_message = "Failed to receive complete channel list body.\n";
    fatal_error(ctx);
  }

  size_t count = full_list->channel_id_length;
  if (count > MAX_CHANNEL_COUNT) {
    count = MAX_CHANNEL_COUNT;
  }
  ctx->channel_count = count;
  for (size_t i = 0; i < count; i++) {
    ctx->channel_ids[i] = full_list->channel_id_array[i];
  }

  // printf("Available channels (%zu):\n", count);
  // for (size_t i = 0; i < count; i++) {
  //   printf("  [%zu] Channel ID: %hhu\n", i, ctx->channel_ids[i]);
  // }

  free(full_list);
}

void network_execute_channel_join(client_context *ctx, uint8_t channel_id) {

  if (convert_address(ctx) != 0) {
    ctx->error_message = "Invalid Server IP format.\n";
    fatal_error(ctx);
  }

  // socket_create(ctx);
  // socket_connect(ctx, ctx->manager_port);

  send_channel_join_request(ctx, channel_id);
  recv_channel_join_response(ctx);

  ctx->current_channel_id = channel_id;

  memset(ctx->username_cache, 0, sizeof(ctx->username_cache));
  memset(ctx->username_cached, 0, sizeof(ctx->username_cached));
}

static void send_channel_join_request(client_context *ctx, uint8_t channel_id) {
  size_t body_size =
      sizeof(big_channel_info_t); // flexible array member at 0 length

  big_header_t hdr = {.version = BIG_CHAT_VERSION,
                      .type = TYPE_GET_CHANNEL_INFO_REQUEST,
                      .status = 0,
                      .reserved = 0,
                      .body = htonl((uint32_t)body_size)};

  if (send(ctx->active_sock_fd, &hdr, sizeof(hdr), 0) != sizeof(hdr)) {
    ctx->error_message = "Network Error: Failed to send channel join header.\n";
    fatal_error(ctx);
  }

  big_channel_info_t *body = calloc(1, body_size);
  if (!body) {
    ctx->error_message = "Fatal: Out of memory.\n";
    fatal_error(ctx);
  }

  fill_authentication_credentials(ctx, &body->authentication);
  // channel_name left as zeroes, server identifies by channel_id
  body->channel_id = channel_id;
  body->user_id_length = 0;

  if (send(ctx->active_sock_fd, body, body_size, 0) != (ssize_t)body_size) {
    free(body);
    ctx->error_message = "Network Error: Failed to send channel join body.\n";
    fatal_error(ctx);
  }
  free(body);
}

static void recv_channel_join_response(client_context *ctx) {
  big_header_t hdr;
  ssize_t recvd = recv_all(ctx->active_sock_fd, &hdr, sizeof(hdr));

  if (recvd <= 0) {
    ctx->error_message =
        "Server closed connection unexpectedly during channel join.\n";
    fatal_error(ctx);
  }
  if (recvd != sizeof(hdr)) {
    ctx->error_message = "Failed to receive channel join response header.\n";
    fatal_error(ctx);
  }
  if (hdr.type != TYPE_GET_CHANNEL_INFO_RESPONSE) {
    ctx->error_message =
        "Protocol Error: Expected Get Channel Info Response (0x23).\n";
    fatal_error(ctx);
  }
  if (hdr.status != STATUS_OK) {
    fprintf(stderr, "Server Error Code: 0x%02X\n", hdr.status);
    ctx->error_message = "Channel Join Failed: Server returned error.\n";
    fatal_error(ctx);
  }

  // drain the response body (we have the channel_id already)
  uint32_t body_size = ntohl(hdr.body);
  if (body_size > 0) {
    char *junk = malloc(body_size);
    if (junk) {
      recv(ctx->active_sock_fd, junk, body_size, MSG_WAITALL);
      free(junk);
    }
  }
}