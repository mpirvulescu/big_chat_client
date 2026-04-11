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
  INPUT_MAX = 1020,      /* max bytes the user may type in one go    */
  HISTORY_MAX = 512,     /* ring-buffer depth for received messages  */
  POLL_TIMEOUT_MS = 100, /* socket poll interval while in chat       */
  HISTORY_FETCH = 200    /* messages to request from server on join  */
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
  PREFIX_RESERVE = 14, /* space for "[username]: " prefix */
  ASCII_CTRL_AND_U = 21
};

enum {
  CONFIRM_BOX_H = 5,
  CONFIRM_BOX_W = 36,
  CONFIRM_BTN_COL = 10,
  SIDEBAR_WIDTH = 20,
  MIN_MSG_WIDTH = 20,
  USER_LIST_H_OFFSET = 2
};

/* -------------------------------------------------------------------------
 * Chat-history ring buffer
 * ---------------------------------------------------------------------- */

typedef struct {
  uint64_t timestamp;
  int is_mine;
  int is_deleted;
  uint8_t sender_id;
  char sender_name[USERNAME_LENGTH];
  char text[INPUT_MAX + 1];
} chat_line_t;

static chat_line_t s_history[HISTORY_MAX];                             // NOLINT
static int s_history_total = 0; /* total ever added (unbounded)    */  // NOLINT
static int s_scroll_offset = 0; /* 0 = bottom; >0 = scrolled up   */   // NOLINT
static int s_select_mode = 0; /* 0 = normal, 1 = select mode     */    // NOLINT
static int s_selected_index = 0; /* absolute index into ring buffer */ // NOLINT

/* -------------------------------------------------------------------------
 * Persistent global windows
 * ---------------------------------------------------------------------- */

static WINDOW *s_title_win = NULL;  // NOLINT
static WINDOW *s_status_win = NULL; // NOLINT

/* -------------------------------------------------------------------------
 * Forward declarations for all static helpers
 * ---------------------------------------------------------------------- */

static void history_push(const char *sender_name, const char *text, int is_mine,
                         uint64_t timestamp, uint8_t sender_id);
static void history_clear(void);
static void draw_title(const char *left, const char *right);
static WINDOW *make_box(int height, int width, const char *title);
static void draw_history(WINDOW *msg_win, int msg_h, int msg_w);
static void on_incoming_message(const char *sender_name, const char *text,
                                const void *userdata, uint64_t timestamp,
                                uint8_t sender_id);
static void on_history_message(const char *sender_name, const char *text,
                               const void *userdata, uint64_t timestamp,
                               uint8_t sender_id);
static void chat_poll_socket(client_context *ctx);
static int chat_edit_selected(client_context *ctx, WINDOW *inp_win, int msg_h);
static void chat_delete_selected(client_context *ctx);

/* -------------------------------------------------------------------------
 * History helpers
 * ---------------------------------------------------------------------- */

static void history_push(const char *sender_name, const char *text, int is_mine,
                         uint64_t timestamp, uint8_t sender_id) {
  int idx = s_history_total % HISTORY_MAX;

  if (sender_name != NULL) {
    strncpy(s_history[idx].sender_name, sender_name,
            (size_t)(USERNAME_LENGTH - 1));
    s_history[idx].sender_name[USERNAME_LENGTH - 1] = '\0';
  } else {
    s_history[idx].sender_name[0] = '\0';
  }

  s_history[idx].is_mine = is_mine;
  s_history[idx].is_deleted = 0;
  s_history[idx].timestamp = timestamp;
  s_history[idx].sender_id = sender_id;
  strncpy(s_history[idx].text, text, INPUT_MAX);
  s_history[idx].text[INPUT_MAX] = '\0';

  s_history_total++;
  s_scroll_offset = 0; /* new message — snap back to bottom */
}

static void history_clear(void) {
  memset(s_history, 0, sizeof(s_history));
  s_history_total = 0;
  s_scroll_offset = 0;
  s_select_mode = 0;
  s_selected_index = 0;
}

/*
 * Return the chat_line_t* for absolute index i (0 = oldest in ring).
 * Returns NULL if i is out of range.
 */
