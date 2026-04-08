#ifndef MESSAGING_H
#define MESSAGING_H

#include "client.h"
#include "protocol.h"
#include <stddef.h>

enum {
    MAX_MESSAGE = 1024,
    MS_PER_SEC = 1000ULL,
    NS_PER_MS = 1000000LL,
    TIMESTAMP_SIZE_BYTES = 8U
};

void network_execute_messaging_loop(client_context *ctx);

uint64_t network_send_message(client_context *ctx, const char *text);

int network_receive_pending(client_context *ctx,
                            void (*on_message)(const char *sender_name,
                                               const char *text,
                                               const void *userdata),
                            void *userdata);

void lookup_username(client_context *ctx, uint8_t sender_id, char *out_name,
                     size_t out_size);

void network_fetch_history(client_context *ctx,
                           void (*on_message)(const char *sender_name,
                                              const char *text,
                                              const void       *userdata,
                                              uint64_t    timestamp,
                                              uint8_t     sender_id),
                           void *userdata,
                           uint16_t limit);
 
int network_edit_message(client_context *ctx,
                         uint64_t original_timestamp,
                         const char *new_text);

int network_delete_message(client_context *ctx, uint64_t original_timestamp);

#endif /* MESSAGING_H*/

