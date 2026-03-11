#ifndef NETWORK_H
#define NETWORK_H

#include "client.h"
#include "protocol.h"
#include <stdint.h>


int convert_address(client_context *ctx);
void socket_create(client_context *ctx);
void socket_connect(client_context *ctx, uint16_t port);

void fill_authentication_credentials(client_context *ctx, big_auth_t *auth);

void network_execute_discovery(client_context *ctx);

void network_execute_account_creation(client_context *ctx);

void network_execute_login(client_context *ctx);

void network_execute_logout(client_context *ctx);

#endif /*NETWORK.H*/