static chat_line_t *history_get(int i) {
  int total = s_history_total < HISTORY_MAX ? s_history_total : HISTORY_MAX;
  if (i < 0 || i >= total) {
    return NULL;
  }
  /* When total < HISTORY_MAX the ring has not wrapped; oldest is index 0.
     When total >= HISTORY_MAX the oldest slot is (s_history_total %
     HISTORY_MAX). */
  int base =
      (s_history_total >= HISTORY_MAX) ? (s_history_total % HISTORY_MAX) : 0;
  int idx = (base + i) % HISTORY_MAX;
  return &s_history[idx];
}

void ui_update_last_message_timestamp(uint64_t provisional_ts,
                                      uint64_t official_ts) {
  int total = s_history_total < HISTORY_MAX ? s_history_total : HISTORY_MAX;
  int i;

  (void)provisional_ts; /* reserved for future use */

  /* Walk backwards: find the newest is_mine entry and patch it */
  for (i = total - 1; i >= 0; i--) {
    chat_line_t *entry = history_get(i);
    if (entry && entry->is_mine && !entry->is_deleted) {
      entry->timestamp = official_ts;
      return;
    }
  }
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
  int active;
  int ulen;
  int plen;
  int field_w;
  WINDOW *win;

  snprintf(title_left, sizeof(title_left), "BIG Chat  —  %s", phase);
  draw_title(title_left, "Tab: next field   Enter: submit");
  ui_set_status("Enter your credentials and press Enter to %s.  "
                "Already have an account? Press F2 to skip!",
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

    if (errmsg[0] != '\0') {
      wattron(win, COLOR_PAIR(COLOR_PAIR_ERROR) | A_BOLD);
      mvwprintw(win, ERR_ROW, LABEL_COL, "%-*s", BOX_W - LABEL_COL - 2, errmsg);
      wattroff(win, COLOR_PAIR(COLOR_PAIR_ERROR) | A_BOLD);
    }

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
        uname[ulen++] = (char)ch;
        uname[ulen] = '\0';
      } else if (active == 1 && plen < PASSWORD_LENGTH - 1) {
        pword[plen++] = (char)ch;
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
  ui_set_status(
      "Up/Down: navigate   Enter: join   Q: logout   D: delete account");

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
    } else if (ch == 'd' || ch == 'D') {
      int confirm = ui_confirm_delete_account();
      if (confirm) {
        channel_list_choice_t delete_result;
        delete_result.action = CHANNEL_LIST_DELETE;
        delete_result.channel_id = 0;
        delwin(win);
        clear();
        refresh();
        return delete_result;
      }
      touchwin(win);
      wnoutrefresh(win);
      doupdate();
    } else if (ch == '\n' || ch == KEY_ENTER) {
      if (count > 0 && selected >= 0) {
        break;
      }
    }
  }

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

int ui_confirm_delete_account(void) {
  WINDOW *confirm = make_box(CONFIRM_BOX_H, CONFIRM_BOX_W, "Delete Account");
  mvwprintw(confirm, 2, 3, "Are you sure you want to delete?");
  mvwprintw(confirm, 3, CONFIRM_BTN_COL, "[Y] Yes        [N] No");
  wnoutrefresh(confirm);
  doupdate();

  for (;;) {
    int ch = wgetch(confirm);
    if (ch == 'y' || ch == 'Y') {
      delwin(confirm);
      clear();
      refresh();
      return 1;
    }
    if (ch == 'n' || ch == 'N') {
      delwin(confirm);
      clear();
      refresh();
      return 0;
    }
  }
}

/* =========================================================================
 * SCREEN: chat — helper callbacks
 * ====================================================================== */

/*
 * Callback for network_receive_pending — live incoming messages.
 * Timestamp is not known from this path; use 0.
 */
static void on_incoming_message(const char *sender_name, const char *text,
                                const void *userdata, uint64_t timestamp,
                                uint8_t sender_id) {
  const client_context *ctx = (const client_context *)userdata;
  int is_mine = (strncmp(sender_name, ctx->username, USERNAME_LENGTH) == 0);

  /* Save the OFFICIAL timestamp immediately */
  history_push(sender_name, text, is_mine, timestamp, sender_id);
}

/*
 * Callback for network_fetch_history — historical messages on join.
 */
