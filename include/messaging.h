#ifndef MESSAGING_H
#define MESSAGING_H

#include "client.h"

// Messaging event loop (RFC 6.7.1 Send Message, RFC 6.7.2 Get Message, RFC 7.1 unsolicited responses)
// Pattern: adapted from D'Arcy poll/main.c:80 — poll() event loop with stdin + socket
void network_execute_messaging_loop(client_context *ctx);

#endif /* MESSAGING_H */
