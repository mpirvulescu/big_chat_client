#include "client.h"
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
static int run_authentication_phase(client_context *ctx);

int main(int argc, char **argv) {
  client_context ctx;
  ctx = init_context();

  ctx.argc = argc;
  ctx.argv = argv;

  parse_arguments(&ctx);
  handle_arguments(&ctx);

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
          exit(EXIT_FAILURE);
        }
        ctx->manager_port = (uint16_t)port;
      }
      break;
    case 'h':
      printf("Usage: %s -m <manager_ip> -p <manager_port>\n", ctx->argv[0]);
      exit(EXIT_SUCCESS);
    case ':':
      fprintf(stderr, "Error: Option '-%c' requires an argument.\n", optopt);
      exit(EXIT_FAILURE);
    case '?':
      fprintf(stderr, "Error: Unknown option '-%c'.\n", optopt);
      exit(EXIT_FAILURE);
    default:
      exit(EXIT_FAILURE);
    }
  }
}

static void handle_arguments(client_context *ctx) {
  if (ctx->manager_ip[0] == '\0') {
    fprintf(stderr, "Error: Manager IP (-m) must be specified.\n");
    exit(EXIT_FAILURE);
  }

  if (ctx->manager_port == 0) {
    fprintf(stderr, "Error: Manager Port (-p) must be specified.\n");
    exit(EXIT_FAILURE);
  }

  struct in_addr addr;
  if (inet_pton(AF_INET, ctx->manager_ip, &addr) != 1) {
    fprintf(stderr, "Error: '%s' is not a valid IPv4 address.\n",
            ctx->manager_ip);
    exit(EXIT_FAILURE);
  }

  printf("client is ready for discovery via %s:%u\n", ctx->manager_ip,
         ctx->manager_port);
}