static void on_history_message(const char *sender_name, const char *text,
                               const void *userdata, uint64_t timestamp,
                               uint8_t sender_id) {
  const client_context *ctx = (const client_context *)userdata;
  int is_mine = (strncmp(sender_name, ctx->username, USERNAME_LENGTH) == 0);
  history_push(sender_name, text, is_mine, timestamp, sender_id);
}

/* =========================================================================
 * SCREEN: chat — draw_history
 * ====================================================================== */

/*
 * Redraws the message history inside msg_win.
 * When s_select_mode is non-zero, the line at s_selected_index is
 * highlighted and own-message lines show an [E]dit/[D]elete hint.
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
    chat_line_t *line = history_get(i);
    if (!line) {
      draw_row++;
      continue;
    }

    int text_w = msg_w - PREFIX_RESERVE;
    if (text_w < 1) {
      text_w = 1;
    }

    int is_sel = (s_select_mode && i == s_selected_index);

    if (is_sel) {
      wattron(msg_win, A_REVERSE);
    }

    if (line->is_deleted) {
      wattron(msg_win, COLOR_PAIR(COLOR_PAIR_ERROR));
      mvwprintw(msg_win, draw_row, 1, "[deleted]%.*s", text_w, "");
      wattroff(msg_win, COLOR_PAIR(COLOR_PAIR_ERROR));
    } else if (line->is_mine) {
      wattron(msg_win, COLOR_PAIR(COLOR_PAIR_MINE) | A_BOLD);
      mvwprintw(msg_win, draw_row, 1, "[You]: ");
      wattroff(msg_win, A_BOLD);
      wprintw(msg_win, "%.*s", text_w, line->text);
      wattroff(msg_win, COLOR_PAIR(COLOR_PAIR_MINE));
    } else {
      wattron(msg_win, COLOR_PAIR(COLOR_PAIR_THEIRS) | A_BOLD);
      mvwprintw(msg_win, draw_row, 1, "[%.*s]: ", USERNAME_LENGTH - 1,
                line->sender_name);
      wattroff(msg_win, A_BOLD);
      wattron(msg_win, COLOR_PAIR(COLOR_PAIR_THEIRS));
      wprintw(msg_win, "%.*s", text_w, line->text);
      wattroff(msg_win, COLOR_PAIR(COLOR_PAIR_THEIRS));
    }

    if (is_sel) {
      wattroff(msg_win, A_REVERSE);
    }

    draw_row++;
  }

  /* Scroll hint */
  if (s_scroll_offset > 0) {
    wattron(msg_win, COLOR_PAIR(COLOR_PAIR_LABEL));
    mvwprintw(msg_win, msg_h - 1, 2, " +%d newer ", s_scroll_offset);
    wattroff(msg_win, COLOR_PAIR(COLOR_PAIR_LABEL));
  }

  wnoutrefresh(msg_win);
}

/* =========================================================================
 * SCREEN: chat — socket poll
 * ====================================================================== */

static void chat_poll_socket(client_context *ctx) {
  int result;
  do {
    result = network_receive_pending(ctx, on_incoming_message, ctx);
  } while (result > 0);
}

/* =========================================================================
 * SCREEN: chat — inline edit (called when user presses E in select mode)
 *
 * Opens the input box pre-filled with the selected message text, lets
 * the user edit it, and on Enter calls network_edit_message().
 * Returns 1 if the message was updated, 0 if the user cancelled.
 * ====================================================================== */

