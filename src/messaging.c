// // #define _DEFAULT_SOURCE // NOLINT(bugprone-reserved-identifier,
// cert-dcl37-c,
// // cert-dcl51-cpp)
// #include "messaging.h"
// #include "network_funcs.h"
// #include "protocol.h"
// #include "utils.h"
// #include <arpa/inet.h>
// // #include <endian.h>
// #if defined(__FreeBSD__)
// #include <byteswap.h>
// #include <sys/endian.h>
// #else
// #include <endian.h>
// #endif
// #include <errno.h>
// #include <poll.h>
// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <sys/socket.h>
// #include <sys/time.h>
// #include <time.h>
// #include <unistd.h>

#define _DEFAULT_SOURCE // NOLINT(bugprone-reserved-identifier, cert-dcl37-c,
                        // cert-dcl51-cpp)
#include "messaging.h"
#include "network_funcs.h"
#include "protocol.h"
#include "utils.h"
#include <arpa/inet.h>
#include <errno.h>
#ifdef __FreeBSD__
#include <sys/endian.h>
#else
#include <endian.h>
#endif
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void discard_body(client_context *ctx, uint32_t body_size);

void network_execute_messaging_loop(client_context *ctx) {
  (void)ctx;
  // No-op: the ncurses UI drives the messaging loop directly via
  // network_send_message() and network_receive_pending().
}

void network_send_message(client_context *ctx, const char *text) {
  uint16_t msg_len = (uint16_t)strlen(text);
  size_t body_size = sizeof(big_send_message_t) + msg_len;
  size_t total_size = sizeof(big_header_t) + body_size;

  uint8_t *buf = calloc(1, total_size);
  if (!buf) {
    ctx->error_message = "Fatal: Out of memory preparing message.\n";
    fatal_error(ctx);
  }

  // big_header_t hdr = {.version = BIG_CHAT_VERSION,
  //                     .type = TYPE_SEND_MESSAGE_REQUEST,
  //                     .status = STATUS_OK,
  //                     .reserved = 0,
  //                     .body = htonl((uint32_t)body_size)};

  big_header_t hdr;
  memset(&hdr, 0, sizeof(hdr)); // Explicitly zero all bytes, including padding
  hdr.version = BIG_CHAT_VERSION;
  hdr.type = TYPE_SEND_MESSAGE_REQUEST;
  hdr.status = STATUS_OK;
  hdr.reserved = 0;
  hdr.body = htonl((uint32_t)body_size);

  big_send_message_t body = {0};
  fill_authentication_credentials(ctx, &body.authentication);
  body.timestamp = htobe64((uint64_t)time(NULL));
  body.message_length = htons(msg_len);
  body.channel_id = ctx->current_channel_id;

  uint8_t *p = buf;
  memcpy(p, &hdr, sizeof(hdr));
  p += sizeof(hdr);
  memcpy(p, &body, sizeof(body));
  p += sizeof(body);
  memcpy(p, text, msg_len);

  if (send(ctx->active_sock_fd, buf, total_size, 0) != (ssize_t)total_size) {
    free(buf);
    ctx->error_message = "Network Error: Failed to send chat message.\n";
    fatal_error(ctx);
  }
  free(buf);
}

