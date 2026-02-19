#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "client.h"
#include <stdint.h>

typedef enum 
{
    BIG_CHAT_VERSION = 0x02,
    TYPE_DISCOVERY_REQUEST       = 0x0A,
    TYPE_DISCOVERY_RESPONSE       = 0x0B,
    TYPE_ACCOUNT_CREATE_REQUEST  = 0x10,
    TYPE_ACCOUNT_CREATE_RESPONSE  = 0x11,
    TYPE_LOGIN_OR_LOGOUT_REQUEST           = 0x14,
    TYPE_LOGIN_OR_LOGOUT_RESPONSE           = 0x15,
    TYPE_GET_CHANNEL_INFO_REQUEST = 0x22,
    TYPE_GET_CHANNEL_INFO_RESPONSE = 0x23,
    TYPE_LIST_ALL_CHANNELS_REQUEST = 0x2A,
    TYPE_LIST_ALL_CHANNELS_RESPONSE = 0x2B,
    TYPE_SEND_MESSAGE_REQUEST = 0x30,
    TYPE_SEND_MESSAGE_RESPONSE = 0x31, 
    TYPE_GET_MESSAGE_REQUEST = 0x32,
    TYPE_GET_MESSAGE_RESPONSE = 0x33
} big_chat_message_t;

// RFC Section 4.3 — Status codes
typedef enum
{
    STATUS_OK                  = 0x00,
    STATUS_INVALID_VERSION     = 0x40,
    STATUS_INVALID_TYPE        = 0x41,
    STATUS_INVALID_SIZE        = 0x42,
    STATUS_MALFORMED_REQUEST   = 0x43,
    STATUS_INVALID_CREDENTIALS = 0x44,
    STATUS_NOT_FOUND           = 0x45,
    STATUS_ALREADY_EXISTS      = 0x46,
    STATUS_NOT_REGISTERED      = 0x47,
    STATUS_FORBIDDEN           = 0x48,
    STATUS_NOT_CHANNEL_MEMBER  = 0x49,
    STATUS_INTERNAL_ERROR      = 0x80,
    STATUS_SERVICE_UNAVAILABLE = 0x81,
    STATUS_RESOURCE_EXHAUSTED  = 0x82,
    STATUS_MESSAGE_TOO_LARGE   = 0x83,
    STATUS_TIMEOUT             = 0x84
} big_status_code_t;


// 8-byte fixed header
typedef struct __attribute__((packed)){
    uint8_t  version;   
    uint8_t  type;      
    uint8_t  status;    
    uint8_t  reserved;   
    uint32_t body; 
} big_header_t;

typedef struct __attribute__((packed)) {
    uint8_t a, b, c, d;
} ipv4_address_t;

// body for discovery response (Type 0x0B)
typedef struct __attribute__((packed)) {
    ipv4_address_t ip_address;
    uint8_t  server_id;  
} big_discovery_res_t;

typedef struct __attribute__((packed)) {
    char username[USERNAME_LENGTH];
    char password[PASSWORD_LENGTH];
} big_auth_t;


//body for account creation (type 0x10)
typedef struct __attribute__((packed)) {
    big_auth_t authentication;
    uint8_t  client_id;
    // uint8_t  status; // 0x01 for create?  // DG: I am removing this for now to match spec
} big_create_account_req_t;

//body for login/logout (type 0x14)
typedef struct __attribute__((packed)) {
    big_auth_t authentication;
    ipv4_address_t client_ip;                  // 4 bytes (network byte order)
    uint8_t  status;                     // 1 byte (1=login, 0=logout)
} big_login_logout_req_t;

//body for channel info
typedef struct __attribute__((packed)) {
    big_auth_t authentication;
    char channel_name[CHANNEL_NAME_LENGTH];
    uint8_t channel_id;
    uint8_t user_id_length;
    uint8_t user_ids_array[];
} big_channel_info_t;

//body for channel lists
typedef struct __attribute__((packed)) {
    big_auth_t authentication;
    uint8_t channel_id_length;
    uint8_t channel_id_array[];
} big_channel_list_t;

typedef struct __attribute__((packed)) {
    big_auth_t authentication;
    uint64_t timestamp;
    uint16_t message_length;
    uint8_t channel_id;
    char message[];
} big_send_message_t;

typedef struct __attribute__((packed)) {
    big_auth_t authentication;
    uint64_t timestamp;
    uint16_t message_length;
    uint8_t channel_id;
    uint8_t sender_id;
    char message[];
} big_get_message_t;


#endif