static int chat_edit_selected(client_context *ctx, WINDOW *inp_win, int msg_h) {
  chat_line_t *line = history_get(s_selected_index);
  if (!line || !line->is_mine || line->is_deleted || line->timestamp == 0) {
    return 0;
  }

  (void)msg_h; /* reserved for future layout use */

  /* Copy existing text into editing buffer */
  char edit_buf[INPUT_MAX + 1];
  memset(edit_buf, 0, sizeof(edit_buf));
  strncpy(edit_buf, line->text, INPUT_MAX);
  int edit_len = (int)strlen(edit_buf);
  int inp_scroll = 0;

  ui_set_status("Edit message. Enter: confirm   Esc: cancel");

  for (;;) {
    int field_w = COLS - 4;
    if (field_w < 1) {
      field_w = 1;
    }

    if (edit_len - inp_scroll >= field_w) {
      inp_scroll = edit_len - field_w + 1;
    }
    if (inp_scroll < 0) {
      inp_scroll = 0;
    }

    werase(inp_win);
    wattron(inp_win, COLOR_PAIR(COLOR_PAIR_BORDER));
    box(inp_win, 0, 0);
    wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_BORDER));
    wattron(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);
    mvwprintw(inp_win, 0, 2, " Edit ");
    wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);

    wattron(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL));
    mvwhline(inp_win, 1, 2, ' ', field_w);
    mvwprintw(inp_win, 1, 2, "%.*s", field_w, edit_buf + inp_scroll);
    wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL));

    int cur_col = 2 + (edit_len - inp_scroll);
    if (cur_col > COLS - 3) {
      cur_col = COLS - 3;
    }
    wmove(inp_win, 1, cur_col);

    wnoutrefresh(inp_win);
    doupdate();

    /* Blocking getch for the edit dialog */
    wtimeout(inp_win, -1);
    int ch = wgetch(inp_win);
    wtimeout(inp_win, POLL_TIMEOUT_MS);

    if (ch == ASCII_ESC) {
      return 0; /* cancelled */
    }

    if (ch == '\n' || ch == KEY_ENTER) {
      if (edit_len == 0) {
        return 0;
      }
      if (network_edit_message(ctx, line->timestamp, edit_buf) == 0) {
        /* Update local history */
        strncpy(line->text, edit_buf, INPUT_MAX);
        line->text[INPUT_MAX] = '\0';
        return 1;
      }
      return 0;
    }

    if (ch == KEY_BACKSPACE || ch == ASCII_DEL || ch == '\b') {
      if (edit_len > 0) {
        edit_buf[--edit_len] = '\0';
      }
    } else if (ch >= ASCII_SPACE && ch < ASCII_DEL && edit_len < INPUT_MAX) {
      edit_buf[edit_len++] = (char)ch;
      edit_buf[edit_len] = '\0';
    }
  }
}

/* =========================================================================
 * SCREEN: chat — delete selected (called when user presses D in select mode)
 * ====================================================================== */

static void chat_delete_selected(client_context *ctx) {
  chat_line_t *line = history_get(s_selected_index);
  if (!line || !line->is_mine || line->is_deleted || line->timestamp == 0) {
    return;
  }

  /* Confirm */
  WINDOW *confirm =
      make_box(CONFIRM_BOX_H, CONFIRM_BOX_W + 4, "Delete Message");
  mvwprintw(confirm, 2, 3, "Delete this message?");
  mvwprintw(confirm, 3, CONFIRM_BTN_COL, "[Y] Yes        [N] No");
  wnoutrefresh(confirm);
  doupdate();

  wtimeout(confirm, -1);
  int ans = wgetch(confirm);
  delwin(confirm);
  clear();
  refresh();

  if (ans != 'y' && ans != 'Y') {
    return;
  }

  if (network_delete_message(ctx, line->timestamp) == 0) {
    line->is_deleted = 1;
    line->text[0] = '\0';
  }
}

static void draw_user_list(WINDOW *win, client_context *ctx, int h, int w) {
  int i;
  int max_visible = h - USER_LIST_H_OFFSET;

  werase(win);
  wattron(win, COLOR_PAIR(COLOR_PAIR_BORDER));
  box(win, 0, 0);
  wattroff(win, COLOR_PAIR(COLOR_PAIR_BORDER));

  wattron(win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);
  mvwprintw(win, 0, 2, " Online (%u) ", (unsigned int)ctx->channel_user_count);
  wattroff(win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);

  for (i = 0; i < ctx->channel_user_count && i < max_visible; i++) {
    char name[USERNAME_LENGTH];
    uint8_t sid = ctx->channel_users[i];

    /* Resolve name using your 256-slot cache */
    lookup_username(ctx, sid, name, sizeof(name));

    if (name[0] == '\0') {
      snprintf(name, sizeof(name), "[%u]", (unsigned int)sid);
    }

    if (sid == ctx->account_id) {
      wattron(win, COLOR_PAIR(COLOR_PAIR_MINE) | A_BOLD);
      mvwprintw(win, i + 1, 1, "> %-16s", ctx->username);
      wattroff(win, COLOR_PAIR(COLOR_PAIR_MINE) | A_BOLD);
    } else {
      wattron(win, COLOR_PAIR(COLOR_PAIR_THEIRS));
      mvwprintw(win, i + 1, 1, "  %-16s", name);
      wattroff(win, COLOR_PAIR(COLOR_PAIR_THEIRS));
    }
  }
  wnoutrefresh(win);
}

