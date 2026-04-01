#include "messaging.h"
#include "network_funcs.h"
#include "protocol.h"
#include "utils.h"
#include <arpa/inet.h>

#if defined(__FreeBSD__)
#include <sys/endian.h>
#else
#include <endian.h>
#endif

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// 🔥 helper (ADD THIS)
static ssize_t recv_all(int sock, void *buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    ssize_t n = recv(sock, (char *)buf + total, len - total, 0);
    if (n <= 0) return n;
    total += n;
  }
  return total;
}

static void discard_body(client_context *ctx, uint32_t body_size);

void network_execute_messaging_loop(client_context *ctx) {
  (void)ctx;
}

void network_send_message(client_context *ctx, const char *text) {
  uint16_t msg_len = (uint16_t)strlen(text);
  size_t body_size = sizeof(big_send_message_t) + msg_len;
  size_t total_size = sizeof(big_header_t) + body_size;

  uint8_t *buf = calloc(1, total_size);
  if (!buf) fatal_error(ctx);

  big_header_t hdr = {
      .version = BIG_CHAT_VERSION,
      .type = TYPE_SEND_MESSAGE_REQUEST,
      .status = STATUS_OK,
      .reserved = 0,
      .body = htonl((uint32_t)body_size)};

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
    fatal_error(ctx);
  }
  free(buf);
}

int network_receive_pending(client_context *ctx,
                            void (*on_message)(const char *, const char *, void *),
                            void *userdata) {

  struct pollfd pfd = {.fd = ctx->active_sock_fd, .events = POLLIN};
  if (poll(&pfd, 1, 0) <= 0) return 0;

  big_header_t header;
  if (recv_all(ctx->active_sock_fd, &header, sizeof(header)) <= 0)
    return -1;

  uint32_t body_size = ntohl(header.body);

  if (header.type == TYPE_SEND_MESSAGE_RESPONSE) {
    discard_body(ctx, body_size);
    return 0;
  }

  if (header.type == TYPE_GET_MESSAGE_RESPONSE) {
    big_get_message_t *msg = malloc(body_size + 1);
    if (!msg) return -1;

    if (recv_all(ctx->active_sock_fd, msg, body_size) != (ssize_t)body_size) {
      free(msg);
      return -1;
    }

    char sender_name[USERNAME_LENGTH + 1] = {0};
    lookup_username(ctx, msg->sender_id, sender_name, sizeof(sender_name));

    uint16_t msg_len = ntohs(msg->message_length);
    char *text = (char *)msg->message;
    text[msg_len] = '\0';

    if (on_message) on_message(sender_name, text, userdata);

    free(msg);
    return 1;
  }

  discard_body(ctx, body_size);
  return 0;
}

static void discard_body(client_context *ctx, uint32_t body_size) {
  if (body_size == 0) return;
  char *buf = malloc(body_size);
  if (!buf) return;
  recv_all(ctx->active_sock_fd, buf, body_size);
  free(buf);
}