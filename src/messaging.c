#include "messaging.h"
#include "client.h"
#include "protocol.h"
#include "utils.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// Local constants — same pattern as private enums in ui.c (CLAUDE.md: "Private
// constants in .c files")
enum {
  POLL_FD_COUNT =
      2, // stdin + socket (D'Arcy poll/main.c:82 uses client_count + 1)
  POLL_IDX_STDIN = 0,
  POLL_IDX_SOCKET = 1,
  MSG_INPUT_BUF_SIZE = 1024 // stdin line buffer for user input
};

// Signal flag for clean shutdown — cloned from D'Arcy poll/main.c:43
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile sig_atomic_t exit_flag = 0;

// Cloned from network_funcs.c:366 — same fatal_error pattern
static void fatal_error(client_context *ctx, char *msg);

// Signal handler — cloned from D'Arcy poll/main.c:130-154
static void setup_signal_handler(void);
static void sigint_handler(int signum);

// RFC 6.7.1: Message Create — send message to channel (type byte 0x30)
// Pattern: send_login_logout_request() (network_funcs.c:373-412)
static void send_message_request(client_context *ctx, uint8_t channel_id,
                                 const char *message, uint16_t msg_len);

// RFC 6.7.1: Message Create Response (type byte 0x31)
// Pattern: recv_login_logout_response() (network_funcs.c:415-457)
static void recv_message_response(client_context *ctx);

// RFC 6.7.2: Message Read Response (type byte 0x33) — unsolicited (RFC 7.1)
// Called from event loop after header already recv'd
static void recv_incoming_message(client_context *ctx, const big_header_t *hdr);

// --- fatal_error (cloned from network_funcs.c:366-370) ---
static void fatal_error(client_context *ctx, char *msg) {
  ctx->exit_code = EXIT_FAILURE;
  ctx->exit_message = msg;
  quit(ctx);
}

// --- setup_signal_handler (cloned from D'Arcy poll/main.c:130-143) ---
static void setup_signal_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#endif
  sa.sa_handler = sigint_handler;
#ifdef __clang__
#pragma clang diagnostic pop
#endif
  (void)sigaction(SIGINT, &sa, NULL);
}

// --- sigint_handler (cloned from D'Arcy poll/main.c:148-152) ---
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

static void sigint_handler(int signum) { exit_flag = 1; }

#pragma GCC diagnostic pop

// --- send_message_request ---
// RFC 6.7.1: Message Create Request (type byte 0x30, RFC Appendix A)
// ASN.1: SendMessage ::= SEQUENCE { auth Auth, timestamp Timestamp, messageLen
// UInt16, channelId ChannelId, message OCTET STRING } Hexpat: struct
// Send_Message { Auth auth; Timestamp timestamp; be u16 messageLen; ChannelId
// channelId; char messageArr[messageLen]; } Body: Auth (32) + Timestamp (8) +
// MessageLen (2, BE) + ChannelId (1) + Message (variable) = 43 + msg_len
// Pattern: send_login_logout_request() (network_funcs.c:373-412)
static void send_message_request(client_context *ctx, uint8_t channel_id,
                                 const char *message, uint16_t msg_len) {
  // Fixed part of body — flexible array member excluded from sizeof
  big_send_message_t body;
  memset(&body, 0, sizeof(body));

  // Fill Auth credentials — same pattern as network_funcs.c:377-378
  strncpy(body.authentication.username, ctx->username, USERNAME_LENGTH);
  strncpy(body.authentication.password, ctx->password, PASSWORD_LENGTH);

  // TODO(M2): get current timestamp via time(NULL), convert to big-endian
  // (htobe64) Expected: body.timestamp = htobe64((uint64_t)time(NULL));
  // RFC 5.2: "64-bit unsigned integer in big-endian byte order representing
  // seconds since Unix epoch"
  body.timestamp = 0;

  // TODO(M2): convert messageLen to big-endian (htons)
  // Expected: body.message_length = htons(msg_len);
  // RFC 6.7.1: "MessageLen (2 bytes, big-endian): Length of the Message content
  // in bytes"
  body.message_length = 0;

  body.channel_id = channel_id;

  // Build header — body size is variable: sizeof(fixed_fields) + msg_len
  // RFC Appendix C: Message Create min body = 43 bytes
  // Pattern: same as network_funcs.c:397-401 but body size includes variable
  // part
  uint32_t total_body_size = (uint32_t)sizeof(body) + (uint32_t)msg_len;

  big_header_t req = {
      .version = BIG_CHAT_VERSION,
      .type = TYPE_SEND_MESSAGE_REQUEST, // 0x30 (RFC 6.7.1)
      .status = 0,
      .reserved = 0,
      .body = htonl(total_body_size) // big-endian (RFC 4.4)
  };

  // send header — same pattern as network_funcs.c:404-407
  if (send(ctx->active_sock_fd, &req, sizeof(req), 0) != sizeof(req)) {
    perror("send");
    fatal_error(ctx, "Network Error: Failed to send message request header.\n");
  }

  // send fixed body fields — same pattern as network_funcs.c:409-412
  if (send(ctx->active_sock_fd, &body, sizeof(body), 0) != sizeof(body)) {
    perror("send");
    fatal_error(ctx, "Network Error: Failed to send message request body.\n");
  }

  // send message text (variable part) — new: no existing pattern, split send
  // for variable-length body
  if (msg_len > 0) {
    if (send(ctx->active_sock_fd, message, (size_t)msg_len, 0) !=
        (ssize_t)msg_len) {
      perror("send");
      fatal_error(ctx, "Network Error: Failed to send message text.\n");
    }
  }
}

