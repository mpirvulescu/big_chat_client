#include "client.h"
#include "channels.h"
#include "messaging.h"
#include "network_funcs.h"
#include "ui.h"
#include "utils.h"
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static client_context init_context(void);
static void parse_arguments(client_context *ctx);
static void handle_arguments(client_context *ctx);
static int run_discovery_phase(client_context *ctx);
static int run_account_creation_phase(client_context *ctx);
static int run_login_phase(client_context *ctx);
static int run_channel_phase(client_context *ctx);
/*bug when logging out, removing for now*/
// static int run_messaging_phase(client_context *ctx);
static int run_logout_phase(client_context *ctx);

int main(int argc, char **argv) {
  client_context ctx = init_context();
  ctx.argc = argc;
  ctx.argv = argv;

  parse_arguments(&ctx);
  handle_arguments(&ctx);

  ui_init();
  ui_set_status("Discovering server at %s:%u ...", ctx.manager_ip,
                ctx.manager_port);

  run_discovery_phase(&ctx);
  run_account_creation_phase(&ctx);
  run_login_phase(&ctx);
  run_channel_phase(&ctx);

  if (ctx.last_action == CHANNEL_LIST_DELETE) {
    network_execute_delete_account(&ctx);
    ui_teardown();
    quit(&ctx);
  }

  /*bug when logging out, removing for now*/
  // run_messaging_phase(&ctx);
  run_logout_phase(&ctx);

  ui_teardown();
  quit(&ctx);
  return EXIT_SUCCESS;
}

static client_context init_context(void) {
  client_context ctx = {0};
  ctx.argc = 0;
  ctx.argv = NULL;
  ctx.exit_code = EXIT_SUCCESS;
  ctx.state = STATE_DISCONNECTED;
  ctx.active_sock_fd = -1;
  ctx.manager_port = 0;

  return ctx;
}

// parse them boys
static void parse_arguments(client_context *ctx) {
  int opt;
  const char *optstring = ":m:p:h";
  opterr = 0;

  while ((opt = getopt(ctx->argc, ctx->argv, optstring)) != -1) {
    switch (opt) {
    case 'm':
      if (optarg) {
        snprintf(ctx->manager_ip, sizeof(ctx->manager_ip), "%s", optarg);
      }
      break;

    case 'p':
      if (optarg) {
        char *endptr;
        unsigned long port;
        errno = 0;
        port = strtoul(optarg, &endptr, PORT_BASE);
        if (errno != 0 || *endptr != '\0' || port > UINT16_MAX) {
          fprintf(stderr, "Error: Invalid port '%s'. Range: 1-65535.\n",
                  optarg);
          ctx->exit_code = EXIT_FAILURE;
          print_usage(ctx);
        }
        ctx->manager_port = (uint16_t)port;
      }
      break;

    case 'h':
      fprintf(stderr, "Usage: %s -m <manager_ip> -p <manager_port>\n",
              ctx->argv[0]);
      ctx->exit_code = EXIT_SUCCESS;
      print_usage(ctx);
      break;

    case ':':
      fprintf(stderr, "Error: Option '-%c' requires an argument.\n", optopt);
      ctx->exit_code = EXIT_FAILURE;
      print_usage(ctx);
      break;

    case '?':
      fprintf(stderr, "Error: Unknown option '-%c'.\n", optopt);
      ctx->exit_code = EXIT_FAILURE;
      print_usage(ctx);
      break;

    default:
      ctx->exit_code = EXIT_FAILURE;
      print_usage(ctx);
      break;
    }
  }
}

static void handle_arguments(client_context *ctx) {
  if (ctx->manager_ip[0] == '\0') {
    fprintf(stderr, "Error: Manager IP (-m) must be specified.\n");
    ctx->exit_code = EXIT_FAILURE;
    print_usage(ctx);
  }

  if (ctx->manager_port == 0) {
    fprintf(stderr, "Error: Manager Port (-p) must be specified.\n");
    ctx->exit_code = EXIT_FAILURE;
    print_usage(ctx);
  }
}

static int run_discovery_phase(client_context *ctx) {
  ctx->state = STATE_DISCOVERING;
  network_execute_discovery(ctx);
  return 0;
}

static int run_account_creation_phase(client_context *ctx) {
  ctx->state = STATE_CONNECTING_TO_SERVER;
  credentials_result_t result = ui_screen_credentials(ctx, "Register");
  if (result == CREDENTIALS_SUBMIT) {
    ui_set_status("Registering account...");
    network_execute_account_creation(ctx);
  }
  ctx->state = STATE_LOGGED_IN;
  return 0;
}

static int run_login_phase(client_context *ctx) {
  ctx->state = STATE_LOGGED_IN;
  memset(ctx->username, 0, sizeof(ctx->username));
  memset(ctx->password, 0, sizeof(ctx->password));
  ui_screen_credentials(ctx, "Login");
  ui_set_status("Logging in...");
  network_execute_login(ctx);
  ui_set_status("Fetching account details...");
  network_fetch_own_account_id(ctx);

  return 0;
}

static int run_channel_phase(client_context *ctx) {
  chat_exit_reason_t chat_reason = CHAT_EXIT_LOGOUT;
  do {
    channel_list_choice_t choice;
    ctx->state = STATE_LOGGED_IN;
    choice = network_execute_channel_phase(ctx);
    if (choice.action == CHANNEL_LIST_LOGOUT ||
        choice.action == CHANNEL_LIST_DELETE) {
      ctx->last_action = choice.action;
      break;
    }
    network_execute_channel_join(ctx, choice.channel_id);
    ctx->state = STATE_MESSAGING;
    chat_reason = ui_screen_chat(ctx);
  } while (chat_reason == CHAT_EXIT_CHANNEL_LIST);
  return 0;
}

/*bug when logging out, removing for now*/

// static int run_messaging_phase(client_context *ctx) {
//   ctx->state = STATE_MESSAGING;
//   /* ui_screen_chat blocks until ESC or /quit,
//      closes the socket, and sets state to STATE_LOGGED_IN. */
//   ui_screen_chat(ctx);
//   ctx->state = STATE_LOGGED_IN;
//   return 0;
// }

static int run_logout_phase(client_context *ctx) {
  ctx->state = STATE_EXITING;
  ui_set_status("Logging out...");
  network_execute_logout(ctx);
  return 0;
}
