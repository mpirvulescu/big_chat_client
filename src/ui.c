/*
 * ui.c — ncurses frontend for BIG Chat client
 *
 * Compile: gcc ... ui.c -lncurses
 *
 * Design rules enforced by the project build flags:
 *   - Every static function is forward-declared before its definition.
 *   - No variable-length arrays.
 *   - No implicit narrowing: every int->uint8_t, int->char conversion is
 *     an explicit cast.
 *   - No sign/unsigned mixing in comparisons: loop variables over uint8_t
 *     counts cast the count to int at the loop boundary.
 *   - No aggregate (struct) returns.
 *   - All format strings passed to mvwprintw/wprintw are string literals.
 *     When printing a runtime string use wprintw(win, "%s", str).
 *   - ui_set_status carries __attribute__((format)) so callers are checked.
 *   - No shadowed variable names across any scope boundary.
 */

#include "ui.h"
#include "client.h"
#include "messaging.h"
#include <curses.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Compile-time constants
 * ---------------------------------------------------------------------- */

enum {
  COLOR_PAIR_TITLE = 1,    /* white on blue   — title bar               */
  COLOR_PAIR_STATUS = 2,   /* black on white  — status bar              */
  COLOR_PAIR_SELECTED = 3, /* black on cyan   — highlighted channel row */
  COLOR_PAIR_MINE = 4,     /* cyan on default — your own messages       */
  COLOR_PAIR_THEIRS = 5,   /* white on default— incoming messages       */
  COLOR_PAIR_BORDER = 6,   /* blue on default — window borders          */
  COLOR_PAIR_LABEL = 7,    /* yellow on default — field labels          */
  COLOR_PAIR_ERROR = 8     /* red on default  — error / warning text    */
};

enum {
  INPUT_MAX = 1020,     /* max bytes the user may type in one go    */
  HISTORY_MAX = 512,    /* ring-buffer depth for received messages  */
  POLL_TIMEOUT_MS = 100 /* socket poll interval while in chat       */
};

enum {
  ESC_DELAY_MS = 25,
  ASCII_ESC = 27,
  ASCII_SPACE = 32,
  ASCII_DEL = 127,
  TITLE_BUF_SIZE = 64,
  ERRMSG_BUF_SIZE = 80,
  TITLE_RIGHT_BUF_SIZE = 48,
  DEFAULT_LIST_WIDTH = 40,
  PREFIX_RESERVE = 10
};

enum {
  CONFIRM_BOX_H = 5,
  CONFIRM_BOX_W = 36,
  CONFIRM_BTN_COL = 10,
};

/* -------------------------------------------------------------------------
 * Chat-history ring buffer
 * ---------------------------------------------------------------------- */

typedef struct {
  int is_mine; /* 1 = sent by us, 0 = received         */
  char sender_name[USERNAME_LENGTH];
  char text[INPUT_MAX + 1];
} chat_line_t;

static chat_line_t s_history[HISTORY_MAX];                           // NOLINT
static int s_history_total = 0; /* total ever added (unbounded)   */ // NOLINT
static int s_scroll_offset = 0; /* 0 = bottom; >0 = scrolled up   */ // NOLINT

/* -------------------------------------------------------------------------
 * Persistent global windows
 * ---------------------------------------------------------------------- */

static WINDOW *s_title_win = NULL;  // NOLINT
static WINDOW *s_status_win = NULL; // NOLINT

/* -------------------------------------------------------------------------
 * Forward declarations for all static helpers
 * ---------------------------------------------------------------------- */

static void history_push(const char *sender_name, const char *text,
                         int is_mine);
static void history_clear(void);
static void draw_title(const char *left, const char *right);
static WINDOW *make_box(int height, int width, const char *title);
static void draw_history(WINDOW *msg_win, int msg_h, int msg_w);
static void on_incoming_message(const char *sender_name, const char *text,
                                void *userdata);
static void chat_poll_socket(client_context *ctx);