// --- recv_message_response ---
// RFC 6.7.1: Message Create Response (type byte 0x31, RFC Appendix A)
// Pattern: recv_login_logout_response() (network_funcs.c:415-457) — nearly
// identical
static void recv_message_response(client_context *ctx) {
  big_header_t hdr;

  // recv header — same pattern as network_funcs.c:418
  ssize_t recvd = recv(ctx->active_sock_fd, &hdr, sizeof(hdr), MSG_WAITALL);

  // check for disconnect — same pattern as network_funcs.c:420-422
  if (recvd <= 0) {
    fatal_error(ctx, "Server disconnected during message send.\n");
  }

  if (recvd != sizeof(hdr)) {
    fatal_error(ctx, "Incomplete message send response.\n");
  }

  // validate type — same pattern as network_funcs.c:428-430
  if (hdr.type != TYPE_SEND_MESSAGE_RESPONSE) {
    fatal_error(ctx,
                "Protocol Error: Expected Send Message Response (0x31).\n");
  }

  // check status — same pattern as network_funcs.c:432-435
  if (hdr.status != STATUS_OK) {
    fprintf(stderr, "Server Error Code: 0x%02X\n", hdr.status);
    fatal_error(ctx, "Send Message Failed: Server returned error.\n");
  }

  // drain any response body — same pattern as network_funcs.c:438-456
  uint32_t bsize = ntohl(hdr.body);
  if (bsize > 0) {
    char *junk = malloc(bsize);

    if (junk == NULL) {
      fatal_error(ctx, "Fatal: Out of memory.\n");
      return;
    }

    ssize_t recvd_body =
        recv(ctx->active_sock_fd, junk, (size_t)bsize, MSG_WAITALL);

    if (recvd_body != (ssize_t)bsize) {
      free(junk);
      fatal_error(ctx, "Failed to read message send response body.\n");
      return;
    }
    free(junk);
  }
}

// --- recv_incoming_message ---
// RFC 6.7.2: Message Read Response (type byte 0x33) — unsolicited push from
// server (RFC 7.1) ASN.1: GetMessage ::= SEQUENCE { auth Auth, timestamp
// Timestamp, messageLen UInt16, channelId ChannelId, senderId UserId, message
// OCTET STRING } Hexpat: struct Get_Message { Auth auth; Timestamp timestamp;
// be u16 messageLen; ChannelId channelId; UserId senderId; char
// messageArr[messageLen]; } Body: Auth (32) + Timestamp (8) + MessageLen (2,
// BE) + ChannelId (1) + SenderId (1) + Message (variable) = 44 + msgLen Header
// already recv'd in event loop — body size available from hdr->body
static void recv_incoming_message(client_context *ctx,
                                  const big_header_t *hdr) {
  uint32_t body_len = ntohl(hdr->body);

  // TODO(M2): Implement all steps below, then REMOVE the drain block at the
  // bottom of this function. The drain reads the same bytes — keeping both will
  // double-read and desync the TCP stream.
  //
  // Step 1: read fixed part of big_get_message_t body (44 bytes)
  //   big_get_message_t fixed_body;
  //   if (body_len < sizeof(big_get_message_t))
  //     fatal_error(ctx, "Body too small for incoming message.\n");
  //   recv(ctx->active_sock_fd, &fixed_body, sizeof(fixed_body), MSG_WAITALL);
  //   (check recv return value)
  //
  // Step 2: convert fields from big-endian
  //   uint16_t msg_len = ntohs(fixed_body.message_length);
  //   uint64_t ts = be64toh(fixed_body.timestamp);
  //   NOTE: be64toh is non-POSIX (Linux <endian.h>). For macOS/FreeBSD
  //   portability, use a manual htonll() helper via two htonl() calls.
  //   NOTE: #include <time.h> needed for timestamp display.
  //
  // Step 3: read variable message text
  //   char *msg_buf = malloc(msg_len + 1);
  //   (check malloc return — fatal_error on NULL)
  //   recv(ctx->active_sock_fd, msg_buf, msg_len, MSG_WAITALL);
  //   (check recv return value)
  //   msg_buf[msg_len] = '\0';
  //
  // Step 4: print message and clean up
  //   printf("[channel %u] user %u (ts=%llu): %s\n", fixed_body.channel_id,
  //          fixed_body.sender_id, (unsigned long long)ts, msg_buf);
  //   free(msg_buf);

  // STUB: Drain entire body for now — REMOVE when implementing above steps.
  // Pattern from recv_login_logout_response() (network_funcs.c:438-456)
  if (body_len > 0) {
    char *junk = malloc(body_len);

    if (junk == NULL) {
      fatal_error(ctx, "Fatal: Out of memory.\n");
      return;
    }

    ssize_t recvd =
        recv(ctx->active_sock_fd, junk, (size_t)body_len, MSG_WAITALL);

    if (recvd != (ssize_t)body_len) {
      free(junk);
      fatal_error(ctx, "Failed to read incoming message body.\n");
      return;
    }
    free(junk);
  }
}