/* =========================================================================
 * SCREEN: chat — main loop
 * ====================================================================== */

// chat_exit_reason_t ui_screen_chat(client_context *ctx) {
//   char title_right[TITLE_BUF_SIZE];
//   char input[INPUT_MAX + 1];
//   int input_len;
//   int inp_scroll;
//   int msg_h;
//   int inp_y;
//   WINDOW *msg_win;
//   WINDOW *inp_win;

//   snprintf(title_right, sizeof(title_right), "User: %s   Channel: %u",
//            ctx->username, (unsigned int)ctx->current_channel_id);
//   draw_title("BIG Chat  -  Messaging | Enter: send  PgUp/PgDn: scroll  Up: "
//              "select msg  Esc: channels",
//              title_right);
//   ui_set_status("Loading history...");

//   msg_h = LINES - 4;
//   if (msg_h < 3) {
//     msg_h = 3;
//   }
//   inp_y = LINES - 3;

//   msg_win = newwin(msg_h, COLS, 1, 0);
//   inp_win = newwin(3, COLS, inp_y, 0);
//   keypad(inp_win, TRUE);
//   wtimeout(inp_win, POLL_TIMEOUT_MS);

//   memset(input, 0, sizeof(input));
//   input_len = 0;
//   inp_scroll = 0;
//   s_scroll_offset = 0;
//   s_select_mode = 0;
//   s_selected_index = 0;
//   history_clear();

//   /* --- Load channel history before entering the event loop --- */
//   network_fetch_history(ctx, on_history_message, ctx, HISTORY_FETCH);

//   /* After loading history, scroll position is at the bottom (newest).
//      Offset shows most recent HISTORY_FETCH messages; leave scroll at 0. */

//   ui_set_status("Enter: send   Ctrl+U/Ctrl+D: scroll   Up: select msg   "
//                 "Esc: channels");

//   while (ctx->state == STATE_MESSAGING) {
//     int ch;

//     /* --- Redraw history panel --- */
//     draw_history(msg_win, msg_h, COLS);

//     /* --- Redraw input / mode indicator box --- */
//     werase(inp_win);
//     wattron(inp_win, COLOR_PAIR(COLOR_PAIR_BORDER));
//     box(inp_win, 0, 0);
//     wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_BORDER));

//     if (s_select_mode) {
//       /* In select mode, show mode label and hint */
//       wattron(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);
//       mvwprintw(inp_win, 0, 2, " SELECT MODE ");
//       wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);

//       wattron(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL));
//       mvwprintw(inp_win, 1, 2,
//                 "Up/Down: navigate   E: edit   D: delete   "
//                 "Esc: back");
//       wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL));
//     } else {
//       int field_w;
//       int cur_col;

//       wattron(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);
//       mvwprintw(inp_win, 0, 2, " Message ");
//       wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);

//       field_w = COLS - 4;
//       if (field_w < 1) {
//         field_w = 1;
//       }

//       if (input_len - inp_scroll >= field_w) {
//         inp_scroll = input_len - field_w + 1;
//       }
//       if (inp_scroll < 0) {
//         inp_scroll = 0;
//       }

//       wattron(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL));
//       mvwhline(inp_win, 1, 2, ' ', field_w);
//       mvwprintw(inp_win, 1, 2, "%.*s", field_w, input + inp_scroll);
//       wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL));

//       cur_col = 2 + (input_len - inp_scroll);
//       if (cur_col > COLS - 3) {
//         cur_col = COLS - 3;
//       }
//       wmove(inp_win, 1, cur_col);
//     }

//     wnoutrefresh(inp_win);
//     doupdate();

//     /* --- Keyboard input --- */
//     ch = wgetch(inp_win);