/* -------------------------------------------------------------------------
 * History helpers
 * ---------------------------------------------------------------------- */

static void history_push(const char *sender_name, const char *text,
                         int is_mine) {
  int idx = s_history_total % HISTORY_MAX;

  if (sender_name != NULL) {
    strncpy(s_history[idx].sender_name, sender_name,
            (size_t)(USERNAME_LENGTH - 1));
    s_history[idx].sender_name[USERNAME_LENGTH - 1] = '\0';
  } else {
    s_history[idx].sender_name[0] = '\0';
  }

  s_history[idx].is_mine = is_mine;
  strncpy(s_history[idx].text, text, INPUT_MAX);
  s_history[idx].text[INPUT_MAX] = '\0';
  s_history_total++;
  s_scroll_offset = 0; /* new message — snap back to bottom */
}

static void history_clear(void) {
  memset(s_history, 0, sizeof(s_history));
  s_history_total = 0;
  s_scroll_offset = 0;
}

/* -------------------------------------------------------------------------
 * ui_init / ui_teardown
 * ---------------------------------------------------------------------- */

void ui_init(void) {
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(1);
  set_escdelay(ESC_DELAY_MS);

  if (has_colors()) {
    start_color();
    use_default_colors();
    init_pair(COLOR_PAIR_TITLE, COLOR_WHITE, COLOR_BLUE);
    init_pair(COLOR_PAIR_STATUS, COLOR_BLACK, COLOR_WHITE);
    init_pair(COLOR_PAIR_SELECTED, COLOR_BLACK, COLOR_CYAN);
    init_pair(COLOR_PAIR_MINE, COLOR_CYAN, -1);
    init_pair(COLOR_PAIR_THEIRS, COLOR_WHITE, -1);
    init_pair(COLOR_PAIR_BORDER, COLOR_BLUE, -1);
    init_pair(COLOR_PAIR_LABEL, COLOR_YELLOW, -1);
    init_pair(COLOR_PAIR_ERROR, COLOR_RED, -1);
  }

  s_title_win = newwin(1, COLS, 0, 0);
  s_status_win = newwin(1, COLS, LINES - 1, 0);
}

void ui_teardown(void) {
  if (s_title_win) {
    delwin(s_title_win);
    s_title_win = NULL;
  }
  if (s_status_win) {
    delwin(s_status_win);
    s_status_win = NULL;
  }
  endwin();
}

/* -------------------------------------------------------------------------
 * ui_fatal
 * ---------------------------------------------------------------------- */

void ui_fatal(const char *msg) {
  ui_teardown();
  (void)fputs(msg, stderr);
  exit(EXIT_FAILURE);
}

/* -------------------------------------------------------------------------
 * ui_set_status
 * ---------------------------------------------------------------------- */

void ui_set_status(const char *fmt, ...) {
  va_list ap;

  werase(s_status_win);
  wbkgd(s_status_win, (chtype)COLOR_PAIR(COLOR_PAIR_STATUS));
  wattron(s_status_win, COLOR_PAIR(COLOR_PAIR_STATUS));
  wmove(s_status_win, 0, 1);

  va_start(ap, fmt);
  vw_printw(s_status_win, fmt, ap);
  va_end(ap);

  wattroff(s_status_win, COLOR_PAIR(COLOR_PAIR_STATUS));
  wnoutrefresh(s_status_win);
  doupdate();
}

/* -------------------------------------------------------------------------
 * Internal: draw_title
 * ---------------------------------------------------------------------- */

static void draw_title(const char *left, const char *right) {
  werase(s_title_win);
  wbkgd(s_title_win, (chtype)COLOR_PAIR(COLOR_PAIR_TITLE));
  wattron(s_title_win, COLOR_PAIR(COLOR_PAIR_TITLE) | A_BOLD);

  mvwprintw(s_title_win, 0, 1, "%s", left);

  if (right != NULL && right[0] != '\0') {
    int right_col = COLS - (int)strlen(right) - 1;
    if (right_col > 0) {
      mvwprintw(s_title_win, 0, right_col, "%s", right);
    }
  }

  wattroff(s_title_win, COLOR_PAIR(COLOR_PAIR_TITLE) | A_BOLD);
  wnoutrefresh(s_title_win);
}

