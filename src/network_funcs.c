#include "network_funcs.h"
#include "channels.h"
#include "client.h"
#include "protocol.h"
#include "ui.h"
#include "utils.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// these two for network_execute_discovery
static void send_discovery_request(client_context *ctx);
static void recv_discovery_response(client_context *ctx,
                                    big_discovery_res_t *dest);

// helpers for account creation
static void send_account_creation_request(client_context *ctx);
static void recv_account_creation_response(client_context *ctx);

// helpers for login/logout
static void send_login_logout_request(client_context *ctx, uint8_t status_flag);
static void recv_login_logout_response(client_context *ctx);

// Ensures full buffer is received by looping until all bytes are read
static ssize_t recv_all(int sock, void *buffer, size_t length);

// Ensures full buffer is sent by handling partial send calls
static ssize_t send_all(int sock, const void *buffer, size_t length);

int convert_address(client_context *ctx) {
  memset(&ctx->addr, 0, sizeof(ctx->addr));

  if (inet_pton(AF_INET, ctx->manager_ip,
                &(((struct sockaddr_in *)&ctx->addr)->sin_addr)) == 1) {
    ctx->addr.ss_family = AF_INET;
    return 0;
  }

  return -1;
}

void fill_authentication_credentials(client_context *ctx, big_auth_t *auth) {
  memset(auth, 0, sizeof(big_auth_t));
  strncpy(auth->username, ctx->username, USERNAME_LENGTH - 1);
  strncpy(auth->password, ctx->password, PASSWORD_LENGTH - 1);
}

void socket_create(client_context *ctx) {
  // NOLINTNEXTLINE(android-cloexec-socket)
  ctx->active_sock_fd = socket(ctx->addr.ss_family, SOCK_STREAM, 0);

  if (ctx->active_sock_fd == -1) {
    ctx->error_message = "Fatal: Could not create socket.\n";
    fatal_error(ctx);
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
    ctx->error_message = "Fatal: internal address error.\n";
    fatal_error(ctx);
  }

  // printf("Connecting to: %s:%u\n", addr_str, port);

  // convert port to network byte order
  net_port = htons(port);
  ipv4_ptr->sin_port = net_port;
  addr_len = sizeof(struct sockaddr_in);

  // connection call
  if (connect(ctx->active_sock_fd, (struct sockaddr *)ipv4_ptr, addr_len) ==
      -1) {
    // fprintf(stderr, "Error: connect (%d): %s\n", errno, strerror(errno));
    ctx->error_message = "Fatal: Could not connect to server.\n";
    fatal_error(ctx);
  }

  // printf("Successfully connected to: %s:%u\n", addr_str, port);
}

