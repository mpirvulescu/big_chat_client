#ifndef CHANNELS_H
#define CHANNELS_H

#include "client.h"
#include <stdint.h>

typedef struct {
    uint8_t channel_id;
} channel_t;

void network_execute_channel_list(client_context *ctx);

#endif /* CHANNELS_H*/