//     if (ch == ERR) {
//       /* timeout — poll socket */
//       chat_poll_socket(ctx);
//       continue;
//     }

//     /* ---- SELECT MODE keys ---- */
//     if (s_select_mode) {
//       int total = s_history_total < HISTORY_MAX ? s_history_total :
//       HISTORY_MAX;

//       if (ch == ASCII_ESC) {
//         s_select_mode = 0;
//         ui_set_status("Enter: send   PgUp/PgDn: scroll   "
//                       "Up: select msg   Esc: channels");

//       } else if (ch == KEY_UP) {
//         if (s_selected_index > 0) {
//           s_selected_index--;
//           /* Keep selected line visible */
//           int visible = msg_h - 2;
//           int first = total - visible - s_scroll_offset;
//           if (s_selected_index < first) {
//             s_scroll_offset++;
//           }
//         }
//       } else if (ch == KEY_DOWN) {
//         if (s_selected_index < total - 1) {
//           s_selected_index++;
//           int visible = msg_h - 2;
//           int first = total - visible - s_scroll_offset;
//           if (s_selected_index >= first + visible) {
//             if (s_scroll_offset > 0) {
//               s_scroll_offset--;
//             }
//           }
//         }
//       } else if (ch == 'e' || ch == 'E') {
//         /* Edit — only own messages */
//         const chat_line_t *sel = history_get(s_selected_index);
//         if (sel && sel->is_mine && !sel->is_deleted && sel->timestamp != 0) {
//           chat_edit_selected(ctx, inp_win, msg_h);
//           s_select_mode = 0;
//           ui_set_status("Enter: send   PgUp/PgDn: scroll   "
//                         "Up: select msg   Esc: channels");
//         }
//       } else if (ch == 'd' || ch == 'D') {
//         /* Delete — only own messages */
//         const chat_line_t *sel = history_get(s_selected_index);
//         if (sel && sel->is_mine && !sel->is_deleted && sel->timestamp != 0) {
//           chat_delete_selected(ctx);
//           s_select_mode = 0;
//           ui_set_status("Enter: send   Ctrl+u/Ctrl+d: scroll   "
//                         "Up: select msg   Esc: channels");
//         }
//       }
//       continue;
//     }

//     /* ---- NORMAL MODE keys ---- */

//     if (ch == ASCII_ESC) {
//       ctx->state = STATE_LOGGED_IN;
//       delwin(msg_win);
//       delwin(inp_win);
//       clear();
//       refresh();
//       return CHAT_EXIT_CHANNEL_LIST;
//     }

//     if (ch == KEY_UP) {
//       /* Enter select mode, starting at the most-recent message */
//       int total = s_history_total < HISTORY_MAX ? s_history_total :
//       HISTORY_MAX; if (total > 0) {
//         s_select_mode = 1;
//         s_selected_index = total - 1; /* newest */
//         s_scroll_offset = 0;
//         ui_set_status("SELECT: Up/Down: navigate   E: edit   "
//                       "D: delete   Esc: back");
//       }
//       continue;
//     }

//     if (ch == KEY_PPAGE ||
//         ch == ASCII_CTRL_AND_U) { /* 21 is ASCII for Ctrl+U */
//       int max_scroll = s_history_total - (msg_h - 2);
//       if (max_scroll < 0) {
//         max_scroll = 0;
//       }
//       s_scroll_offset += (msg_h - 2);
//       if (s_scroll_offset > max_scroll) {
//         s_scroll_offset = max_scroll;
//       }
//       continue;
//     }

//     if (ch == KEY_NPAGE || ch == 4) { /* 4 is ASCII for Ctrl+D */
//       s_scroll_offset -= (msg_h - 2);
//       if (s_scroll_offset < 0) {
//         s_scroll_offset = 0;
//       }
//       continue;
//     }

//     if (ch == '\n' || ch == KEY_ENTER) {
//       if (input_len == 0) {
//         continue;
//       }

//       if (strcmp(input, "/quit") == 0) {
//         ctx->state = STATE_LOGGED_IN;
//         break;
//       }

//       // network_send_message(ctx, input);

//       uint64_t ts = network_send_message(ctx, input);
//       /* Optimistically add to local history with timestamp */
//       history_push(ctx->username, input, 1, ts, ctx->account_id);