/* -------------------------------------------------------------------------
 * Internal: make_box
 * Creates a bordered window centred on the screen.
 * Caller must delwin() the returned pointer.
 * ---------------------------------------------------------------------- */

static WINDOW *make_box(int height, int width, const char *title) {
  int starty = (LINES - height) / 2;
  int startx = (COLS - width) / 2;
  WINDOW *win = newwin(height, width, starty, startx);

  keypad(win, TRUE);

  wattron(win, COLOR_PAIR(COLOR_PAIR_BORDER));
  box(win, 0, 0);
  wattroff(win, COLOR_PAIR(COLOR_PAIR_BORDER));

  if (title != NULL && title[0] != '\0') {
    int tx = (width - (int)strlen(title) - 2) / 2;
    if (tx < 1) {
      tx = 1;
    }
    wattron(win, A_BOLD | COLOR_PAIR(COLOR_PAIR_TITLE));
    mvwprintw(win, 0, tx, " %s ", title);
    wattroff(win, A_BOLD | COLOR_PAIR(COLOR_PAIR_TITLE));
  }

  return win;
}

/* =========================================================================
 * SCREEN: credentials (shared by Register and Login)
 * ====================================================================== */

credentials_result_t ui_screen_credentials(client_context *ctx,
                                           const char *phase) {
  /* Field geometry inside the box */
  const int BOX_H = 10;
  const int BOX_W = 46;
  const int LABEL_COL = 3;
  const int FIELD_COL = 15;
  const int UROW = 3;
  const int PROW = 5;
  const int ERR_ROW = 8;

  char title_left[TITLE_BUF_SIZE];
  char errmsg[ERRMSG_BUF_SIZE];
  char uname[USERNAME_LENGTH];
  char pword[PASSWORD_LENGTH];
  int active; /* 0 = username field, 1 = password field */
  int ulen;
  int plen;
  int field_w;
  WINDOW *win;

  snprintf(title_left, sizeof(title_left), "BIG Chat  \xe2\x80\x94  %s", phase);
  draw_title(title_left, "Tab: next field   Enter: submit");
  ui_set_status("Enter your credentials and press Enter to %s.  Already have "
                "an account? Press F2 to skip!",
                phase);

  memset(uname, 0, sizeof(uname));
  memset(pword, 0, sizeof(pword));
  memset(errmsg, 0, sizeof(errmsg));
  active = 0;
  ulen = 0;
  plen = 0;

  win = make_box(BOX_H, BOX_W, phase);
  field_w = BOX_W - FIELD_COL - 3;

  for (;;) {
    int ch;
    int uhl = (active == 0) ? A_REVERSE : 0;
    int phl = (active == 1) ? A_REVERSE : 0;

    /* --- username label & field --- */
    wattron(win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);
    mvwprintw(win, UROW, LABEL_COL, "Username:");
    wattroff(win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);

    wattron(win, uhl != 0 ? A_REVERSE : A_NORMAL);
    mvwhline(win, UROW, FIELD_COL, ' ', field_w);
    mvwprintw(win, UROW, FIELD_COL, "%.*s", field_w, uname);
    if (uhl == 0) {
      wattroff(win, A_NORMAL);
    } else {
      wattroff(win, A_REVERSE);
    }

    /* --- password label & field (masked) --- */
    wattron(win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);
    mvwprintw(win, PROW, LABEL_COL, "Password:");
    wattroff(win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);

    wattron(win, phl != 0 ? A_REVERSE : A_NORMAL);
    mvwhline(win, PROW, FIELD_COL, ' ', field_w);
    {
      int pi;
      for (pi = 0; pi < plen && pi < field_w; pi++) {
        mvwaddch(win, PROW, FIELD_COL + pi, (chtype)'*');
      }
    }
    if (phl == 0) {
      wattroff(win, A_NORMAL);
    } else {
      wattroff(win, A_REVERSE);
    }

    /* --- error message --- */
    if (errmsg[0] != '\0') {
      wattron(win, COLOR_PAIR(COLOR_PAIR_ERROR) | A_BOLD);
      mvwprintw(win, ERR_ROW, LABEL_COL, "%-*s", BOX_W - LABEL_COL - 2, errmsg);
      wattroff(win, COLOR_PAIR(COLOR_PAIR_ERROR) | A_BOLD);
    }

    /* --- cursor --- */
    if (active == 0) {
      int cx = ulen < field_w ? ulen : field_w - 1;
      wmove(win, UROW, FIELD_COL + cx);
    } else {
      int cx = plen < field_w ? plen : field_w - 1;
      wmove(win, PROW, FIELD_COL + cx);
    }

    wnoutrefresh(win);
    doupdate();

    ch = wgetch(win);

    if (ch == '\t' || ch == KEY_DOWN || ch == KEY_UP) {
      active ^= 1;
      errmsg[0] = '\0';

    } else if (ch == '\n' || ch == KEY_ENTER) {
      if (ulen == 0) {
        strncpy(errmsg, "Username cannot be empty.", sizeof(errmsg) - 1);
        active = 0;
      } else if (plen == 0) {
        strncpy(errmsg, "Password cannot be empty.", sizeof(errmsg) - 1);
        active = 1;
      } else {
        strncpy(ctx->username, uname, (size_t)(USERNAME_LENGTH - 1));
        ctx->username[USERNAME_LENGTH - 1] = '\0';
        strncpy(ctx->password, pword, (size_t)(PASSWORD_LENGTH - 1));
        ctx->password[PASSWORD_LENGTH - 1] = '\0';
        delwin(win);
        clear();
        refresh();
        return CREDENTIALS_SUBMIT;
      }

    } else if (ch == KEY_BACKSPACE || ch == ASCII_DEL || ch == '\b') {
      if (active == 0 && ulen > 0) {
        uname[--ulen] = '\0';
      } else if (active == 1 && plen > 0) {
        pword[--plen] = '\0';
      }
      errmsg[0] = '\0';

    } else if (ch == KEY_F(2)) {
      delwin(win);
      clear();
      refresh();
      return CREDENTIALS_SKIP;

    } else if (ch >= ASCII_SPACE && ch < ASCII_DEL) {
      if (active == 0 && ulen < USERNAME_LENGTH - 1) {
        uname[ulen] = (char)ch;
        ulen++;
        uname[ulen] = '\0';
      } else if (active == 1 && plen < PASSWORD_LENGTH - 1) {
        pword[plen] = (char)ch;
        plen++;
        pword[plen] = '\0';
      }
      errmsg[0] = '\0';
    }
  }

  return CREDENTIALS_SUBMIT;
}

