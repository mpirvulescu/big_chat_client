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
    CHANNEL_NAME_LENGTH = 16
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

} client_context;

#endif /*CLIENT_H*/