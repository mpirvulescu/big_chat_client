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

static big_header_t make_header(uint8_t type, uint32_t body_size) {
  big_header_t hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.version = BIG_CHAT_VERSION;
  hdr.type = type;
  hdr.status = STATUS_OK;
  hdr.reserved = 0;
  hdr.body = htonl(body_size);
  return hdr;
}

void network_execute_messaging_loop(client_context *ctx) {
  (void)ctx;
  // No-op: the ncurses UI drives the messaging loop directly via
  // network_send_message() and network_receive_pending().
}

uint64_t network_send_message(client_context *ctx, const char *text) {
  uint16_t msg_len = (uint16_t)strlen(text);
  size_t body_size = sizeof(big_send_message_t) + msg_len;
  size_t total_size = sizeof(big_header_t) + body_size;

  uint8_t *buf = calloc(1, total_size);
  if (!buf) {
    ctx->error_message = "Fatal: Out of memory preparing message.\n";
    fatal_error(ctx);
  }

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  uint64_t ms_ts =
      ((uint64_t)ts.tv_sec * MS_PER_SEC) + (uint64_t)(ts.tv_nsec / NS_PER_MS);

  big_header_t hdr =
      make_header(TYPE_SEND_MESSAGE_REQUEST, (uint32_t)body_size);

  big_send_message_t body = {0};
  memset(&body, 0, sizeof(body));
  fill_authentication_credentials(ctx, &body.authentication);
  body.timestamp = htobe64(ms_ts);
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
  return ms_ts;
}