void network_execute_discovery(client_context *ctx) {
  // printf("\n--- Phase 1: Server Discovery ---\n");

  // setup connection to manager
  if (convert_address(ctx) != 0) {
    ctx->error_message = "Invalid Manager IP format.\n";
    fatal_error(ctx);
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
  // struct in_addr node_ip_addr;
  // node_ip_addr.s_addr = response.ip_address;

  char node_ip_str[INET_ADDRSTRLEN];
  snprintf(node_ip_str, sizeof(node_ip_str), "%u.%u.%u.%u",
           response.ip_address.a, response.ip_address.b, response.ip_address.c,
           response.ip_address.d);
  // fprintf(stderr, "DEBUG: redirected to chat node: %s\n", node_ip_str);
  //  if (inet_ntop(AF_INET, &node_ip_addr, node_ip_str, sizeof(node_ip_str)) ==
  //      NULL) {
  //    fatal_error(ctx, "Discovery Error: Invalid IP received from
  //    Manager.\n");
  //  }

  // printf("Redirecting to Chat Node %d at %s\n", response.server_id,
  // node_ip_str);

  // overwrite manager IP with new chat node IP
  snprintf(ctx->manager_ip, sizeof(ctx->manager_ip), "%s", node_ip_str);

  // clean up manager socket
  close(ctx->active_sock_fd);
  ctx->active_sock_fd = -1;

  // update the state
  ctx->state = STATE_AWAITING_USER_INFO;
}

void network_execute_account_creation(client_context *ctx) {
  // printf("\n--- Phase 2: Account Registration ---\n");

  // re-establish connection (discovery closed it, IP is updated)
  if (convert_address(ctx) != 0) {
    ctx->error_message = "Invalid Server IP format.\n";
    fatal_error(ctx);
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

  // printf("Registration Successful. Account created.\n");
}

void network_execute_login(client_context *ctx) {
  // printf("\n--- Phase 3: Login ---\n");

  if (convert_address(ctx) != 0) {
    ctx->error_message = "Invalid Server IP format.\n";
    fatal_error(ctx);
  }

  socket_create(ctx);
  socket_connect(ctx, ctx->manager_port);

  send_login_logout_request(ctx, 1);
  recv_login_logout_response(ctx);

  // cleanup connection
  // close(ctx->active_sock_fd);
  // ctx->active_sock_fd = -1;

  // printf("Login Successful.\n");
}

void network_execute_logout(client_context *ctx) {
  // printf("\n--- Phase 4: Logout ---\n");

  if (convert_address(ctx) != 0) {
    ctx->error_message = "Invalid Server IP format.\n";
    fatal_error(ctx);
  }

  socket_create(ctx);
  socket_connect(ctx, ctx->manager_port);

  send_login_logout_request(ctx, 0);
  recv_login_logout_response(ctx);

  // cleanup connection
  close(ctx->active_sock_fd);
  ctx->active_sock_fd = -1;

  // printf("Logout Successful.\n");
}

// void network_execute_login(client_context *ctx) {}

static void send_discovery_request(client_context *ctx) {
  big_discovery_res_t body = {0};

  big_header_t req = {.version = BIG_CHAT_VERSION,
                      .type = TYPE_DISCOVERY_REQUEST,
                      .status = 0,
                      .reserved = 0,
                      .body = htonl(sizeof(body))};

  // send header
  if (send_all(ctx->active_sock_fd, &req, sizeof(req)) != sizeof(req)) {
    ctx->error_message = "Network Error: Failed to send discovery request.\n";
    fatal_error(ctx);
  }
  // send body
  if (send_all(ctx->active_sock_fd, &body, sizeof(body)) != sizeof(body)) {
    ctx->error_message = "Network Error: Failed to send discovery body.\n";
    fatal_error(ctx);
  }
}

static void recv_discovery_response(client_context *ctx,
                                    big_discovery_res_t *dest) {
  big_header_t hdr;

  // read header
  ssize_t recvd = recv_all(ctx->active_sock_fd, &hdr, sizeof(hdr));

  if (recvd == 0) {
    ctx->error_message = "Server closed connection unexpectedly.\n";
    fatal_error(ctx);
  }

  if (recvd != sizeof(hdr)) {
    ctx->error_message = "Failed to receive protocol header.\n";
    fatal_error(ctx);
  }

  // validate packet
  if (hdr.type != TYPE_DISCOVERY_RESPONSE) {
    ctx->error_message =
        "Protocol Error: Invalid response type from Manager.\n";
    fatal_error(ctx);
  }

  // check status - RFC Section 4.3.5: response status MUST reflect result
  // added to prevent code from parsing body if status NOT valid
  if (hdr.status != STATUS_OK) {
    // fprintf(stderr, "Manager Error Code: 0x%02X\n", hdr.status);
    ctx->error_message = "Discovery Failed: Manager returned error.\n";
    fatal_error(ctx);
  }

  // convert from network order to host order
  uint32_t body_len = ntohl(hdr.body);
  if (body_len != sizeof(big_discovery_res_t)) {
    // fprintf(stderr, "Protocol Error: Expected body size %zu, got %u\n",
    // sizeof(big_discovery_res_t), body_len);
    ctx->error_message = "Invalid discovery response size.\n";
    fatal_error(ctx);
  }

  // read body into dest
  recvd = recv_all(ctx->active_sock_fd, dest, sizeof(big_discovery_res_t));

  if (recvd != sizeof(big_discovery_res_t)) {
    ctx->error_message = "Failed to receive discovery body.\n";
    fatal_error(ctx);
  }
}

static void send_account_creation_request(client_context *ctx) {
  big_create_account_req_t body = {0};

  strncpy(body.authentication.username, ctx->username,
          sizeof(body.authentication.username));
  strncpy(body.authentication.password, ctx->password,
          sizeof(body.authentication.password));

  body.client_id = 0; // 0 for new account?
  // body.status = 0x01; // as per protocol // DG: disabling for now

  big_header_t req = {
      .version = BIG_CHAT_VERSION,
      .type = TYPE_ACCOUNT_CREATE_REQUEST,
      .status = 0,
      .reserved = 0,
      .body = htonl(sizeof(body)) // worry about endianness i think
  };

  // send header
  if (send_all(ctx->active_sock_fd, &req, sizeof(req)) != sizeof(req)) {
    ctx->error_message = "Network Error: Failed to send register header.\n";
    fatal_error(ctx);
  }

  // send body
  if (send_all(ctx->active_sock_fd, &body, sizeof(body)) != sizeof(body)) {
    ctx->error_message = "Network Error: Failed to send register body.\n";
    fatal_error(ctx);
  }
}

static void recv_account_creation_response(client_context *ctx) {
  big_header_t hdr;

  ssize_t recvd = recv_all(ctx->active_sock_fd, &hdr, sizeof(hdr));

  if (recvd <= 0) {
    ctx->error_message = "Server disconnected during registration.\n";
    fatal_error(ctx);
  }

  if (recvd != sizeof(hdr)) {
    ctx->error_message = "Incomplete register response.\n";
    fatal_error(ctx);
  }

  if (hdr.type != TYPE_ACCOUNT_CREATE_RESPONSE) {
    ctx->error_message = "Protocol Error: Unexpected response type.\n";
    fatal_error(ctx);
  }

  // check status byte - any non-zero status is fatal (RFC Section 4.3)
  // status byte enum in prtocol.h
  if (hdr.status != STATUS_OK) {
    // fprintf(stderr, "Server Error Code: 0x%02X\n", hdr.status);
    if (hdr.status == STATUS_ALREADY_EXISTS) {
      ctx->error_message = "Registration Failed: Username already exists.\n";
      fatal_error(ctx);
    } else if (hdr.status == STATUS_INVALID_CREDENTIALS) {
      ctx->error_message = "Registration Failed: Invalid credentials.\n";
      fatal_error(ctx);
    } else if (hdr.status == STATUS_NOT_FOUND) {
      ctx->error_message = "Registration Failed: Resource not found.\n";
      fatal_error(ctx);
    } else if (hdr.status == STATUS_INTERNAL_ERROR) {
      ctx->error_message = "Registration Failed: Server internal error.\n";
      fatal_error(ctx);
    } else {
      ctx->error_message = "Registration Failed: Unknown server error.\n";
      fatal_error(ctx);
    }
  }

  // consume any body if the server sent one (protocol says response might have
  // body) *** READY FOR REMOVAL @DSG

  // parse response body to get assigned account ID
  uint32_t bsize = ntohl(hdr.body);
  if (bsize == sizeof(big_create_account_req_t)) {
    big_create_account_req_t resp_body;

    ssize_t recvd_body =
        recv_all(ctx->active_sock_fd, &resp_body, sizeof(resp_body));

    if (recvd_body != (ssize_t)sizeof(resp_body)) {
      ctx->error_message = "Failed to read registration response body.\n";
      fatal_error(ctx);
    }

    ctx->account_id = resp_body.client_id;
    // printf("Assigned account ID: %u\n", ctx->account_id);

  } else if (bsize > 0) {
    char *junk = malloc(bsize);

    if (junk == NULL) {
      ctx->error_message = "Fatal: Out of memory.\n";
      fatal_error(ctx);
      return;
    }

    // Cast bsize to size_t to match the function signature exactly
    ssize_t recvd_body = recv_all(ctx->active_sock_fd, junk, (size_t)bsize);

    if (recvd_body != (ssize_t)bsize) {
      free(junk);
      ctx->error_message = "Failed to read response body.\n";
      fatal_error(ctx);
      return;
    }
    free(junk);
  }
}

// get actual client IP from the connected socket
static void send_login_logout_request(client_context *ctx,
                                      uint8_t status_flag) {
  big_login_logout_req_t body = {0};

  strncpy(body.authentication.username, ctx->username, USERNAME_LENGTH);
  strncpy(body.authentication.password, ctx->password, PASSWORD_LENGTH);
  body.status = status_flag;

  // strncpy(body.password, ctx->password,
  //         sizeof(body.password)); // fix this retard @DSG
  // body.account_id = ctx->account_id;
  // body.status = status_flag;

  struct sockaddr_in local_addr;
  socklen_t addr_len = sizeof(local_addr);
  if (getsockname(ctx->active_sock_fd, (struct sockaddr *)&local_addr,
                  &addr_len) == -1) {
    ctx->error_message = "Failed to get local socket address.\n";
    fatal_error(ctx);
  }

  memcpy(&body.client_ip, &local_addr.sin_addr.s_addr, sizeof(ipv4_address_t));

  // body.client_ip = local_addr.sin_addr.s_addr; // already network byte order

  big_header_t req = {.version = BIG_CHAT_VERSION,
                      .type = TYPE_LOGIN_OR_LOGOUT_REQUEST,
                      .status = 0,
                      .reserved = 0,
                      .body = htonl(sizeof(body))};

  // send header
  if (send_all(ctx->active_sock_fd, &req, sizeof(req)) != sizeof(req)) {
    ctx->error_message = "Network Error: Failed to send login header.\n";
    fatal_error(ctx);
  }

  // send body
  if (send_all(ctx->active_sock_fd, &body, sizeof(body)) != sizeof(body)) {
    ctx->error_message = "Network Error: Failed to send login body.\n";
    fatal_error(ctx);
  }
}

// channel_list_choice_t network_execute_channel_phase(client_context *ctx) {
//   channel_list_choice_t choice;

//   if (convert_address(ctx) != 0) {
//     ctx->error_message = "Invalid Server IP format.\n";
//     fatal_error(ctx);
//   }
//   socket_create(ctx);
//   socket_connect(ctx, ctx->manager_port);
//   network_execute_channel_list(ctx);
//   close(ctx->active_sock_fd);
//   ctx->active_sock_fd = -1;

//   if (ctx->channel_count == 0) {
//     ctx->error_message = "No channels available on this server.\n";
//     fatal_error(ctx);
//   }

//   choice = ui_screen_channel_list(ctx);

//   if (choice.action == CHANNEL_LIST_LOGOUT) {
//     return choice; /* skip join entirely */
//   }

//   if (convert_address(ctx) != 0) {
//     ctx->error_message = "Invalid Server IP format.\n";
//     fatal_error(ctx);
//   }
//   socket_create(ctx);
//   socket_connect(ctx, ctx->manager_port);
//   network_execute_channel_join(ctx, choice.channel_id);

//   return choice;
// }

channel_list_choice_t network_execute_channel_phase(client_context *ctx) {
  channel_list_choice_t choice;

  // fprintf(stderr, "DEBUG: sock_fd before channel list: %d\n",
  // ctx->active_sock_fd);
  // Reuse the socket already open from login
  network_execute_channel_list(ctx);

  if (ctx->channel_count == 0) {
    ctx->error_message = "No channels available on this server.\n";
    fatal_error(ctx);
  }

  choice = ui_screen_channel_list(ctx);

  if (choice.action == CHANNEL_LIST_LOGOUT) {
    return choice;
  }

  // Still on the same socket — just join directly
  network_execute_channel_join(ctx, choice.channel_id);
  // fprintf(stderr, "DEBUG: sock_fd after channel join: %d\n",
  // ctx->active_sock_fd);

  return choice;
}

// any non-zero status is fatal (ok=0x00, senderError=0x10, receiverError=0x20)
static void recv_login_logout_response(client_context *ctx) {
  big_header_t hdr;

  ssize_t recvd = recv_all(ctx->active_sock_fd, &hdr, sizeof(hdr));

  if (recvd <= 0) {
    ctx->error_message = "Server disconnected during login.\n";
    fatal_error(ctx);
  }

  if (recvd != sizeof(hdr)) {
    ctx->error_message = "Incomplete login response.\n";
    fatal_error(ctx);
  }

  if (hdr.type != TYPE_LOGIN_OR_LOGOUT_RESPONSE) {
    ctx->error_message = "Protocol Error: Unexpected response type.\n";
    fatal_error(ctx);
  }

  if (hdr.status != STATUS_OK) {
    // fprintf(stderr, "Server Error Code: 0x%02X\n", hdr.status);
    ctx->error_message = "Login/Logout Failed: Server returned error.\n";
    fatal_error(ctx);
  }

  // drain any response body
  uint32_t bsize = ntohl(hdr.body);
  if (bsize > 0) {
    char *junk = malloc(bsize);

    if (junk == NULL) {
      ctx->error_message = "Fatal: Out of memory.\n";
      fatal_error(ctx);
      return;
    }

    ssize_t recvd_body = recv_all(ctx->active_sock_fd, junk, (size_t)bsize);

    if (recvd_body != (ssize_t)bsize) {
      free(junk);
      ctx->error_message = "Failed to read login response body.\n";
      fatal_error(ctx);
      return;
    }
    free(junk);
  }
}

// Ensures full buffer is received by looping until all bytes are read
static ssize_t recv_all(int sock, void *buffer, size_t length) {
  size_t total = 0;
  char *buf = (char *)buffer;

  while (total < length) {
    ssize_t n = recv(sock, buf + total, length - total, 0);

    if (n <= 0) {
      return n; // error or disconnect
    }

    total += n;
  }

  return (ssize_t)total;
}

// Ensures full buffer is sent by handling partial send calls
static ssize_t send_all(int sock, const void *buffer, size_t length) {
  size_t total = 0;
  const char *buf = (const char *)buffer;

  while (total < length) {
    ssize_t n = send(sock, buf + total, length - total, 0);

    if (n <= 0) {
      return n;
    }

    total += n;
  }

  return (ssize_t)total;
}
