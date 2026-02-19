#ifndef CLIENT_H
#define CLIENT_H

#include <arpa/inet.h>
#include <stdint.h>

enum 
{
    PORT_BASE = 10
};

enum
{
    USERNAME_LENGTH = 16,
    PASSWORD_LENGTH = 16,
    CHANNEL_NAME_LENGTH = 16,
    MAX_CHANNEL_COUNT = 255    // RFC 6.6.1: ChannelIdLen is uint8_t (0..255), ASN.1 ChannelId ::= UInt8
};

typedef enum 
{
    STATE_DISCONNECTED,
    STATE_DISCOVERING,
    STATE_CONNECTING_TO_SERVER,
    STATE_AWAITING_USER_INFO,
    STATE_LOGGED_IN,
    STATE_MESSAGING,
    STATE_EXITING
} client_state;

typedef struct 
{
    int argc;
    char **argv;

    int exit_code;
    char *exit_message;

    client_state state; //keep track of where we're at
    int active_sock_fd; //the active socket (gonna switch from manager to chat)
    struct sockaddr_storage addr;

    char manager_ip[INET_ADDRSTRLEN];
    uint16_t manager_port;

    // user credentials
    char username[USERNAME_LENGTH];
    char password[PASSWORD_LENGTH];
    uint8_t account_id;

    // Server connection — persistent after login (RFC 7.1: multiple PDUs on one TCP connection)
    // Set after discovery (network_funcs.c:119), used for login→channel→messaging→logout
    char server_ip[INET_ADDRSTRLEN];
    uint16_t server_port;

    // Channel state — populated by Channels Read response (RFC 6.6.1)
    // ASN.1: ListAllChannels ::= SEQUENCE { auth Auth, channelIdLen UInt8, channelIds SEQUENCE OF ChannelId }
    uint8_t channel_ids[MAX_CHANNEL_COUNT];    // from ListAllChannels.channelIds
    uint8_t channel_count;                     // from ListAllChannels.channelIdLen
    uint8_t current_channel_id;                // user-selected channel for messaging

    // Messaging loop flag — controls poll() event loop (adapted from D'Arcy poll/main.c:80)
    // RFC 7.1: server sends unsolicited Message.Read.Response (0x33) requiring event loop
    int in_messaging_loop;

} client_context;

#endif /*CLIENT_H*/