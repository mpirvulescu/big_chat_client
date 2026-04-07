#ifndef UI_H
#define UI_H

typedef struct client_context client_context;

// #include "client.h"
#include <stdint.h>

typedef enum {
    CHAT_EXIT_LOGOUT,
    CHAT_EXIT_CHANNEL_LIST,
} chat_exit_reason_t;

typedef enum {
    CREDENTIALS_SUBMIT,
    CREDENTIALS_SKIP,
} credentials_result_t;

typedef enum {
    CHANNEL_LIST_JOIN,
    CHANNEL_LIST_LOGOUT,
    CHANNEL_LIST_DELETE
} channel_list_result_t;

typedef struct {
    channel_list_result_t action;
    uint8_t channel_id;
} channel_list_choice_t;
 
/*
 * ui_init  — call once before any other ui_ function.
 * ui_teardown — call once at exit (safe to call before ui_init).
 */
void    ui_init(void);
void    ui_teardown(void);
 
/*
 * ui_set_status — printf-style message written to the persistent status bar.
 * Safe to call from any phase while ncurses is active.
 */
void    ui_set_status(const char *fmt, ...)
            __attribute__((format(printf, 1, 2)));
 
/*
 * ui_fatal — tears down ncurses, prints msg to stderr, exits.
 * Use instead of fatal_error() once ui_init() has been called.
 */
void    ui_fatal(const char *msg)
            __attribute__((noreturn));
 
/*
 * ui_screen_credentials
 *   Draws a login/register form.
 *   'phase' appears in the title box ("Register" or "Login").
 *   Blocks until the user submits valid (non-empty) credentials.
 *   Writes directly into ctx->username and ctx->password.
 */
credentials_result_t     ui_screen_credentials(client_context *ctx, const char *phase);
 
/*
 * ui_screen_channel_list
 *   Draws the channel list populated from ctx->channel_ids / ctx->channel_count.
 *   Blocks until the user selects a channel with Enter.
 *   Returns the selected channel_id.
 */
channel_list_choice_t  ui_screen_channel_list(client_context *ctx);
 
/*
 * ui_screen_chat
 *   Runs the interactive messaging loop.
 *   Calls network_send_message() for outbound messages.
 *   Calls network_receive_pending() on a 100 ms timer to drain inbound data.
 *   Blocks until the user presses Escape or types /quit.
 *   Closes ctx->active_sock_fd before returning and sets ctx->state to
 *   STATE_LOGGED_IN.
 */
chat_exit_reason_t    ui_screen_chat(client_context *ctx);

int ui_confirm_delete_account(void);
 
#endif /* UI_H */