/* =========================================================================
 * SCREEN: channel list
 * ====================================================================== */

channel_list_choice_t ui_screen_channel_list(client_context *ctx) {
  char title_right[TITLE_RIGHT_BUF_SIZE];
  WINDOW *win;
  int selected;
  int count;
  int list_h;
  int list_w;
  int visible;
  int scroll;

  snprintf(title_right, sizeof(title_right), "User: %s", ctx->username);
  draw_title("BIG Chat  -  Channels", title_right);
  ui_set_status("Up/Down: navigate   Enter: join channel   Q: logout");

  list_h = LINES - 2;
  list_w = DEFAULT_LIST_WIDTH;
  if (list_w > COLS) {
    list_w = COLS;
  }

  win = make_box(list_h, list_w, "Available Channels");
  selected = 0;
  count = (int)ctx->channel_count;
  visible = list_h - 4;
  if (visible < 1) {
    visible = 1;
  }
  scroll = 0;

  for (;;) {
    int row;
    int ch;

    if (selected < 0) {
      selected = 0;
    }
    if (count > 0 && selected >= count) {
      selected = count - 1;
    }
    if (selected < scroll) {
      scroll = selected;
    }
    if (selected >= scroll + visible) {
      scroll = selected - visible + 1;
    }

    for (row = 2; row < list_h - 1; row++) {
      mvwhline(win, row, 1, ' ', list_w - 2);
    }

    if (count == 0) {
      wattron(win, COLOR_PAIR(COLOR_PAIR_ERROR) | A_BOLD);
      mvwprintw(win, 2, 2, "No channels available on this server.");
      wattroff(win, COLOR_PAIR(COLOR_PAIR_ERROR) | A_BOLD);
    } else {
      int vi;
      for (vi = 0; vi < visible && (scroll + vi) < count; vi++) {
        int ch_idx = scroll + vi;
        uint8_t cid = ctx->channel_ids[ch_idx];
        int is_sel = (ch_idx == selected);
        row = 2 + vi;
        if (is_sel) {
          wattron(win, COLOR_PAIR(COLOR_PAIR_SELECTED) | A_BOLD);
          mvwhline(win, row, 1, ' ', list_w - 2);
        }
        mvwprintw(win, row, 3, "%s Channel %3u", is_sel ? ">" : " ",
                  (unsigned int)cid);
        if (is_sel) {
          wattroff(win, COLOR_PAIR(COLOR_PAIR_SELECTED) | A_BOLD);
        }
      }
    }

    if (scroll > 0) {
      mvwaddch(win, 1, list_w / 2, ACS_UARROW);
    }
    if (scroll + visible < count) {
      mvwaddch(win, list_h - 2, list_w / 2, ACS_DARROW);
    }

    wnoutrefresh(win);
    doupdate();

    ch = wgetch(win);

    if (ch == KEY_UP) {
      selected--;
    } else if (ch == KEY_DOWN) {
      selected++;
    } else if (ch == 'q' || ch == 'Q') {
      while (1) {
        WINDOW *confirm =
            make_box(CONFIRM_BOX_H, CONFIRM_BOX_W, "Confirm Logout");
        mvwprintw(confirm, 2, 3, "Are you sure you want to logout?");
        mvwprintw(confirm, 3, CONFIRM_BTN_COL, "[Y] Yes        [N] No");
        wnoutrefresh(confirm);
        doupdate();
        int ans = wgetch(confirm);
        delwin(confirm);
        clear();
        refresh();
        if (ans == 'y' || ans == 'Y') {
          channel_list_choice_t logout_result;
          logout_result.action = CHANNEL_LIST_LOGOUT;
          logout_result.channel_id = 0;
          delwin(win);
          clear();
          refresh();
          return logout_result;
        }
        break;
      }
      touchwin(win);
      wnoutrefresh(win);
      doupdate();
    } else if (ch == '\n' || ch == KEY_ENTER) {
      if (count > 0 && selected >= 0) {
        break;
      }
    }
  } /* end for(;;) */

  {
    channel_list_choice_t result;
    result.action = CHANNEL_LIST_JOIN;
    if (count > 0 && selected >= 0) {
      result.channel_id = ctx->channel_ids[selected];
    } else {
      result.channel_id = 0;
    }
    delwin(win);
    clear();
    refresh();
    return result;
  }
}

