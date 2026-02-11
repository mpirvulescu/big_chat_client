#include "network_funcs.h"
#include "protocol.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>

static int send_discovery_request(int fd);
static int recv_discovery_response(int fd, big_discovery_res_t *dest);

// add the socket shit from networkfuncs.h
int network_execute_discovery(client_context *ctx) {}
int network_execute_login(client_context *ctx) {}

static int send_discovery_request(int fd) {}
static int recv_discovery_response(int fd, big_discovery_res_t *dest) {}
