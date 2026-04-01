#include "channels.h"
#include "client.h"
#include "network_funcs.h"
#include "protocol.h"
#include "utils.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

static void receive_header_for_channel_list(client_context *ctx,
                                            big_header_t *header) {

  ssize_t recvd = recv_all(ctx->active_sock_fd, header, sizeof(*header));

  if (recvd <= 0 || recvd != sizeof(*header)) {
    fatal_error(ctx);
  }

  if (header->type != TYPE_LIST_ALL_CHANNELS_RESPONSE ||
      header->status != STATUS_OK) {
    fatal_error(ctx);
      }
}

static void receive_body_for_channel_list(client_context *ctx,
                                          uint32_t total_size) {

  big_channel_list_t *full_list = malloc(total_size);
  if (!full_list) fatal_error(ctx);

  if (recv_all(ctx->active_sock_fd, full_list, total_size) != (ssize_t)total_size) {
    free(full_list);
    fatal_error(ctx);
  }

  ctx->channel_count = full_list->channel_id_length;
  memcpy(ctx->channel_ids, full_list->channel_id_array, ctx->channel_count);

  free(full_list);
}

static void recv_channel_join_response(client_context *ctx) {
  big_header_t hdr;

  if (recv_all(ctx->active_sock_fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
    fatal_error(ctx);
  }

  if (hdr.type != TYPE_GET_CHANNEL_INFO_RESPONSE || hdr.status != STATUS_OK) {
    fatal_error(ctx);
  }

  uint32_t body_size = ntohl(hdr.body);
  if (body_size > 0) {
    char *buf = malloc(body_size);
    if (buf) {
      recv_all(ctx->active_sock_fd, buf, body_size);
      free(buf);
    }
  }
}