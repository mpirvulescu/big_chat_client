#ifndef CHANNELS_H
#define CHANNELS_H

#include "client.h"
#include <stdint.h>

typedef struct
{
    uint8_t channel_id;
} channel_t;

// Channels Read workflow (RFC 6.6.1, type bytes 0x2A/0x2B)
// Pattern: network_execute_discovery() (network_funcs.c:82)
void network_execute_channel_list(client_context *ctx);

#endif /* CHANNELS_H */
