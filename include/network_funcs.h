#ifndef NETWORK_H
#define NETWORK_H

#include "client.h"
#include <stdint.h>

void convert_address(client_context *ctx);

int socket_create(client_context *ctx);

void socket_connect(client_context *ctx);

int network_execute_discovery(client_context *ctx);


#endif