/* =========================================================================
 * SCREEN: chat
 * ====================================================================== */

/*
 * Callback handed to network_receive_pending().
 * Pushes an incoming message into the local history ring buffer.
 */
static void on_incoming_message(const char *sender_name, const char *text,
                                void *userdata) {
  const client_context *ctx = (client_context *)userdata;
  int is_mine = (strncmp(sender_name, ctx->username, USERNAME_LENGTH) == 0);
  history_push(sender_name, text, is_mine);
}

/*
 * draw_history — redraws the message history inside msg_win.
 */
static void draw_history(WINDOW *msg_win, int msg_h, int msg_w) {
  int visible;
  int total;
  int first;
  int last;
  int draw_row;
  int i;

  werase(msg_win);
  wattron(msg_win, COLOR_PAIR(COLOR_PAIR_BORDER));
  box(msg_win, 0, 0);
  wattroff(msg_win, COLOR_PAIR(COLOR_PAIR_BORDER));

  visible = msg_h - 2;
  if (visible < 1) {
    wnoutrefresh(msg_win);
    return;
  }

  total = s_history_total < HISTORY_MAX ? s_history_total : HISTORY_MAX;

  first = total - visible - s_scroll_offset;
  if (first < 0) {
    first = 0;
  }
  last = first + visible;
  if (last > total) {
    last = total;
  }

  draw_row = 1;
  for (i = first; i < last; i++) {
    int idx = i % HISTORY_MAX;
    chat_line_t *line = &s_history[idx];
    int text_w = msg_w - PREFIX_RESERVE; /* space for "[XXX]: " prefix */
    if (text_w < 1) {
      text_w = 1;
    }

    if (line->is_mine) {
      wattron(msg_win, COLOR_PAIR(COLOR_PAIR_MINE) | A_BOLD);
      mvwprintw(msg_win, draw_row, 1, "[You]: ");
      wattroff(msg_win, A_BOLD);
      wprintw(msg_win, "%.*s", text_w, line->text);
      wattroff(msg_win, COLOR_PAIR(COLOR_PAIR_MINE));
    } else {
      wattron(msg_win, COLOR_PAIR(COLOR_PAIR_THEIRS) | A_BOLD);
      mvwprintw(msg_win, draw_row, 1, "[%3s]: ", line->sender_name);
      wattroff(msg_win, A_BOLD);
      wattron(msg_win, COLOR_PAIR(COLOR_PAIR_THEIRS));
      wprintw(msg_win, "%.*s", text_w, line->text);
      wattroff(msg_win, COLOR_PAIR(COLOR_PAIR_THEIRS));
    }
    draw_row++;
  }

  /* scroll hint */
  if (s_scroll_offset > 0) {
    wattron(msg_win, COLOR_PAIR(COLOR_PAIR_LABEL));
    mvwprintw(msg_win, msg_h - 1, 2, " +%d newer ", s_scroll_offset);
    wattroff(msg_win, COLOR_PAIR(COLOR_PAIR_LABEL));
  }

  wnoutrefresh(msg_win);
}

