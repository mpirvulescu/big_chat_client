#include "channels.h"
#include "client.h"
#include "protocol.h"
#include "utils.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// Cloned from network_funcs.c:366 — same fatal_error pattern
static void fatal_error(client_context *ctx, char *msg);

// RFC 6.6.1: Channels Read — send/recv helpers (static, same visibility as
// network_funcs.c:16-17)
static void send_channel_list_request(client_context *ctx);
static void recv_channel_list_response(client_context *ctx);

// --- fatal_error (cloned from network_funcs.c:366-370) ---
static void fatal_error(client_context *ctx, char *msg) {
  ctx->exit_code = EXIT_FAILURE;
  ctx->exit_message = msg;
  quit(ctx);
}

// --- send_channel_list_request ---
// RFC 6.6.1: Channels Read Request (type byte 0x2A, RFC Appendix A)
// ASN.1: ListAllChannels ::= SEQUENCE { auth Auth, channelIdLen UInt8,
// channelIds SEQUENCE OF ChannelId } Hexpat: struct List_All_Channels { Auth
// auth; be u8 channelIdLen; ChannelId channelIdArr[channelIdLen]; } Body: Auth
// (32 bytes) + channelIdLen (1 byte) = 33 bytes min (RFC Appendix C) Pattern:
// send_discovery_request() (network_funcs.c:198-217)
static void send_channel_list_request(client_context *ctx) {
  // Fixed part of body — flexible array member has zero length for request
  big_channel_list_t body;
  memset(&body, 0, sizeof(body));

  // Fill Auth credentials — same pattern as send_login_logout_request()
  // (network_funcs.c:377-378)
  strncpy(body.authentication.username, ctx->username, USERNAME_LENGTH);
  strncpy(body.authentication.password, ctx->password, PASSWORD_LENGTH);

  body.channel_id_length = 0; // request: no channel IDs needed

  // Build header — same pattern as send_discovery_request()
  // (network_funcs.c:201-205)
  big_header_t req = {
      .version = BIG_CHAT_VERSION,
      .type = TYPE_LIST_ALL_CHANNELS_REQUEST, // 0x2A (RFC 6.6.1)
      .status = 0,
      .reserved = 0,
      .body = htonl(sizeof(body)) // 33 bytes, big-endian (RFC 4.4)
  };

  // send header — same pattern as network_funcs.c:208-211
  if (send(ctx->active_sock_fd, &req, sizeof(req), 0) != sizeof(req)) {
    perror("send");
    fatal_error(ctx,
                "Network Error: Failed to send channel list request header.\n");
  }

  // send body (fixed part only) — same pattern as network_funcs.c:213-216
  if (send(ctx->active_sock_fd, &body, sizeof(body), 0) != sizeof(body)) {
    perror("send");
    fatal_error(ctx,
                "Network Error: Failed to send channel list request body.\n");
  }
}

// --- recv_channel_list_response ---
// RFC 6.6.1: Channels Read Response (type byte 0x2B, RFC Appendix A)
// Response body: Auth (32, zeroed per RFC 5.3) + channelIdLen (1) + channelIds
// (channelIdLen bytes) Pattern: recv_discovery_response()
// (network_funcs.c:219-261)
static void recv_channel_list_response(client_context *ctx) {
  big_header_t hdr;

  // recv header — same pattern as network_funcs.c:224
  ssize_t recvd = recv(ctx->active_sock_fd, &hdr, sizeof(hdr), MSG_WAITALL);

  // check for disconnect — same pattern as network_funcs.c:226-228
  if (recvd == 0) {
    fatal_error(ctx, "Server closed connection unexpectedly.\n");
  }

  if (recvd != sizeof(hdr)) {
    fatal_error(ctx, "Failed to receive protocol header.\n");
  }

  // validate type — same pattern as network_funcs.c:235-237
  if (hdr.type != TYPE_LIST_ALL_CHANNELS_RESPONSE) {
    fatal_error(ctx,
                "Protocol Error: Expected Channels Read Response (0x2B).\n");
  }

  // check status — same pattern as network_funcs.c:241-244
  if (hdr.status != STATUS_OK) {
    fprintf(stderr, "Server Error Code: 0x%02X\n", hdr.status);
    fatal_error(ctx, "Channel List Failed: Server returned error.\n");
  }

  // validate body size — same pattern as network_funcs.c:247-252
  // RFC 6.6.1: minimum body = 33 bytes (Auth 32 + channelIdLen 1)
  uint32_t body_len = ntohl(hdr.body);
  if (body_len < sizeof(big_channel_list_t)) {
    fprintf(stderr,
            "Protocol Error: Body too small for channel list (%u < %zu)\n",
            body_len, sizeof(big_channel_list_t));
    fatal_error(ctx, "Invalid channel list response size.\n");
  }

  // recv fixed part (33 bytes) — adapted from network_funcs.c:255-260
  big_channel_list_t fixed_body;
  recvd =
      recv(ctx->active_sock_fd, &fixed_body, sizeof(fixed_body), MSG_WAITALL);

  if (recvd != (ssize_t)sizeof(fixed_body)) {
    fatal_error(ctx, "Failed to receive channel list fixed body.\n");
  }

  // TODO(M2): Implement both steps below, then REMOVE the drain block beneath.
  // The drain reads the same variable-length bytes — keeping both will
  // double-read and desync the TCP stream.
  //
  // Step 1: recv channel ID array into ctx
  //   Verify: body_len - sizeof(big_channel_list_t) ==
  //   fixed_body.channel_id_length recv(ctx->active_sock_fd, ctx->channel_ids,
  //     fixed_body.channel_id_length, MSG_WAITALL);
  //   (check recv return value — fatal_error on short read)
  //
  // Step 2: store count
  //   ctx->channel_count = fixed_body.channel_id_length;

  // STUB: Drain remaining body for now — REMOVE when implementing above steps.
  // Pattern from recv_login_logout_response() (network_funcs.c:438-456)
  uint32_t remaining = body_len - (uint32_t)sizeof(big_channel_list_t);
  if (remaining > 0) {
    char *junk = malloc(remaining);

    if (junk == NULL) {
      fatal_error(ctx, "Fatal: Out of memory.\n");
      return;
    }

    ssize_t recvd_body =
        recv(ctx->active_sock_fd, junk, (size_t)remaining, MSG_WAITALL);

    if (recvd_body != (ssize_t)remaining) {
      free(junk);
      fatal_error(ctx, "Failed to read channel list variable body.\n");
      return;
    }
    free(junk);
  }
}

// --- network_execute_channel_list ---
// Channels Read workflow (RFC 6.6.1)
// Pattern: network_execute_discovery() (network_funcs.c:82-127)
// Difference: does NOT close socket — persistent connection (RFC 7.1, RFC 3.2
// para 6)
void network_execute_channel_list(client_context *ctx) {
  printf("\n--- Phase 4: Channel List ---\n");

  // protocol exchange — same pattern as network_funcs.c:96-100
  send_channel_list_request(ctx);
  recv_channel_list_response(ctx);

  // NOTE: do NOT close socket — persistent connection from login through logout
  // RFC 7.1: "Multiple PDUs MAY be sent consecutively on a single TCP
  // connection"

  // TODO(M2): print channel list from ctx->channel_ids[0..ctx->channel_count-1]
  printf("Channel list received.\n");
}