//       memset(input, 0, sizeof(input));
//       input_len = 0;
//       inp_scroll = 0;

//       s_scroll_offset = 0; /* snap to bottom */
//       chat_poll_socket(ctx);
//       continue;
//     }

//     if (ch == KEY_BACKSPACE || ch == ASCII_DEL || ch == '\b') {
//       if (input_len > 0) {
//         input[--input_len] = '\0';
//       }
//       continue;
//     }

//     if (ch >= ASCII_SPACE && ch < ASCII_DEL && input_len < INPUT_MAX) {
//       input[input_len++] = (char)ch;
//       input[input_len] = '\0';
//     }

//     chat_poll_socket(ctx);
//   }

//   close(ctx->active_sock_fd);
//   ctx->active_sock_fd = -1;

//   delwin(msg_win);
//   delwin(inp_win);
//   clear();
//   refresh();
//   return CHAT_EXIT_LOGOUT;
// }

chat_exit_reason_t ui_screen_chat(client_context *ctx) {
  char title_right[TITLE_BUF_SIZE];
  char input[INPUT_MAX + 1];
  int input_len;
  int inp_scroll;
  int msg_h;
  int msg_w;
  int inp_y;
  WINDOW *msg_win;
  WINDOW *inp_win;
  WINDOW *user_win; /* Sidebar for 0x3B User List */

  /* Set up the title bar */
  snprintf(title_right, sizeof(title_right), "User: %s   Channel: %u",
           ctx->username, (unsigned int)ctx->current_channel_id);
  draw_title("BIG Chat  -  Messaging | Enter: send  PgUp/PgDn: scroll  Up: "
             "select msg  Esc: channels",
             title_right);
  ui_set_status("Loading history...");

  /* Height calculations */
  msg_h = LINES - 4;
  if (msg_h < 3) {
    msg_h = 3;
  }
  inp_y = LINES - 3;

  /* Width calculations - Dynamic splitting for sidebar */
  msg_w = COLS - SIDEBAR_WIDTH;
  if (msg_w < MIN_MSG_WIDTH) {
    msg_w = COLS; /* Fallback: screen too narrow, hide sidebar */
  }

  /* Create windows using calculated dimensions */
  msg_win = newwin(msg_h, msg_w, 1, 0);
  user_win = newwin(msg_h, SIDEBAR_WIDTH, 1, msg_w);
  inp_win = newwin(3, COLS, inp_y, 0);

  keypad(inp_win, TRUE);
  wtimeout(inp_win, POLL_TIMEOUT_MS);

  /* State initialization */
  memset(input, 0, sizeof(input));
  input_len = 0;
  inp_scroll = 0;
  s_scroll_offset = 0;
  s_select_mode = 0;
  s_selected_index = 0;
  history_clear();

  /* --- Load channel history before entering the event loop --- */
  network_fetch_history(ctx, on_history_message, ctx, HISTORY_FETCH);

  ui_set_status("Enter: send   Ctrl+U/Ctrl+D: scroll   Up: select msg   "
                "Esc: channels");

  while (ctx->state == STATE_MESSAGING) {
    int ch;

    /* 1. Redraw message history using the dynamic width */
    draw_history(msg_win, msg_h, msg_w);

    /* 2. Redraw the sidebar (0x3B Data) */
    draw_user_list(user_win, ctx, msg_h, SIDEBAR_WIDTH);

    /* 3. Redraw input / mode indicator box */
    werase(inp_win);
    wattron(inp_win, COLOR_PAIR(COLOR_PAIR_BORDER));
    box(inp_win, 0, 0);
    wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_BORDER));

    if (s_select_mode) {
      wattron(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);
      mvwprintw(inp_win, 0, 2, " SELECT MODE ");
      wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);

      wattron(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL));
      mvwprintw(inp_win, 1, 2,
                "Up/Down: navigate   E: edit   D: delete   "
                "Esc: back");
      wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL));
    } else {
      int field_w;
      int cur_col;

      wattron(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);
      mvwprintw(inp_win, 0, 2, " Message ");
      wattroff(inp_win, COLOR_PAIR(COLOR_PAIR_LABEL) | A_BOLD);

      field_w = COLS - 4;
      if (field_w < 1) {
        field_w = 1;
      }

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
    }

    wnoutrefresh(msg_win);
    wnoutrefresh(user_win);
    wnoutrefresh(inp_win);
    doupdate();

    /* --- Keyboard input --- */
    ch = wgetch(inp_win);

    if (ch == ERR) {
      /* timeout — poll socket for broadcasts (0x33, 0x3B, etc) */
      chat_poll_socket(ctx);
      continue;
    }

    /* ---- SELECT MODE keys ---- */
    if (s_select_mode) {
      int total = s_history_total < HISTORY_MAX ? s_history_total : HISTORY_MAX;

      if (ch == ASCII_ESC) {
        s_select_mode = 0;
        ui_set_status("Enter: send   PgUp/PgDn: scroll   "
                      "Up: select msg   Esc: channels");

      } else if (ch == KEY_UP) {
        if (s_selected_index > 0) {
          s_selected_index--;
          int visible = msg_h - 2;
          int first = total - visible - s_scroll_offset;
          if (s_selected_index < first) {
            s_scroll_offset++;
          }
        }
      } else if (ch == KEY_DOWN) {
        if (s_selected_index < total - 1) {
          s_selected_index++;
          int visible = msg_h - 2;
          int first = total - visible - s_scroll_offset;
          if (s_selected_index >= first + visible) {
            if (s_scroll_offset > 0) {
              s_scroll_offset--;
            }
          }
        }
      } else if (ch == 'e' || ch == 'E') {
        const chat_line_t *sel = history_get(s_selected_index);
        if (sel && sel->is_mine && !sel->is_deleted && sel->timestamp != 0) {
          chat_edit_selected(ctx, inp_win, msg_h);
          s_select_mode = 0;
          ui_set_status("Enter: send   PgUp/PgDn: scroll   "
                        "Up: select msg   Esc: channels");
        }
      } else if (ch == 'd' || ch == 'D') {
        const chat_line_t *sel = history_get(s_selected_index);
        if (sel && sel->is_mine && !sel->is_deleted && sel->timestamp != 0) {
          chat_delete_selected(ctx);
          s_select_mode = 0;
          ui_set_status("Enter: send   Ctrl+u/Ctrl+d: scroll   "
                        "Up: select msg   Esc: channels");
        }
      }
      continue;
    }

    /* ---- NORMAL MODE keys ---- */
    if (ch == ASCII_ESC) {
      ctx->state = STATE_LOGGED_IN;
      break;
    }

    if (ch == KEY_UP) {
      int total = s_history_total < HISTORY_MAX ? s_history_total : HISTORY_MAX;
      if (total > 0) {
        s_select_mode = 1;
        s_selected_index = total - 1;
        s_scroll_offset = 0;
        ui_set_status("SELECT: Up/Down: navigate   E: edit   "
                      "D: delete   Esc: back");
      }
      continue;
    }

    if (ch == KEY_PPAGE || ch == ASCII_CTRL_AND_U) {
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

    if (ch == KEY_NPAGE || ch == 4) {
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

      /* Hybrid UI: Draw locally AND send to server */
      uint64_t ts = network_send_message(ctx, input);
      history_push(ctx->username, input, 1, ts, ctx->account_id);

      memset(input, 0, sizeof(input));
      input_len = 0;
      inp_scroll = 0;
      s_scroll_offset = 0;
      chat_poll_socket(ctx);
      continue;
    }

    if (ch == KEY_BACKSPACE || ch == ASCII_DEL || ch == '\b') {
      if (input_len > 0) {
        input[--input_len] = '\0';
      }
      continue;
    }

    if (ch >= ASCII_SPACE && ch < ASCII_DEL && input_len < INPUT_MAX) {
      input[input_len++] = (char)ch;
      input[input_len] = '\0';
    }

    chat_poll_socket(ctx);
  }

  /* Cleanup all windows */
  delwin(msg_win);
  delwin(user_win);
  delwin(inp_win);

  clear();
  refresh();

  if (ctx->state != STATE_LOGGED_IN) {
    close(ctx->active_sock_fd);
    ctx->active_sock_fd = -1;
    return CHAT_EXIT_LOGOUT;
  }

  return CHAT_EXIT_CHANNEL_LIST;
}