int network_receive_pending(client_context *ctx,
                            void (*on_message)(const char *sender_id,
                                               const char *text,
                                               void *userdata),
                            void *userdata) {
  // Non-blocking peek — return immediately if nothing is ready
  struct pollfd pfd = {.fd = ctx->active_sock_fd, .events = POLLIN};
  int ready = poll(&pfd, 1, 0);
  if (ready <= 0) {
    return 0;
  }

  big_header_t header;
  ssize_t recvd =
      recv(ctx->active_sock_fd, &header, sizeof(header), MSG_WAITALL);
  if (recvd <= 0) {
    return -1;
  }

  uint32_t body_size = ntohl(header.body);

  if (header.type == TYPE_SEND_MESSAGE_RESPONSE) {
    discard_body(ctx, body_size);
    return 0; // ACK, nothing to show
  }

  if (header.type == TYPE_GET_MESSAGE_RESPONSE) {
    if (body_size < sizeof(big_get_message_t)) {
      discard_body(ctx, body_size);
      return -1;
    }

    big_get_message_t *msg = malloc(body_size + 1); // +1 for null terminator
    if (!msg) {
      discard_body(ctx, body_size);
      return -1;
    }

    recvd = recv(ctx->active_sock_fd, msg, body_size, MSG_WAITALL);
    if (recvd != (ssize_t)body_size) {
      free(msg);
      return -1;
    }

    if (msg->channel_id != ctx->current_channel_id) {
      free(msg);
      return 0;
    }

    // if (msg->sender_id == ctx->account_id) {
    //   free(msg);
    //   return 0;
    // }

    char sender_name[USERNAME_LENGTH + 1];
    memset(sender_name, 0, sizeof(sender_name));
    lookup_username(ctx, msg->sender_id, sender_name, sizeof(sender_name));
    if (sender_name[0] == '\0') {
      snprintf(sender_name, sizeof(sender_name), "[%u]",
               (unsigned int)msg->sender_id);
    }

    uint16_t msg_len = ntohs(msg->message_length);

    // Null-terminate safely — malloc gave us body_size+1 bytes
    char *text = (char *)msg->message;
    if (msg_len > body_size - sizeof(big_get_message_t)) {
      msg_len = (uint16_t)(body_size - sizeof(big_get_message_t));
    }
    text[msg_len] = '\0';

    if (on_message) {
      on_message(sender_name, text, userdata);
    }

    free(msg);
    return 1;
  }

  // Unknown packet type — drain and continue
  discard_body(ctx, body_size);
  fprintf(stderr, "DEBUG: unexpected type 0x%02X size %u\n", header.type,
          body_size);
  return 0;
}

static void discard_body(client_context *ctx, uint32_t body_size) {
  if (body_size == 0) {
    return;
  }
  char *junk = malloc(body_size);
  if (!junk) {
    return;
  }
  recv(ctx->active_sock_fd, junk, body_size, MSG_WAITALL);
  free(junk);
  (void)ctx;
}

// void lookup_username(client_context *ctx, uint8_t sender_id, char *out_name,
//                      size_t out_size) {
//   // REMOVE LATER. THIS IS ONLY IF SERVERS DON'T SUPPORT GETTING USERNAME
//   // (0X12/0X13) SO IT DOESNT HANG
//   // NOLINTNEXTLINE(clang-diagnostic-c23-extensions,-warnings-as-errors)
//   struct timeval tv = {
//       .tv_sec = 0,
//       .tv_usec =
//           500000}; //
//           NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers,-warnings-as-errors)
//   struct timeval zero = {.tv_sec = 0, .tv_usec = 0};
//   /*REMOVAL END HERE*/

//   big_user_info_t req;
//   memset(&req, 0, sizeof(req));
//   fill_authentication_credentials(ctx, &req.authentication);
//   req.user_id = sender_id; /* server resolves by ID when username is zeroed
//   */

//   big_header_t hdr = {.version = BIG_CHAT_VERSION,
//                       .type = TYPE_GET_USER_INFO_REQUEST,
//                       .status = 0,
//                       .reserved = 0,
//                       .body = htonl(sizeof(req))};

//   if (send(ctx->active_sock_fd, &hdr, sizeof(hdr), 0) != sizeof(hdr)) {
//     return;
//   }
//   if (send(ctx->active_sock_fd, &req, sizeof(req), 0) != sizeof(req)) {
//     return;
//   }

//   // REMOVE LATER. THIS IS ONLY IF SERVERS DON'T SUPPORT GETTING USERNAME
//   // (0X12/0X13) SO IT DOESNT HANG
//   setsockopt(ctx->active_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

