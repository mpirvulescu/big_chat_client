#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "client.h"
#include <stdint.h>

// RFC Appendix A — all valid type bytes (Resource << 3 | Action << 1 | Direction)
// Cross-checked against ASN.1 MessageType and hexpat Type enum
typedef enum
{
    BIG_CHAT_VERSION = 0x02,

    // Server Resource (RFC 6.1)
    TYPE_SERVER_REGISTRATION_REQUEST   = 0x00,    // Server Create Request
    TYPE_SERVER_REGISTRATION_RESPONSE  = 0x01,    // Server Create Response
    TYPE_SERVER_HEALTH_CHECK_REQUEST   = 0x04,    // Server Update Request
    TYPE_SERVER_HEALTH_CHECK_RESPONSE  = 0x05,    // Server Update Response

    // ActivatedServer Resource (RFC 6.2)
    TYPE_SERVER_ACTIVATION_REQUEST     = 0x08,    // ActivatedServer Create Request
    TYPE_SERVER_ACTIVATION_RESPONSE    = 0x09,    // ActivatedServer Create Response
    TYPE_DISCOVERY_REQUEST             = 0x0A,    // ActivatedServer Read Request
    TYPE_DISCOVERY_RESPONSE            = 0x0B,    // ActivatedServer Read Response
    TYPE_SERVER_DEACTIVATION_REQUEST   = 0x0E,    // ActivatedServer Delete Request
    TYPE_SERVER_DEACTIVATION_RESPONSE  = 0x0F,    // ActivatedServer Delete Response

    // User Resource (RFC 6.3)
    TYPE_ACCOUNT_CREATE_REQUEST        = 0x10,    // User Create Request
    TYPE_ACCOUNT_CREATE_RESPONSE       = 0x11,    // User Create Response
    TYPE_GET_USER_INFO_REQUEST         = 0x12,    // User Read Request
    TYPE_GET_USER_INFO_RESPONSE        = 0x13,    // User Read Response
    TYPE_LOGIN_OR_LOGOUT_REQUEST       = 0x14,    // User Update Request
    TYPE_LOGIN_OR_LOGOUT_RESPONSE      = 0x15,    // User Update Response

    // Log Resource (RFC 6.4)
    TYPE_LOG_REQUEST                   = 0x18,    // Log Create Request
    TYPE_LOG_RESPONSE                  = 0x19,    // Log Create Response

    // Channel Resource (RFC 6.5)
    TYPE_GET_CHANNEL_INFO_REQUEST      = 0x22,    // Channel Read Request
    TYPE_GET_CHANNEL_INFO_RESPONSE     = 0x23,    // Channel Read Response

    // Channels Resource (RFC 6.6)
    TYPE_LIST_ALL_CHANNELS_REQUEST     = 0x2A,    // Channels Read Request
    TYPE_LIST_ALL_CHANNELS_RESPONSE    = 0x2B,    // Channels Read Response

    // Message Resource (RFC 6.7)
    TYPE_SEND_MESSAGE_REQUEST          = 0x30,    // Message Create Request
    TYPE_SEND_MESSAGE_RESPONSE         = 0x31,    // Message Create Response
    TYPE_GET_MESSAGE_REQUEST           = 0x32,    // Message Read Request
    TYPE_GET_MESSAGE_RESPONSE          = 0x33     // Message Read Response
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