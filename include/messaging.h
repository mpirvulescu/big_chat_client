#ifndef MESSAGING_H
#define MESSAGING_H

#include "client.h"
#include "protocol.h"

enum {
    MAX_MESSAGE = 1024
};

void network_execute_messaging_loop(client_context *ctx);

void network_send_message(client_context *ctx, const char *text);

int network_receive_pending(client_context *ctx,
                            void (*on_message)(const char *sender_name,
                                               const char *text,
                                               void *userdata),
                            void *userdata);

void lookup_username(client_context *ctx, uint8_t sender_id, char *out_name,
                     size_t out_size);

#endif /* MESSAGING_H*/

