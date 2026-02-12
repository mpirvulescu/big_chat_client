#include "client.h"
#include "network_funcs.h"
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
static int run_logout_phase(client_context *ctx);

int main(int argc, char **argv) {
  client_context ctx;
  ctx = init_context();

  ctx.argc = argc;
  ctx.argv = argv;

  parse_arguments(&ctx);
  handle_arguments(&ctx);

  // find the fucking server
  run_discovery_phase(&ctx);

  // create the damn account
  run_account_creation_phase(&ctx);

  // login and logout with the chat server
  run_login_phase(&ctx);
  run_logout_phase(&ctx);

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
    // for the manager ip
    case 'm':
      if (optarg) {
        snprintf(ctx->manager_ip, sizeof(ctx->manager_ip), "%s", optarg);
      }
      break;
    // for the manager port
    case 'p':
      if (optarg) {
        char *endptr;
        errno = 0;
        // parse the port
        unsigned long port = strtoul(optarg, &endptr, PORT_BASE);

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
      printf("Usage: %s -m <manager_ip> -p <manager_port>\n", ctx->argv[0]);
      ctx->exit_code = EXIT_SUCCESS;
      print_usage(ctx);
    case ':':
      fprintf(stderr, "Error: Option '-%c' requires an argument.\n", optopt);
      ctx->exit_code = EXIT_FAILURE;
      print_usage(ctx);
    case '?':
      fprintf(stderr, "Error: Unknown option '-%c'.\n", optopt);
      ctx->exit_code = EXIT_FAILURE;
      print_usage(ctx);
    default:
      ctx->exit_code = EXIT_FAILURE;
      print_usage(ctx);
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

  //   struct in_addr addr;
  //   if (inet_pton(AF_INET, ctx->manager_ip, &addr) != 1) {
  //     fprintf(stderr, "Error: '%s' is not a valid IPv4 address.\n",
  //             ctx->manager_ip);
  //     ctx->exit_code = EXIT_FAILURE;
  //     print_usage(ctx);
  //   }

  printf("client is ready for discovery via %s:%u\n", ctx->manager_ip,
         ctx->manager_port);
}

static int run_discovery_phase(client_context *ctx) {
  ctx->state = STATE_DISCOVERING;

  network_execute_discovery(ctx);

  return 0;
}

static int run_account_creation_phase(client_context *ctx) {
  ctx->state = STATE_CONNECTING_TO_SERVER;

  printf("\n[Account Creation]\n");
  // disable to prevent double account setup loop
  // get_user_input(ctx->username, sizeof(ctx->username), "Enter Username: ");
  // get_user_input(ctx->password, sizeof(ctx->password), "Enter Password: ");

  // username loop
  while (1) {
    get_user_input(ctx->username, sizeof(ctx->username), "Enter Username: ");
    if (strlen(ctx->username) > 0) {
      break; // input is valid
    }
    printf("Error: Username cannot be empty. Please try again.\n");
  }

  // password loop
  while (1) {
    get_user_input(ctx->password, sizeof(ctx->password), "Enter Password: ");
    if (strlen(ctx->password) > 0) {
      break; // input is valid
    }
    printf("Error: Password cannot be empty. Please try again.\n");
  }

  // call network layer to do the handshake
  network_execute_account_creation(ctx);

  ctx->state = STATE_LOGGED_IN;
  return 0;
}

static int run_login_phase(client_context *ctx) {
  ctx->state = STATE_LOGGED_IN;
  network_execute_login(ctx);
  return 0;
}

static int run_logout_phase(client_context *ctx) {
  ctx->state = STATE_EXITING;
  network_execute_logout(ctx);
  return 0;
}