int network_receive_pending(client_context *ctx,
                            void (*on_message)(const char *sender_id,
                                               const char *text,
                                               const void *userdata),
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

  if (header.type == TYPE_EDIT_MESSAGE_RESPONSE ||
      header.type == TYPE_DELETE_MESSAGE_RESPONSE) {
    discard_body(ctx, body_size);
    return 0;
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

void network_fetch_history(client_context *ctx,
                           void (*on_message)(const char *sender_name,
                                              const char *text,
                                              const void *userdata,
                                              uint64_t timestamp,
                                              uint8_t sender_id),
                           void *userdata, uint16_t limit) {
  /* ---- Build GET_HISTORY request ---- */
  big_get_history_t req;
  memset(&req, 0, sizeof(req));
  fill_authentication_credentials(ctx, &req.authentication);
  req.start_timestamp = htobe64(0ULL); /* from the very beginning */
  req.result_len_limit = htons(limit);
  req.result_len = 0;
  req.channel_id = ctx->current_channel_id;

  big_header_t hdr =
      make_header(TYPE_GET_HISTORY_REQUEST, (uint32_t)sizeof(req));

  if (send(ctx->active_sock_fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) {
    return;
  }
  if (send(ctx->active_sock_fd, &req, sizeof(req), 0) != (ssize_t)sizeof(req)) {
    return;
  }

  /* ---- Read response header ---- */
  /* Use a generous timeout for history fetch */
  struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
  struct timeval zero = {.tv_sec = 0, .tv_usec = 0};
  setsockopt(ctx->active_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  big_header_t resp_hdr;
  if (recv(ctx->active_sock_fd, &resp_hdr, sizeof(resp_hdr), MSG_WAITALL) !=
      (ssize_t)sizeof(resp_hdr)) {
    goto cleanup;
  }
  if (resp_hdr.type != TYPE_GET_HISTORY_RESPONSE) {
    discard_body(ctx, ntohl(resp_hdr.body));
    goto cleanup;
  }
  if (resp_hdr.status != STATUS_OK) {
    discard_body(ctx, ntohl(resp_hdr.body));
    goto cleanup;
  }

  uint32_t body_size = ntohl(resp_hdr.body);
  if (body_size < sizeof(big_get_history_t)) {
    discard_body(ctx, body_size);
    goto cleanup;
  }

  /* ---- Read fixed part of response body ---- */
  big_get_history_t resp;
  if (recv(ctx->active_sock_fd, &resp, sizeof(resp), MSG_WAITALL) !=
      (ssize_t)sizeof(resp)) {
    /* drain whatever remains */
    uint32_t remaining = body_size - (uint32_t)sizeof(resp);
    discard_body(ctx, remaining);
    goto cleanup;
  }

  uint16_t result_count = ntohs(resp.result_len);
  uint32_t array_bytes =
      ((uint32_t)result_count * TIMESTAMP_SIZE_BYTES) /* timestamps */
      + ((uint32_t)result_count * 1U);                /* sender_ids  */
  uint32_t fixed_read = (uint32_t)sizeof(resp);

  if (result_count == 0 || body_size < fixed_read + array_bytes) {
    /* Nothing to show or malformed — drain remainder */
    uint32_t leftover = body_size - fixed_read;
    discard_body(ctx, leftover);
    goto cleanup;
  }

  /* Allocate arrays */
  uint64_t *timestamps = malloc((size_t)result_count * sizeof(uint64_t));
  uint8_t *sender_ids = malloc((size_t)result_count);
  if (!timestamps || !sender_ids) {
    free(timestamps);
    free(sender_ids);
    uint32_t leftover = body_size - fixed_read;
    discard_body(ctx, leftover);
    goto cleanup;
  }

  /* Read timestamps array (8 * ResultLen bytes) */
  if (recv(ctx->active_sock_fd, timestamps,
           (size_t)result_count * sizeof(uint64_t),
           MSG_WAITALL) != (ssize_t)((size_t)result_count * sizeof(uint64_t))) {
    free(timestamps);
    free(sender_ids);
    goto cleanup;
  }

  /* Read sender_ids array (1 * ResultLen bytes) */
  if (recv(ctx->active_sock_fd, sender_ids, (size_t)result_count,
           MSG_WAITALL) != (ssize_t)result_count) {
    free(timestamps);
    free(sender_ids);
    goto cleanup;
  }

  /* Drain any unexpected trailing bytes */
  uint32_t consumed = fixed_read + array_bytes;
  if (body_size > consumed) {
    discard_body(ctx, body_size - consumed);
  }

  /* Temporarily restore timeout to 1 s for individual message fetches */
  struct timeval msg_tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(ctx->active_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &msg_tv,
             sizeof(msg_tv));

  /*
   * For each (timestamp, sender_id) pair, fetch the actual message text
   * with a GET_MESSAGE_REQUEST (0x32).
   */
  uint16_t i;
  for (i = 0; i < result_count; i++) {
    uint64_t ts_be = timestamps[i]; /* already big-endian from wire */
    uint64_t ts_host = be64toh(ts_be);
    uint8_t sid = sender_ids[i];

    /* Build GET_MESSAGE_REQUEST */
    big_get_message_t greq;
    memset(&greq, 0, sizeof(greq));
    fill_authentication_credentials(ctx, &greq.authentication);
    greq.timestamp = ts_be; /* send back in wire byte order */
    greq.message_length = 0;
    greq.channel_id = ctx->current_channel_id;
    greq.sender_id = sid;

    big_header_t ghdr =
        make_header(TYPE_GET_MESSAGE_REQUEST, (uint32_t)sizeof(greq));

    if (send(ctx->active_sock_fd, &ghdr, sizeof(ghdr), 0) !=
        (ssize_t)sizeof(ghdr)) {
      break;
    }
    if (send(ctx->active_sock_fd, &greq, sizeof(greq), 0) !=
        (ssize_t)sizeof(greq)) {
      break;
    }

    /* Read response */
    big_header_t gresp_hdr;
    if (recv(ctx->active_sock_fd, &gresp_hdr, sizeof(gresp_hdr), MSG_WAITALL) !=
        (ssize_t)sizeof(gresp_hdr)) {
      break;
    }

    uint32_t gresp_body = ntohl(gresp_hdr.body);

    if (gresp_hdr.type != TYPE_GET_MESSAGE_RESPONSE ||
        gresp_hdr.status != STATUS_OK ||
        gresp_body < sizeof(big_get_message_t)) {
      discard_body(ctx, gresp_body);
      continue;
    }

    big_get_message_t *gmsg = malloc(gresp_body + 1);
    if (!gmsg) {
      discard_body(ctx, gresp_body);
      continue;
    }

    if (recv(ctx->active_sock_fd, gmsg, gresp_body, MSG_WAITALL) !=
        (ssize_t)gresp_body) {
      free(gmsg);
      break;
    }

    uint16_t msg_len = ntohs(gmsg->message_length);
    uint32_t max_len = gresp_body - (uint32_t)sizeof(big_get_message_t);
    if ((uint32_t)msg_len > max_len) {
      msg_len = (uint16_t)max_len;
    }
    char *text = (char *)gmsg->message;
    text[msg_len] = '\0';

    /* Resolve sender name */
    char sender_name[USERNAME_LENGTH + 1];
    memset(sender_name, 0, sizeof(sender_name));
    lookup_username(ctx, sid, sender_name, sizeof(sender_name));
    if (sender_name[0] == '\0') {
      snprintf(sender_name, sizeof(sender_name), "[%u]", (unsigned int)sid);
    }

    if (on_message) {
      on_message(sender_name, text, userdata, ts_host, sid);
    }

    free(gmsg);
  }

  free(timestamps);
  free(sender_ids);

cleanup:
  setsockopt(ctx->active_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero));
}

/* network_edit_message  (0x34 / 0x35) */

int network_edit_message(client_context *ctx, uint64_t original_timestamp,
                         const char *new_text) {
  uint16_t msg_len = (uint16_t)strlen(new_text);
  size_t body_size = sizeof(big_edit_message_t) + msg_len;
  size_t total = sizeof(big_header_t) + body_size;

  uint8_t *buf = calloc(1, total);
  if (!buf) {
    return -1;
  }

  big_header_t hdr =
      make_header(TYPE_EDIT_MESSAGE_REQUEST, (uint32_t)body_size);

  big_edit_message_t body;
  memset(&body, 0, sizeof(body));
  fill_authentication_credentials(ctx, &body.authentication);
  body.timestamp = htobe64(original_timestamp);
  body.message_length = htons(msg_len);
  body.channel_id = ctx->current_channel_id;

  uint8_t *p = buf;
  memcpy(p, &hdr, sizeof(hdr));
  p += sizeof(hdr);
  memcpy(p, &body, sizeof(body));
  p += sizeof(body);
  memcpy(p, new_text, msg_len);

  int sent_ok = (send(ctx->active_sock_fd, buf, total, 0) == (ssize_t)total);
  free(buf);

  if (!sent_ok) {
    return -1;
  }

  /* Wait for ACK */
  struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
  struct timeval zero = {.tv_sec = 0, .tv_usec = 0};
  setsockopt(ctx->active_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  big_header_t resp;
  int rc = 0;
  if (recv(ctx->active_sock_fd, &resp, sizeof(resp), MSG_WAITALL) !=
      (ssize_t)sizeof(resp)) {
    rc = -1;
    goto cleanup;
  }
  discard_body(ctx, ntohl(resp.body));
  if (resp.type != TYPE_EDIT_MESSAGE_RESPONSE || resp.status != STATUS_OK) {
    rc = -1;
  }

cleanup:
  setsockopt(ctx->active_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero));
  return rc;
}

/* network_delete_message  (0x36 / 0x37)*/

int network_delete_message(client_context *ctx, uint64_t original_timestamp) {
  big_delete_message_t body;
  memset(&body, 0, sizeof(body));
  fill_authentication_credentials(ctx, &body.authentication);
  body.timestamp = htobe64(original_timestamp);
  body.channel_id = ctx->current_channel_id;

  big_header_t hdr =
      make_header(TYPE_DELETE_MESSAGE_REQUEST, (uint32_t)sizeof(body));

  if (send(ctx->active_sock_fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) {
    return -1;
  }
  if (send(ctx->active_sock_fd, &body, sizeof(body), 0) !=
      (ssize_t)sizeof(body)) {
    return -1;
  }

  /* Wait for ACK */
  struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
  struct timeval zero = {.tv_sec = 0, .tv_usec = 0};
  setsockopt(ctx->active_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  big_header_t resp;
  int rc = 0;
  if (recv(ctx->active_sock_fd, &resp, sizeof(resp), MSG_WAITALL) !=
      (ssize_t)sizeof(resp)) {
    rc = -1;
    goto cleanup;
  }
  discard_body(ctx, ntohl(resp.body));
  if (resp.type != TYPE_DELETE_MESSAGE_RESPONSE || resp.status != STATUS_OK) {
    rc = -1;
  }

cleanup:
  setsockopt(ctx->active_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero));
  return rc;
}