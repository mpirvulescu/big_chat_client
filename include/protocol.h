#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "client.h"
#include <stdint.h>

typedef enum 
{
    BIG_CHAT_VERSION = 0x01,
    TYPE_DISCOVERY_REQUEST       = 0x0A,
    TYPE_DISCOVERY_RESPONSE       = 0x0B,
    TYPE_ACCOUNT_CREATE_REQUEST  = 0x10,
    TYPE_ACCOUNT_CREATE_RESPONSE  = 0x11,
    TYPE_LOGIN_REQUEST           = 0x14,
    TYPE_LOGIN_RESPONSE           = 0x15
} big_chat_message_t;


// 8-byte fixed header
typedef struct  {
    uint8_t  version;   
    uint8_t  type;      
    uint8_t  status;    
    uint8_t  padding;   
    uint32_t body_size; 
} big_header_t;

// body for discovery response (Type 0x0B)
typedef struct __attribute__((packed)) {
    uint32_t ip_address;
    uint8_t  server_id;  
} big_discovery_res_t;

//body for account creation (type 0x10)
typedef struct __attribute__((packed)) {
    char     username[USERNAME_LENGTH];
    char     password[PASSWORD_LENGTH];
    uint8_t  client_id;
    // uint8_t  status; // 0x01 for create?  // DG: I am removing this for now to match spec
} big_create_account_req_t;

//body for login/logout (type 0x14)
typedef struct __attribute__((packed)) {
    char     password[PASSWORD_LENGTH];  // 16 bytes
    uint8_t  account_id;                 // 1 byte
    uint8_t  status;                     // 1 byte (1=login, 0=logout)
    uint32_t client_ip;                  // 4 bytes (network byte order)
} big_login_logout_req_t;

#endif