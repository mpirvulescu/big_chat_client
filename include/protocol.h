#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

typedef enum 
{
    BIG_CHAT_VERSION = 0x01,
    TYPE_DISCOVERY_REQUEST       = 0x0A,
    TYPE_DISCOVERY_RESPONSE       = 0x0B,
    TYPE_LOGIN_OR_LOGOUT_REQ = 0x15
} big_chat_message_t;


// 8-byte fixed header
typedef struct  {
    uint8_t  version;   
    uint8_t  type;      
    uint8_t  status;    
    uint8_t  padding;   
    uint32_t body_size; 
} big_header_t;

//  body for discovery response (Type 0x0B)
typedef struct __attribute__((packed)) {
    uint32_t ip_address;
    uint8_t  server_id;  
} big_discovery_res_t;

#endif