//   /*REMOVAL END HERE*/

//   /* response body same struct layout: big_user_info_t */
//   big_header_t resp_hdr;
//   if (recv(ctx->active_sock_fd, &resp_hdr, sizeof(resp_hdr), MSG_WAITALL) !=
//       (ssize_t)sizeof(resp_hdr)) {
//     return;
//   }
//   if (resp_hdr.type != TYPE_GET_USER_INFO_RESPONSE) {
//     return;
//   }
//   if (resp_hdr.status != STATUS_OK) {
//     return;
//   }
//   if (ntohl(resp_hdr.body) != sizeof(big_user_info_t)) {
//     return;
//   }

//   big_user_info_t resp;
//   if (recv(ctx->active_sock_fd, &resp, sizeof(resp), MSG_WAITALL) !=
//       (ssize_t)sizeof(resp)) {
//     return;
//   }

//   /* target_username is at resp.target_username, null-padded fixed width */
//   snprintf(out_name, out_size, "%.*s", (int)sizeof(resp.target_username),
//            resp.target_username);

//   /*CHECK PREVIOUS MESSAGE ABOUT REMOVE LATER. THIS WILL BE REMOVED LATER
//   TOO*/

//   goto cleanup;
// cleanup:
//   setsockopt(ctx->active_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &zero,
//   sizeof(zero));
//   /*REMOVAL END HERE*/
// }

void lookup_username(client_context *ctx, uint8_t sender_id, char *out_name,
                     size_t out_size) {
  if (ctx->username_cached[sender_id]) {
    strncpy(out_name, ctx->username_cache[sender_id], out_size - 1);
    out_name[out_size - 1] = '\0';
    return;
  }

  // NOLINTNEXTLINE(clang-diagnostic-c23-extensions,-warnings-as-errors)
  struct timeval tv = {
      .tv_sec = 0,
      .tv_usec =
          500000}; // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers,-warnings-as-errors)
  struct timeval zero = {.tv_sec = 0, .tv_usec = 0};

  big_user_info_t req;
  memset(&req, 0, sizeof(req));
  fill_authentication_credentials(ctx, &req.authentication);
  req.user_id = sender_id;

  big_header_t hdr = {.version = BIG_CHAT_VERSION,
                      .type = TYPE_GET_USER_INFO_REQUEST,
                      .status = 0,
                      .reserved = 0,
                      .body = htonl(sizeof(req))};

  if (send(ctx->active_sock_fd, &hdr, sizeof(hdr), 0) != sizeof(hdr)) {
    return;
  }
  if (send(ctx->active_sock_fd, &req, sizeof(req), 0) != sizeof(req)) {
    return;
  }

  setsockopt(ctx->active_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  big_header_t resp_hdr;
  if (recv(ctx->active_sock_fd, &resp_hdr, sizeof(resp_hdr), MSG_WAITALL) !=
      (ssize_t)sizeof(resp_hdr)) {
    goto cleanup;
  }
  if (resp_hdr.type != TYPE_GET_USER_INFO_RESPONSE) {
    goto cleanup;
  }
  if (resp_hdr.status != STATUS_OK) {
    goto cleanup;
  }
  if (ntohl(resp_hdr.body) != sizeof(big_user_info_t)) {
    goto cleanup;
  }

  big_user_info_t resp;
  if (recv(ctx->active_sock_fd, &resp, sizeof(resp), MSG_WAITALL) !=
      (ssize_t)sizeof(resp)) {
    goto cleanup;
  }

  strncpy(out_name, resp.target_username, out_size - 1);
  out_name[out_size - 1] = '\0';

  // Cache the result
  strncpy(ctx->username_cache[sender_id], out_name, USERNAME_LENGTH - 1);
  ctx->username_cache[sender_id][USERNAME_LENGTH - 1] = '\0';
  ctx->username_cached[sender_id] = 1;

cleanup:
  setsockopt(ctx->active_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero));
}