// --- network_execute_messaging_loop ---
// Messaging event loop (RFC 6.7.1, 6.7.2, 7.1)
// Pattern: D'Arcy poll/main.c:80-101 — poll() with exit_flag
// Adapted: 2 fds (stdin + socket) instead of dynamic client array
void network_execute_messaging_loop(client_context *ctx) {
  printf("\n--- Phase 5: Messaging Loop ---\n");
  printf("Type messages and press Enter to send. Ctrl+C to exit.\n");

  // Signal handler for clean exit — cloned from D'Arcy poll/main.c:53
  setup_signal_handler();

  ctx->in_messaging_loop = 1;

  // Set up poll fds — adapted from D'Arcy poll/main.c:78-82
  // fds[0] = stdin (POLLIN), fds[1] = socket (POLLIN)
  struct pollfd fds[POLL_FD_COUNT];

  fds[POLL_IDX_STDIN].fd = STDIN_FILENO;
  fds[POLL_IDX_STDIN].events = POLLIN;
  fds[POLL_IDX_STDIN].revents = 0;

  fds[POLL_IDX_SOCKET].fd = ctx->active_sock_fd;
  fds[POLL_IDX_SOCKET].events = POLLIN;
  fds[POLL_IDX_SOCKET].revents = 0;

  // Event loop — adapted from D'Arcy poll/main.c:80-101
  while (!exit_flag) {
    // poll() blocks until activity — same pattern as D'Arcy poll/main.c:82
    int activity = poll(fds, POLL_FD_COUNT, -1);

    // Handle EINTR from signal — pattern from D'Arcy select/main.c:138-141
    if (activity < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("poll");
      fatal_error(ctx, "Fatal: poll() error in messaging loop.\n");
    }

    // stdin ready — user typed a message
    if (fds[POLL_IDX_STDIN].revents & POLLIN) {
      char input[MSG_INPUT_BUF_SIZE];

      // Read line from stdin — adapted from get_user_input() (utils.c:34-56)
      if (fgets(input, (int)sizeof(input), stdin) != NULL) {
        // Strip newline — same pattern as get_user_input() (utils.c:47-52)
        for (size_t i = 0; i < sizeof(input) && input[i] != '\0'; i++) {
          if (input[i] == '\n') {
            input[i] = '\0';
            break;
          }
        }

        // Only send non-empty messages
        if (input[0] != '\0') {
          uint16_t msg_len = (uint16_t)strlen(input);
          send_message_request(ctx, ctx->current_channel_id, input, msg_len);
          recv_message_response(ctx);
        }
      } else {
        // EOF on stdin (Ctrl+D) — exit loop
        break;
      }

      // Clear revents — D'Arcy poll/main.c:100
      fds[POLL_IDX_STDIN].revents = 0;
    }

    // socket ready — incoming data from server
    if (fds[POLL_IDX_SOCKET].revents & POLLIN) {
      big_header_t hdr;

      // recv header — same pattern as recv_discovery_response()
      // (network_funcs.c:224)
      ssize_t recvd = recv(ctx->active_sock_fd, &hdr, sizeof(hdr), MSG_WAITALL);

      if (recvd <= 0) {
        fprintf(stderr, "Server disconnected.\n");
        break;
      }

      if (recvd != sizeof(hdr)) {
        fatal_error(ctx, "Failed to receive header in messaging loop.\n");
      }

      // Dispatch by type byte — RFC Appendix A
      switch (hdr.type) {
      case TYPE_GET_MESSAGE_RESPONSE: // 0x33 (RFC 6.7.2) — unsolicited message
                                      // push
        recv_incoming_message(ctx, &hdr);
        break;

      default:
        // Unexpected type: log warning and drain body to keep connection in
        // sync
        fprintf(stderr,
                "Warning: Unexpected message type 0x%02X in messaging loop\n",
                hdr.type);
        {
          uint32_t bsize = ntohl(hdr.body);
          if (bsize > 0) {
            char *junk = malloc(bsize);

            if (junk == NULL) {
              fatal_error(ctx, "Fatal: Out of memory in drain.\n");
              return;
            }

            ssize_t r =
                recv(ctx->active_sock_fd, junk, (size_t)bsize, MSG_WAITALL);

            if (r != (ssize_t)bsize) {
              free(junk);
              fatal_error(ctx, "Failed to drain unexpected message body.\n");
              return;
            }
            free(junk);
          }
        }
        break;
      }

      // Clear revents — D'Arcy poll/main.c:100
      fds[POLL_IDX_SOCKET].revents = 0;
    }
  }

  ctx->in_messaging_loop = 0;
  printf("Messaging loop ended.\n");
}