/*
 * chat_poll_socket — drains all pending inbound packets without blocking.
 */
static void chat_poll_socket(client_context *ctx) {
  int result;
  do {
    result = network_receive_pending(ctx, on_incoming_message, ctx);
  } while (result > 0);
}

/*
 * ui_screen_chat — main chat loop.
 */
chat_exit_reason_t ui_screen_chat(client_context *ctx) {
  char title_right[TITLE_BUF_SIZE];
  char input[INPUT_MAX + 1];
  int input_len;
  int inp_scroll; /* horizontal scroll offset in the input field */
  int msg_h;
  int inp_y;
  WINDOW *msg_win;
  WINDOW *inp_win;

  snprintf(title_right, sizeof(title_right), "User: %s   Channel: %u",
           ctx->username, (unsigned int)ctx->current_channel_id);
  draw_title("BIG Chat  -  Messaging", title_right);
  ui_set_status("Enter: send   PgUp/PgDn: scroll   Esc: channels   Q: logout");

  /*
   * Layout (rows):
   * 0            title bar
   * 1..LINES-4   message history
   * LINES-3      input box (border + text line + border = 3 rows)
   * LINES-1      status bar
   */
  msg_h = LINES - 4;
  if (msg_h < 3) {
    msg_h = 3;
  }
  inp_y = LINES - 3;

  msg_win = newwin(msg_h, COLS, 1, 0);
  inp_win = newwin(3, COLS, inp_y, 0);
  keypad(inp_win, TRUE);
  wtimeout(inp_win, POLL_TIMEOUT_MS); /* non-blocking getch with timeout */

  memset(input, 0, sizeof(input));
  input_len = 0;
  inp_scroll = 0;
  s_scroll_offset = 0;
  history_clear();

  while (ctx->state == STATE_MESSAGING) {
    int field_w;
    int cur_col;
    int ch;

    /* --- redraw history panel --- */
    draw_history(msg_win, msg_h, COLS);

    /* --- redraw input box --- */
    werase(inp_win);
    wattron(inp_win, COLOR_PAIR(COLOR_PAIR_BORDER));
    box(inp_win, 0, 0);
    wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_BORDER));

    wattron(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);
    mvwprintw(inp_win, 0, 2, " Message ");
    wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);

    field_w = COLS - 4;
    if (field_w < 1) {
      field_w = 1;
    }

    /* keep cursor in view horizontally */
    if (input_len - inp_scroll >= field_w) {
      inp_scroll = input_len - field_w + 1;
    }
    if (inp_scroll < 0) {
      inp_scroll = 0;
    }

    wattron(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL));
    mvwhline(inp_win, 1, 2, ' ', field_w);
    mvwprintw(inp_win, 1, 2, "%.*s", field_w, input + inp_scroll);
    wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL));

    cur_col = 2 + (input_len - inp_scroll);
    if (cur_col > COLS - 3) {
      cur_col = COLS - 3;
    }
    wmove(inp_win, 1, cur_col);

    wnoutrefresh(inp_win);
    doupdate();

    /* --- keyboard input (returns ERR after POLL_TIMEOUT_MS) --- */
    ch = wgetch(inp_win);

    if (ch == ERR) {
      /* timeout — poll the socket for incoming messages */
      chat_poll_socket(ctx);
      continue;
    }

    if (ch == ASCII_ESC) { /* ESC — leave channel */
      ctx->state = STATE_LOGGED_IN;
      // close(ctx->active_sock_fd);
      // ctx->active_sock_fd = -1;
      delwin(msg_win);
      delwin(inp_win);
      clear();
      refresh();
      return CHAT_EXIT_CHANNEL_LIST;
    }

    if (ch == KEY_PPAGE) { /* Page Up — scroll history up */
      int max_scroll = s_history_total - (msg_h - 2);
      if (max_scroll < 0) {
        max_scroll = 0;
      }
      s_scroll_offset += (msg_h - 2);
      if (s_scroll_offset > max_scroll) {
        s_scroll_offset = max_scroll;
      }
      continue;
    }

    if (ch == KEY_NPAGE) { /* Page Down — scroll history down */
      s_scroll_offset -= (msg_h - 2);
      if (s_scroll_offset < 0) {
        s_scroll_offset = 0;
      }
      continue;
    }

    if (ch == '\n' || ch == KEY_ENTER) {
      if (input_len == 0) {
        continue;
      }

      if (strcmp(input, "/quit") == 0) {
        ctx->state = STATE_LOGGED_IN;
        break;
      }

      network_send_message(ctx, input);
      // history_push(ctx->username, input, 1);

      memset(input, 0, sizeof(input));
      input_len = 0;
      inp_scroll = 0;

      /* drain anything that arrived while we were composing */
      chat_poll_socket(ctx);
      continue;
    }

    if (ch == KEY_BACKSPACE || ch == ASCII_DEL || ch == '\b') {
      if (input_len > 0) {
        input_len--;
        input[input_len] = '\0';
      }
      continue;
    }

    if (ch >= ASCII_SPACE && ch < ASCII_DEL && input_len < INPUT_MAX) {
      input[input_len] = (char)ch;
      input_len++;
      input[input_len] = '\0';
    }

    /* poll after every keypress so inbound messages don't lag */
    chat_poll_socket(ctx);
  }

  close(ctx->active_sock_fd);
  ctx->active_sock_fd = -1;

  delwin(msg_win);
  delwin(inp_win);
  clear();
  refresh();
  return CHAT_EXIT_LOGOUT;
}