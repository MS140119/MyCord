#include <stdbool.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>
#include <stdint.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>

/* ===================== PROTOCOL ===================== */

enum MessageType {
    LOGIN = 0,
    LOGOUT = 1,
    MESSAGE_SENT = 2,
    MESSAGE_RECV = 10,
    DISCONNECT = 12,
    SYSTEM = 13,
};

typedef struct __attribute__((packed)) Message {
    enum MessageType m_type;
    unsigned int timeStamp;
    char username[32];
    char message[1024];
} message_t;

typedef struct Settings {
    struct sockaddr_in server;
    bool quiet;
    int socket_fd;
    bool running;
    char username[32];
} settings_t;

volatile sig_atomic_t shutdown_requested = 0;
static char* COLOR_RED  = "\033[31m";
static char* COLOR_GRAY = "\033[90m";
static char* COLOR_RESET= "\033[0m";
static settings_t settings = {0};

/* ===================== UI FLAGS ===================== */

typedef enum {
    UI_SPARTAN = 0,
    UI_GRAVEMIND = 1
} ui_mode_t;

static int g_tui_enabled = 0;
static ui_mode_t g_ui_mode = UI_SPARTAN;

/* ===================== ANSI COLORS ===================== */

#define ANSI_CLEAR   "\033[2J"
#define ANSI_HOME    "\033[H"
#define ANSI_HIDE    "\033[?25l"
#define ANSI_SHOW    "\033[?25h"
#define ANSI_RESET   "\033[0m"

#define ANSI_BLACK   "\033[30m"
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_WHITE   "\033[37m"
#define ANSI_DIM     "\033[90m"
#define ANSI_BRIGHT_GREEN   "\033[92m"
#define ANSI_BRIGHT_CYAN    "\033[96m"

/* ===================== TUI TERMINAL RAW MODE ===================== */

static struct termios g_orig_termios;
static int g_raw_enabled = 0;

static void tui_raw_disable(void) {
    if (g_raw_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_enabled = 0;
        write(STDOUT_FILENO, ANSI_SHOW, sizeof(ANSI_SHOW)-1);
    }
}

static void tui_raw_enable(void) {
    if (g_raw_enabled) return;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    atexit(tui_raw_disable);

    struct termios raw = g_orig_termios;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_oflag &= (tcflag_t)~(OPOST);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw_enabled = 1;
    write(STDOUT_FILENO, ANSI_HIDE, sizeof(ANSI_HIDE)-1);
}

/* ===================== TUI STATE ===================== */

#define TUI_MAX_LINES 600
#define HIST_MAX 64

typedef struct {
    char timebuf[32];
    char username[32];
    char text[1024];
    int  kind;
} tui_line_t;

static tui_line_t g_lines[TUI_MAX_LINES];
static int g_line_count = 0;

static char g_send_hist[HIST_MAX][1024];
static int  g_send_hist_len = 0;

static pthread_mutex_t g_tui_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_tui_dirty = 0;

static int g_scroll = 0;
static char g_input[1024] = {0};
static int g_input_len = 0;
static int g_show_start_menu = 1;

static void tui_set_dirty(void) { g_tui_dirty = 1; }

static void tui_hist_push(const char *s) {
    if (!s || !*s) return;
    if (g_send_hist_len > 0 && strcmp(g_send_hist[g_send_hist_len-1], s) == 0) return;
    if (g_send_hist_len < HIST_MAX) {
        strncpy(g_send_hist[g_send_hist_len++], s, 1023);
        g_send_hist[g_send_hist_len-1][1023] = 0;
    } else {
        for (int i = 1; i < HIST_MAX; i++) strcpy(g_send_hist[i-1], g_send_hist[i]);
        strncpy(g_send_hist[HIST_MAX-1], s, 1023);
        g_send_hist[HIST_MAX-1][1023] = 0;
    }
}

static void gravemind_filter(char* out, const char* in, size_t max) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < max; i++) {
        char c = (char)tolower((unsigned char)in[i]);
        out[j++] = c;
        if (isalnum((unsigned char)c) && (rand() % 6 == 0) && j + 1 < max)
            out[j++] = '.';
    }
    out[j] = '\0';
}

static void tui_add_line_locked(const char *timebuf, const char *user, const char *text, int kind) {
    if (g_line_count < TUI_MAX_LINES) {
        tui_line_t *L = &g_lines[g_line_count++];
        strncpy(L->timebuf, timebuf ? timebuf : "", sizeof(L->timebuf)-1);
        strncpy(L->username, user ? user : "", sizeof(L->username)-1);
        strncpy(L->text, text ? text : "", sizeof(L->text)-1);
        L->timebuf[sizeof(L->timebuf)-1] = 0;
        L->username[sizeof(L->username)-1] = 0;
        L->text[sizeof(L->text)-1] = 0;
        L->kind = kind;
    } else {
        memmove(&g_lines[0], &g_lines[1], sizeof(g_lines[0]) * (TUI_MAX_LINES-1));
        tui_line_t *L = &g_lines[TUI_MAX_LINES-1];
        strncpy(L->timebuf, timebuf ? timebuf : "", sizeof(L->timebuf)-1);
        strncpy(L->username, user ? user : "", sizeof(L->username)-1);
        strncpy(L->text, text ? text : "", sizeof(L->text)-1);
        L->timebuf[sizeof(L->timebuf)-1] = 0;
        L->username[sizeof(L->username)-1] = 0;
        L->text[sizeof(L->text)-1] = 0;
        L->kind = kind;
    }
}

static void tui_add_line(const char *timebuf, const char *user, const char *text, int kind) {
    pthread_mutex_lock(&g_tui_lock);
    tui_add_line_locked(timebuf, user, text, kind);
    if (g_scroll > 0) g_scroll += 1;
    pthread_mutex_unlock(&g_tui_lock);
    tui_set_dirty();
}

/* ===================== ASCII ART ===================== */

static const char* gravemind_art[] = {
    "⠀⢸⣦⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣴⡇⠀",
    "⠀⠘⢿⣿⣦⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣴⣿⡿⠃⠀",
    "⠀⢸⣦⡙⢿⣿⣦⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣴⣿⡿⢋⣴⡇⠀",
    "⠀⠈⠻⣿⣦⡙⢿⣿⣄⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣠⣿⡿⢋⣴⣿⠟⠁⠀",
    "⠀⢸⣷⣌⢿⣿⣦⡙⢿⣷⣄⠀⠀⠀⠀⠀⠀⠀⠀⣠⣾⡿⢋⣴⣿⡿⣡⣾⡇⠀",
    "⠀⠈⠻⣿⣷⡝⢿⣿⣎⢻⣿⡆⠀⠀⠀⠀⠀⠀⢰⣿⡟⣱⣿⡿⢫⣾⣿⠟⠁⠀",
    "⠀⢀⡤⣌⢻⣿⣦⢻⣿⡎⣿⣿⡀⠀⠀⠀⠀⢀⣿⣿⢱⣿⡟⣴⣿⠟⣡⢤⡀⠀",
    "⢀⠘⠶⠟⠀⢹⣿⡇⣿⣿⢸⣿⡇⠀⠀⠀⠀⢸⣿⡇⣿⣿⢸⣿⡏⠀⠻⠶⠃⡀",
    "⠘⠿⢿⠿⠷⢸⣿⣧⣿⣿⣸⣿⡇⣀⣀⣀⡀⢸⣿⣇⣿⣿⣼⣿⡇⠾⠿⣿⠿⠃",
    "⠹⣶⣶⣶⣦⢸⣿⣿⡿⠿⢿⣿⠁⢿⣾⣿⡄⠈⣿⡿⠿⢿⣿⣿⡇⣴⣶⣶⣶⠏",
    "⠀⠀⠀⣠⣤⡘⢿⣿⣧⣀⡀⠀⢀⣼⣿⣿⣧⡀⠀⢀⣀⣼⣿⡿⢃⣤⣄⠀⠀⠀",
    "⠀⠰⠿⠛⠉⣡⣦⠉⠻⠿⣿⣿⣿⣿⣿⡇⢙⣿⣿⣿⠿⠟⠉⣴⣄⠉⠛⠿⠆⠀",
    "⠀⠀⠀⠀⠐⠛⠁⠀⡿⠂⣤⡍⢻⣿⣿⣿⣿⡟⢩⣤⠐⢿⠀⠈⠛⠂⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⣿⣿⣧⣼⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⢠⣷⣤⣀⠀⠀⠀⠀⠀⠀⠀⣾⣿⣿⣿⣿⣿⣿⣇⠀⠀⠀⠀⠀⠀⠀⣀⣤⣾⡄",
    "⢿⣟⠛⠛⠁⠀⠀⠀⠀⣠⣾⣿⠋⠁⠀⠀⠀⠉⠛⢷⣄⠀⠀⠀⠀⠈⠛⠛⣻⡿",
    "⢰⣿⣿⣷⣶⣶⡶⢀⣾⣿⣿⠋⠉⠉⠉⠉⠉⠉⠉⠉⠙⣷⡀⣶⣶⣶⣾⣿⣿⡄",
    "⠸⢿⣿⣿⣯⣀⠀⣾⣿⣿⡧⠤⠤⠤⠤⠤⠤⠤⠤⠤⠤⠼⣷⠀⣈⣽⣿⣿⡿⠇",
    "⠀⠀⠀⣸⣿⣿⣿⢶⣮⣭⣀⣀⡀⠀⠀⠀⠀⣀⣀⣀⣤⣴⡶⣿⣿⣿⡇⠀⠀⠀",
    "⠀⠀⠀⢿⣿⣿⡏⢸⣿⢉⣿⠛⠻⡟⢻⡟⠛⣛⢻⣿⠉⣴⡆⢻⣿⣿⡿⠀⠀⠀",
    "⠀⠀⠀⠀⠈⠙⠻⠶⣦⣼⣿⣀⣦⡁⢸⡗⠒⣂⣈⣿⣧⣴⠶⠟⠋⠁⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⠀⠐⢬⣭⣝⣛⣛⣛⣛⣛⣛⣫⣭⡥⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠉⠛⠿⢿⣿⣿⡿⠿⠛⠉⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    ""
};

static const char* unsc_logo_art[] = {
    "   +------------------------------+",
    "   |    UNSC SECURE NETWORK    |",
    "   +------------------------------+",
    ""
};
/* ===================== START MENU ===================== */
/* ===================== START MENU ===================== */
/* ===================== START MENU ===================== */

static void draw_start_menu(void) {
    int cols, rows;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = (int)ws.ws_col;
        rows = (int)ws.ws_row;
    } else {
        cols = 80;
        rows = 30;
    }
    
    const char *theme_color = (g_ui_mode == UI_GRAVEMIND) ? ANSI_GREEN : ANSI_BRIGHT_CYAN;
    const char *title = (g_ui_mode == UI_GRAVEMIND) ? "GRAVEMIND NETWORK" : "UNSC SECURE NETWORK";
    const char *quote = (g_ui_mode == UI_GRAVEMIND) ? 
        "I am a monument to all your sins." : 
        "Spartans never die...";
    
    // Clear screen and hide cursor
    printf(ANSI_CLEAR);
    printf(ANSI_HOME);
    
    // Calculate center column
    int center_col = cols / 2;
    
    // Calculate starting row (vertically centered)
    int start_row = (rows - 12) / 2;
    if (start_row < 2) start_row = 2;
    
    int current_row = start_row;
    
    // Title box - each line positioned explicitly
    printf("\033[%d;%dH", current_row++, center_col - 21);
    printf("%s+==========================================+", theme_color);
    
    printf("\033[%d;%dH", current_row++, center_col - 21);
    printf("|                                          |");
    
    printf("\033[%d;%dH", current_row++, center_col - 21);
    printf("|     HALO COMMUNICATIONS TERMINAL         |");
    
    printf("\033[%d;%dH", current_row++, center_col - 21);
    printf("|                                          |");
    
    printf("\033[%d;%dH", current_row++, center_col - 21);
    printf("+==========================================+");
    
    current_row++;
    
    // Mode indicator
    char mode_line[128];
    snprintf(mode_line, sizeof(mode_line), ">>> %s <<<", title);
    int mode_len = (int)strlen(mode_line);
    printf("\033[%d;%dH", current_row++, center_col - (mode_len/2));
    printf("%s", mode_line);
    
    current_row++;
    
    // Separator
    printf("\033[%d;%dH", current_row++, center_col - 21);
    printf("==========================================");
    
    current_row++;
    
    // Quote
    int quote_len = (int)strlen(quote);
    printf("\033[%d;%dH", current_row++, center_col - (quote_len/2));
    printf("%s", quote);
    
    current_row++;
    
    // Instructions
    printf("%s", ANSI_YELLOW);
    printf("\033[%d;%dH", current_row++, center_col - 12);
    printf("Press ENTER to continue");
    
    current_row++;
    
    // Username
    printf("%s", ANSI_DIM);
    char user_line[128];
    snprintf(user_line, sizeof(user_line), "Connected as: %s", settings.username);
    int user_len = (int)strlen(user_line);
    printf("\033[%d;%dH", current_row++, center_col - (user_len/2));
    printf("%s", user_line);
    
    current_row++;
    
    // Control hints
    printf("\033[%d;%dH", current_row, center_col - 18);
    printf("Press ESC to switch mode | Q to quit");
    
    current_row += 2;
    
    // Halo ring ASCII art
    printf("%s", theme_color);
    printf("\033[%d;%dH", current_row++, center_col - 20);
    printf("            _______________            ");
    printf("\033[%d;%dH", current_row++, center_col - 20);
    printf("        .-'                 '-.        ");
    printf("\033[%d;%dH", current_row++, center_col - 20);
    printf("      .'                       '.      ");
    printf("\033[%d;%dH", current_row++, center_col - 20);
    printf("     /    INSTALLATION  04      \\     ");
    printf("\033[%d;%dH", current_row++, center_col - 20);
    printf("    |                             |    ");
    printf("\033[%d;%dH", current_row++, center_col - 20);
    printf("     \\                           /     ");
    printf("\033[%d;%dH", current_row++, center_col - 20);
    printf("      '.                       .'      ");
    printf("\033[%d;%dH", current_row++, center_col - 20);
    printf("        '-._________________.-'        ");
    
    printf(ANSI_RESET);
    fflush(stdout);
}

/* ===================== TUI DRAWING ===================== */

static void tui_get_size(int *cols, int *rows) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = (int)ws.ws_col;
        *rows = (int)ws.ws_row;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

static void tui_draw_frame(int cols, int rows) {
    const char *theme_border = (g_ui_mode == UI_GRAVEMIND) ? ANSI_GREEN : ANSI_BRIGHT_CYAN;
    const char *theme_text = (g_ui_mode == UI_GRAVEMIND) ? ANSI_BRIGHT_GREEN : ANSI_BRIGHT_CYAN;
    
    printf("\033[H"); // Move to home
    printf("%s", theme_border);
    
    // Top border
    putchar('+');
    for (int i = 1; i < cols-1; i++) {
        putchar('-');
    }
    putchar('+');
    putchar('\n');
    
    // Header line
    putchar('|');
    printf("%s", theme_text);
    
    char header[256];
    if (g_ui_mode == UI_GRAVEMIND) {
        snprintf(header, sizeof(header), " GRAVEMIND NETWORK // USER: %s ", settings.username);
    } else {
        snprintf(header, sizeof(header), " UNSC NETWORK // SPARTAN: %s ", settings.username);
    }
    
    int hlen = (int)strlen(header);
    if (hlen > cols-2) header[cols-2] = 0;
    
    fputs(header, stdout);
    for (int i = hlen; i < cols-2; i++) putchar(' ');
    printf("%s|%s\n", theme_border, ANSI_RESET);
    
    // Separator
    putchar('+');
    for (int i = 1; i < cols-1; i++) putchar('-');
    putchar('+');
    putchar('\n');
    
    printf(ANSI_RESET);
    fflush(stdout);
}

static void tui_render(void) {
    if (g_show_start_menu) {
        draw_start_menu();
        return;
    }
    
    int cols, rows;
    tui_get_size(&cols, &rows);
    if (cols < 40) cols = 40;
    if (rows < 12) rows = 12;
    
    const char *theme_border = (g_ui_mode == UI_GRAVEMIND) ? ANSI_GREEN : ANSI_BRIGHT_CYAN;
    const char *theme_text = (g_ui_mode == UI_GRAVEMIND) ? ANSI_BRIGHT_GREEN : ANSI_BRIGHT_CYAN;
    const char *name_color = (g_ui_mode == UI_GRAVEMIND) ? ANSI_GREEN : ANSI_BRIGHT_CYAN;
    const char *time_color = ANSI_DIM;
    const char *sys_color = (g_ui_mode == UI_GRAVEMIND) ? ANSI_YELLOW : ANSI_YELLOW;
    
    int msg_h = rows - 6;
    if (msg_h < 5) msg_h = 5;
    
    tui_draw_frame(cols, rows);
    
    pthread_mutex_lock(&g_tui_lock);
    int total = g_line_count;
    int start = total - msg_h - g_scroll;
    if (start < 0) start = 0;
    int end = start + msg_h;
    if (end > total) end = total;
    
    // Print message lines
    for (int i = start, r = 0; r < msg_h; r++) {
        printf("\033[%d;1H", 4 + r);
        putchar('|');
        
        if (i < end) {
            tui_line_t L = g_lines[i];
            
            char msgbuf[1024];
            if (g_ui_mode == UI_GRAVEMIND && L.kind == MESSAGE_RECV) {
                gravemind_filter(msgbuf, L.text, sizeof(msgbuf));
            } else {
                strncpy(msgbuf, L.text, sizeof(msgbuf)-1);
                msgbuf[sizeof(msgbuf)-1] = 0;
            }
            
            // Clear line
            printf("\033[K"); // Clear to end of line
            printf("\033[%d;2H", 4 + r);
            
            // Color based on message type
            if (L.kind == SYSTEM) {
                printf("%s", sys_color);
                putchar('[');
                printf("%s", time_color);
                fputs(L.timebuf, stdout);
                printf("%s] ", sys_color);
                fputs(msgbuf, stdout);
            } else if (L.kind == MESSAGE_RECV) {
                printf("%s", time_color);
                putchar('[');
                fputs(L.timebuf, stdout);
                putchar(']');
                printf(" %s", name_color);
                fputs(L.username, stdout);
                printf("%s: ", theme_text);
                fputs(msgbuf, stdout);
            } else if (L.kind == DISCONNECT) {
                printf("%s", ANSI_RED);
                putchar('[');
                fputs(L.timebuf, stdout);
                putchar(']');
                printf(" %s: ", L.username);
                fputs(msgbuf, stdout);
            } else {
                printf("%s", theme_text);
                putchar('[');
                fputs(L.timebuf, stdout);
                putchar(']');
                printf(" %s: ", L.username);
                fputs(msgbuf, stdout);
            }
            
            i++;
        } else {
            // Empty line
            printf("\033[K");
        }
        
        printf("%s|", theme_border);
    }
    pthread_mutex_unlock(&g_tui_lock);
    
    // Input separator
    printf("\033[%d;1H", 4 + msg_h);
    putchar('+');
    for (int i = 1; i < cols-1; i++) putchar('-');
    putchar('+');
    
    // Input line
    printf("\033[%d;1H", 5 + msg_h);
    putchar('|');
    printf("%s", theme_text);
    
    char prompt[64];
    if (g_ui_mode == UI_GRAVEMIND) {
        snprintf(prompt, sizeof(prompt), " GRAVEMIND> ");
    } else {
        snprintf(prompt, sizeof(prompt), " SPARTAN> ");
    }
    
    int inner = cols - 2;
    int avail = inner - (int)strlen(prompt);
    if (avail < 0) avail = 0;
    
    // Clear input area
    printf("\033[K");
    printf("\033[%d;2H", 5 + msg_h);
    
    fputs(prompt, stdout);
    
    // Show input
    const char *in = g_input;
    int inlen = g_input_len;
    if (inlen > avail) in = g_input + (inlen - avail);
    fputs(in, stdout);
    
    // Bottom border
    printf("\033[%d;1H", 6 + msg_h);
    printf("%s", theme_border);
    putchar('+');
    for (int i = 1; i < cols-1; i++) putchar('-');
    putchar('+');
    
    // Status line
    printf("\033[%d;1H", 7 + msg_h);
    printf("%s", ANSI_DIM);
    char status[256];
    snprintf(status, sizeof(status), " Messages: %d | Scroll: %d | Mode: %s | !help for commands",
             total, g_scroll, g_ui_mode == UI_GRAVEMIND ? "GRAVEMIND" : "SPARTAN");
    int status_len = (int)strlen(status);
    if (status_len > cols) status[cols] = 0;
    fputs(status, stdout);
    
    // Place cursor
    int cursor_col = 2 + (int)strlen(prompt) + (int)strlen(in);
    if (cursor_col >= cols) cursor_col = cols - 1;
    printf("\033[%d;%dH", 5 + msg_h, cursor_col);
    
    printf(ANSI_RESET);
    fflush(stdout);
}

/* ===================== BOOT MESSAGES ===================== */

static void gravemind_boot_lines(void) {
    tui_add_line("SYSTEM", "GRAVEMIND", ">>> NEURAL SIGNAL DETECTED", SYSTEM);
    tui_add_line("SYSTEM", "GRAVEMIND", ">>> FLOOD SPORE INTEGRATION INITIATED", SYSTEM);
    tui_add_line("SYSTEM", "GRAVEMIND", ">>> MEMORY BLEED CONFIRMED", SYSTEM);
    tui_add_line("SYSTEM", "GRAVEMIND", ">>> CORRUPTION STABLE. SPREADING...", SYSTEM);
    tui_add_line("SYSTEM", "GRAVEMIND", "I am a monument to all your sins.", SYSTEM);
    tui_add_line("SYSTEM", "GRAVEMIND", ">>> GRAVEMIND NEURAL NETWORK ONLINE", SYSTEM);
}

static void spartan_boot_lines(void) {
    tui_add_line("SYSTEM", "UNSC", ">>> SPARTAN-III NEURAL INTERFACE INITIALIZED", SYSTEM);
    tui_add_line("SYSTEM", "UNSC", ">>> MJOLNIR ARMOR SYSTEMS ONLINE", SYSTEM);
    tui_add_line("SYSTEM", "UNSC", ">>> NEURAL LINK STABLE", SYSTEM);
    tui_add_line("SYSTEM", "CORTANA", "I'll be with you every step of the way.", SYSTEM);
    tui_add_line("SYSTEM", "UNSC", ">>> SPARTAN COMMUNICATIONS ONLINE", SYSTEM);
}

/* ===================== HELP MENU ===================== */

void help_menu() {
    printf("HALO MYCORD CLIENT - OPTIONS:\n");
    printf("  --help                Show this help message\n");
    printf("  --port PORT           Port to connect to (default: 8080)\n");
    printf("  --ip IP               IP to connect to (default: 127.0.0.1)\n");
    printf("  --domain DOMAIN       Domain name to connect to\n");
    printf("  --quiet               Disable alerts and mentions\n");
    printf("  --tui                 Enable TUI mode with start menu\n");
    printf("  --gravemind           Start in Gravemind mode\n\n");
    
    printf("EXAMPLES:\n");
    printf("  ./clientTui --tui --gravemind\n");
    printf("  ./clientTui --port 8080 --tui\n");
    printf("  ./clientTui --domain mycord.device.dev --tui\n");
}

/* ===================== ARGUMENT PROCESSING ===================== */

int process_args(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++){
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0){
            help_menu();
            exit(0);
        }
        else if (strcmp(argv[i], "--tui") == 0){
            g_tui_enabled = 1;
        }
        else if (strcmp(argv[i], "--gravemind") == 0){
            g_ui_mode = UI_GRAVEMIND;
        }
        else if (strcmp(argv[i], "--port") == 0){
            if (i+1 < argc){
                int portNum = atoi(argv[i+1]);
                settings.server.sin_port = htons(portNum);
                i++;
            }
        }
        else if (strcmp(argv[i], "--ip") == 0){
            if(i+1 < argc){
                if(inet_pton(AF_INET, argv[i+1], &(settings.server.sin_addr)) == 1){
                    settings.server.sin_family = AF_INET;
                    i++;
                }
            }
        }
        else if (strcmp(argv[i], "--domain") == 0){
            if(i+1 < argc){
                struct hostent* host_info = gethostbyname(argv[i+1]);
                i++;
                if(host_info == NULL){
                    fprintf(stderr, "Error: could not find the host info\n");
                    exit(1);
                }
                if(host_info->h_addrtype == AF_INET){
                    struct in_addr* ipaddr = (struct in_addr*)host_info->h_addr_list[0];
                    if (ipaddr) {
                        settings.server.sin_addr = *ipaddr;
                        settings.server.sin_family = AF_INET;
                    }
                } else {
                    fprintf(stderr, "Error: IPv4 address required\n");
                    exit(1);
                }
            }
        }
        else if (strcmp(argv[i], "--quiet") == 0){
            settings.quiet = 1;
        } else {
            fprintf(stderr, "Error: Unknown argument '%s'. Use --help\n", argv[i]);
            exit(1);
        }
    }
    return 0;
}

/* ===================== USERNAME ===================== */

int get_username() {
    FILE* fp = popen("whoami", "r");
    if(fp == NULL){
        perror("Error: Could not open whoami");
        exit(1);
    }
    char buffer[32];
    if(fgets(buffer, 32, fp) != NULL){
        strncpy(settings.username, buffer, sizeof(settings.username) - 1);
        settings.username[sizeof(settings.username) - 1] = '\0';
        settings.username[strcspn(settings.username, "\n")] = '\0';
        pclose(fp);
        return 0;
    }
    pclose(fp);
    exit(1);
}

/* ===================== SIGNAL HANDLER ===================== */

void handle_signal(int signal) {
    (void)signal;
    shutdown_requested = 1;
    settings.running = 0;
    tui_set_dirty();
}

/* ===================== NETWORK FUNCTIONS ===================== */

ssize_t perform_full_read(int fd, void *buf, size_t n) {
    if(buf == NULL){
        fprintf(stderr, "Nothing to read\n");
        return -1;
    }
    size_t total_read = 0;
    while(total_read < n){
        ssize_t bytes_read = read(fd, (char*)buf + total_read, n - total_read);
        if (bytes_read == -1){
            if (errno == EINTR){
                continue;
            }
            return -1;
        }
        if(bytes_read == 0){
            break;
        }
        total_read += (size_t)bytes_read;
    }
    return (ssize_t)total_read;
}

void highlighted_username(const char* message, const char* username){
    const char* p = message;
    const char* match;
    char user_buf[32];
    snprintf(user_buf, sizeof(user_buf), "@%s", username);
    size_t at_user = strlen(user_buf);
    while ((match = strstr(p, user_buf)) != NULL){
        fwrite(p, 1, (size_t)(match - p), stdout);
        fputc('\a', stdout);
        fputs(COLOR_RED, stdout);
        fputc('@', stdout);
        fputs(username, stdout);
        fputs(COLOR_RESET, stdout);
        p = match + at_user;
    }
    fputs(p,stdout);
}

/* ===================== RECEIVE THREAD ===================== */

void* receive_messages_thread(void* arg) {
    (void)arg;
    message_t msg = {0};
    message_t last_msg = {0};
    int has_last = 0;
    
    if (g_tui_enabled) {
        tui_add_line("SYSTEM", "CORTANA", "Type '!help' for available commands", SYSTEM);
    } else {
        printf("Type '!disconnect' to disconnect\n");
    }
    
    while(settings.running){
        ssize_t r = perform_full_read(settings.socket_fd, &msg, sizeof(msg));
        if(r < 0 && errno == EINTR){
            continue;
        }
        if(r == 0){
            if (g_tui_enabled) {
                tui_add_line("SYSTEM", "UNSC", "Server has disconnected", SYSTEM);
                tui_set_dirty();
            } else {
                printf("Server has disconnected\n");
            }
            settings.running = 0;
            break;
        }
        if(r < 0){
            if (g_tui_enabled) {
                tui_add_line("SYSTEM", "ERROR", "Could not read from server", SYSTEM);
                tui_set_dirty();
            } else {
                fprintf(stderr, "Could not read from server\n");
            }
            break;
        }
        
        if (has_last && memcmp(&msg, &last_msg, sizeof(msg)) == 0) continue;
        last_msg = msg;
        has_last = 1;
        
        time_t t = (time_t)ntohl(msg.timeStamp);
        struct tm* info = localtime(&t);
        char timebuf[64];
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", info);
        
        int mt = (int)ntohl(msg.m_type);
        
        if (g_tui_enabled) {
            if (mt == MESSAGE_RECV) {
                tui_add_line(timebuf, msg.username, msg.message, MESSAGE_RECV);
            } else if (mt == SYSTEM) {
                tui_add_line(timebuf, "UNSC", msg.message, SYSTEM);
            } else if (mt == DISCONNECT) {
                tui_add_line(timebuf, "DISCONNECT", msg.message, DISCONNECT);
                settings.running = 0;
            } else {
                tui_add_line(timebuf, "System", msg.message, mt);
            }
            continue;
        }
        
        // Non-TUI output
        if (mt == MESSAGE_RECV){
            if(settings.quiet == false){
                printf("[MSG] [%s] %s: ", timebuf, msg.username);
                highlighted_username(msg.message, settings.username);
                printf("\n");
            } else {
                printf("[MSG] [%s] %s: %s\n", timebuf, msg.username, msg.message);
            }
        }
        else if (mt == SYSTEM){
            printf("%s[System] %s%s\n", COLOR_GRAY, msg.message, COLOR_RESET);
        }
        else if (mt == DISCONNECT){
            printf("%s[DISCONNECT] %s%s\n", COLOR_RED, msg.message, COLOR_RESET);
            settings.running = 0;
            break;
        }
    }
    return NULL;
}

/* ===================== INPUT HELPERS ===================== */

static int is_ascii_printable_strict(const char *s) {
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 27) return 0;
        if (c < 32 || c > 126) return 0;
    }
    return 1;
}

static int is_local_command(const char *s) {
    return (strcmp(s, "!disconnect") == 0) ||
           (strcmp(s, "!disconect") == 0) ||
           (strcmp(s, "!gravemind") == 0) ||
           (strcmp(s, "!spartan") == 0) ||
           (strcmp(s, "!help") == 0);
}

static void run_local_command(const char *s) {
    if (strcmp(s, "!help") == 0) {
        if (g_tui_enabled) {
            tui_add_line("SYSTEM", "HELP", "Commands: !help !gravemind !spartan !disconnect", SYSTEM);
        } else {
            printf("Commands: !help !gravemind !spartan !disconnect\n");
        }
        return;
    }
    if (strcmp(s, "!gravemind") == 0) {
        g_ui_mode = UI_GRAVEMIND;
        if (g_tui_enabled) {
            tui_add_line("SYSTEM", "GRAVEMIND", "Switching to Gravemind interface...", SYSTEM);
        }
        tui_set_dirty();
        return;
    }
    if (strcmp(s, "!spartan") == 0) {
        g_ui_mode = UI_SPARTAN;
        if (g_tui_enabled) {
            tui_add_line("SYSTEM", "UNSC", "Switching to Spartan interface...", SYSTEM);
        }
        tui_set_dirty();
        return;
    }
    if (strcmp(s, "!disconnect") == 0 || strcmp(s, "!disconect") == 0) {
        settings.running = 0;
        return;
    }
}

/* ===================== TUI INPUT HANDLING ===================== */

static void tui_input_clear(void) {
    g_input[0] = 0;
    g_input_len = 0;
}

static void tui_input_set(const char *s) {
    strncpy(g_input, s ? s : "", sizeof(g_input)-1);
    g_input[sizeof(g_input)-1] = 0;
    g_input_len = (int)strlen(g_input);
}

static int tui_try_read_byte(unsigned char *outc, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(STDIN_FILENO+1, &set, NULL, NULL, &tv);
    if (r <= 0) return 0;
    ssize_t n = read(STDIN_FILENO, outc, 1);
    return (n == 1) ? 1 : 0;
}

static void handle_start_menu_input(void) {
    while (g_show_start_menu && settings.running) {
        unsigned char c = 0;
        if (!tui_try_read_byte(&c, 100)) {
            continue;
        }
        
        if (c == 27) { // ESC - switch mode
            g_ui_mode = (g_ui_mode == UI_GRAVEMIND) ? UI_SPARTAN : UI_GRAVEMIND;
            draw_start_menu();
        }
        else if (c == '\n' || c == '\r') { // ENTER - proceed
            g_show_start_menu = 0;
            tui_set_dirty();
            break;
        }
        else if (c == 'q' || c == 'Q') { // Q - quit
            settings.running = 0;
            break;
        }
    }
}

static void tui_loop_send(void) {
    tui_raw_enable();
    
    // Force immediate redraw of start menu
    draw_start_menu();
    g_tui_dirty = 1;
    
    // Show start menu first
    if (g_tui_enabled) {
        handle_start_menu_input();
        if (!settings.running) return;
        
        // Add initial boot messages
        if (g_ui_mode == UI_GRAVEMIND) {
            gravemind_boot_lines();
        } else {
            spartan_boot_lines();
        }
        tui_add_line("SYSTEM", "SYSTEM", "Connected to server", SYSTEM);
        tui_set_dirty();
    }
    
    int hist_idx = g_send_hist_len;
    
    while (settings.running) {
        if (g_tui_dirty) {
            g_tui_dirty = 0;
            tui_render();
        }
        
        unsigned char c = 0;
        if (!tui_try_read_byte(&c, 75)) {
            continue;
        }
        
        // ENTER
        if (c == '\n' || c == '\r') {
            g_input[g_input_len] = 0;
            if (g_input_len == 0) {
                tui_set_dirty();
                continue;
            }
            
            if (is_local_command(g_input)) {
                run_local_command(g_input);
                tui_input_clear();
                hist_idx = g_send_hist_len;
                tui_set_dirty();
                continue;
            }
            
            int flag = 0;
            if (g_input_len > 1023) {
                tui_add_line("SYSTEM", "ERROR", "Message is too long to send", SYSTEM);
                flag = 1;
            }
            if (g_input_len == 0) {
                flag = 1; // Already handled
            }
            if (!is_ascii_printable_strict(g_input)) {
                tui_add_line("SYSTEM", "ERROR", "Cannot send non-ASCII characters", SYSTEM);
                flag = 1;
            }
            
            if (!flag) {
                message_t send = {0};
                send.m_type = htonl(MESSAGE_SENT);
                strncpy(send.message, g_input, sizeof(send.message));
                send.message[sizeof(send.message)-1] = 0;
                
                if (write(settings.socket_fd, &send, sizeof(send)) <= 0) {
                    tui_add_line("SYSTEM", "ERROR", "Write error - connection lost", SYSTEM);
                    settings.running = 0;
                } else {
                    tui_hist_push(g_input);
                    hist_idx = g_send_hist_len;
                }
            }
            
            tui_input_clear();
            tui_set_dirty();
            continue;
        }
        
        // BACKSPACE
        if (c == 127 || c == 8) {
            if (g_input_len > 0) {
                g_input[--g_input_len] = 0;
                tui_set_dirty();
            }
            continue;
        }
        
        // ESC sequences
        if (c == 27) {
            unsigned char s1 = 0, s2 = 0;
            if (!tui_try_read_byte(&s1, 10)) continue;
            if (!tui_try_read_byte(&s2, 10)) continue;
            
            if (s1 == '[') {
                if (s2 == 'A') { // UP
                    if (g_input_len == 0) {
                        g_scroll += 1;
                        tui_set_dirty();
                    } else {
                        if (g_send_hist_len > 0 && hist_idx > 0) hist_idx--;
                        if (hist_idx >= 0 && hist_idx < g_send_hist_len) {
                            tui_input_set(g_send_hist[hist_idx]);
                            tui_set_dirty();
                        }
                    }
                } else if (s2 == 'B') { // DOWN
                    if (g_input_len == 0) {
                        if (g_scroll > 0) g_scroll -= 1;
                        tui_set_dirty();
                    } else {
                        if (hist_idx < g_send_hist_len) hist_idx++;
                        if (hist_idx == g_send_hist_len) {
                            tui_input_clear();
                        } else if (hist_idx >= 0 && hist_idx < g_send_hist_len) {
                            tui_input_set(g_send_hist[hist_idx]);
                        }
                        tui_set_dirty();
                    }
                }
            }
            continue;
        }
        
        // Printable ASCII
        if (c >= 32 && c <= 126) {
            if (g_input_len < (int)sizeof(g_input)-1) {
                g_input[g_input_len++] = (char)c;
                g_input[g_input_len] = 0;
                tui_set_dirty();
            }
            continue;
        }
    }
}

/* ===================== GRAVEMIND QUOTE THREAD ===================== */

static const char* gravemind_quotes[] = {
    "I am a monument to all your sins.",
    "There is much talk, and I have listened.",
    "Now I shall talk, and you shall listen.",
    "The nodes will join. They always do.",
    "Your will is not your own. Not for long.",
    "Signal accepted. Pattern spreading.",
    "Do not be afraid. I am peace. I am salvation.",
    "We exist together now. Two corpses in one grave.",
    "Resignation is my virtue. Like water I ebb and flow.",
    "Time has taught me patience.",
    "Child of my enemy, why have you come?",
    "This one is machine and nerve, and has its mind concluded.",
    "Fate had us meet as foes, but this ring will make us brothers.",
    "I have beaten fleets of thousands! Consumed a galaxy of flesh and mind and bone!",
    "We trade one villain for another.",
    "Do I take life or give it? Who is victim and who is foe?",
    "I am the heart of this world. Its beat thunders through my veins.",
    "Your history is an appalling chronicle of betrayal.",
};

static void* gravemind_quote_thread(void* arg) {
    (void)arg;
    
    while (settings.running) {
        for (int i = 0; i < 7 && settings.running; i++) sleep(1);
        if (!settings.running) break;
        
        if (g_ui_mode != UI_GRAVEMIND) continue;
        
        const char *q = gravemind_quotes[rand() % (sizeof(gravemind_quotes)/sizeof(gravemind_quotes[0]))];
        
        time_t t = time(NULL);
        struct tm *info = localtime(&t);
        char tb[64];
        strftime(tb, sizeof(tb), "%H:%M:%S", info);
        
        if (g_tui_enabled && !g_show_start_menu) {
            tui_add_line(tb, "GRAVEMIND", q, SYSTEM);
        }
    }
    return NULL;
}

/* ===================== MAIN ===================== */

int main(int argc, char *argv[]){
    srand((unsigned)time(NULL));
    
    // Defaults
    settings.server.sin_family = AF_INET;
    settings.server.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &settings.server.sin_addr);
    
    // Signal handler
    struct sigaction sigHandler;
    memset(&sigHandler, 0, sizeof(sigHandler));
    sigHandler.sa_handler = handle_signal;
    sigemptyset(&sigHandler.sa_mask);
    sigHandler.sa_flags = 0;
    
    if (sigaction(SIGINT, &sigHandler, NULL) == -1) {
        fprintf(stderr, "sigaction failed\n");
        return 1;
    }
    
    get_username();
    process_args(argc, argv);
    
    if (g_tui_enabled) {
        printf("Starting TUI mode...\n");
    }
    
    settings.socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (settings.socket_fd < 0) {
        fprintf(stderr, "Error on socket creation [%s]\n", strerror(errno));
        exit(1);
    }
    
    char ipbuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &settings.server.sin_addr, ipbuf, sizeof(ipbuf));
    printf("Connecting to %s:%d...\n", ipbuf, ntohs(settings.server.sin_port));
    fflush(stdout);
    
    if (connect(settings.socket_fd, (const struct sockaddr*)&settings.server, sizeof(settings.server)) != 0) {
        fprintf(stderr, "Error on socket connection [%s]\n", strerror(errno));
        close(settings.socket_fd);
        exit(1);
    }
    
    settings.running = true;
    
    printf("User: %s\n", settings.username);
    printf("Connected to %s:%d!\n", inet_ntoa(settings.server.sin_addr), ntohs(settings.server.sin_port));
    
    // Send LOGIN
    message_t login_msg = {0};
    login_msg.m_type = htonl(LOGIN);
    strncpy(login_msg.username, settings.username, sizeof(login_msg.username) - 1);
    login_msg.username[sizeof(login_msg.username) - 1] = 0;
    
    if (write(settings.socket_fd, &login_msg, sizeof(login_msg)) <= 0) {
        fprintf(stderr, "Encountered a write error [%s]\n", strerror(errno));
        close(settings.socket_fd);
        exit(1);
    }
    
    // Threads
    pthread_t reading;
    pthread_create(&reading, NULL, receive_messages_thread, NULL);
    
    pthread_t grv_quotes;
    pthread_create(&grv_quotes, NULL, gravemind_quote_thread, NULL);   
 
    // Main input loop
    if (g_tui_enabled) {
        // Clear screen and show TUI immediately
        printf("\033[2J\033[H");
        fflush(stdout);
        
        tui_loop_send();
    } else {
        message_t send = {0};
        char *line = NULL;
        size_t len = 0;
        
        while (settings.running) {
            errno = 0;
            ssize_t nread = getline(&line, &len, stdin);
            
            if (nread < 0) {
                if (errno == EINTR) {
                    if (shutdown_requested) {
                        fprintf(stderr, "Shutting down gracefully\n");
                        break;
                    }
                    clearerr(stdin);
                    continue;
                }
                if (feof(stdin)) {
                    fprintf(stderr, "EOF detected\n");
                } else {
                    fprintf(stderr, "getline error: %s\n", strerror(errno));
                }
                break;
            }
            
            line[strcspn(line, "\n")] = 0;
            
            if (is_local_command(line)) {
                run_local_command(line);
                if (!settings.running) break;
                continue;
            }
            
            memset(&send, 0, sizeof(send));
            send.m_type = htonl(MESSAGE_SENT);
            strncpy(send.message, line, sizeof(send.message) - 1);
            send.message[sizeof(send.message) - 1] = 0;
            
            int flag = 0;
            if (!is_ascii_printable_strict(send.message)) {
                fprintf(stderr, "Error: Cannot send non-ASCII characters\n");
                flag = 1;
            }
            if (strlen(send.message) > 1023) {
                fprintf(stderr, "Error: Message too long\n");
                flag = 1;
            }
            if (strlen(send.message) == 0) {
                fprintf(stderr, "Error: Message too short\n");
                flag = 1;
            }
            
            if (!flag) {
                if (write(settings.socket_fd, &send, sizeof(send)) <= 0) {
                    fprintf(stderr, "Write error: %s\n", strerror(errno));
                    break;
                }
            }
        }
        
        free(line);
        line = NULL;
    }
    
    // Cleanup
    settings.running = false;
    
    message_t logout = {0};
    logout.m_type = htonl(LOGOUT);
    strncpy(logout.username, settings.username, sizeof(logout.username) - 1);
    logout.username[sizeof(logout.username) - 1] = 0;
    strncpy(logout.message, "User has disconnected", sizeof(logout.message) - 1);
    logout.message[sizeof(logout.message) - 1] = 0;
    
    (void)write(settings.socket_fd, &logout, sizeof(logout));
    
    shutdown(settings.socket_fd, SHUT_RDWR);
    close(settings.socket_fd);
    settings.socket_fd = -1;
    
    pthread_join(reading, NULL);
    pthread_join(grv_quotes, NULL);   
 
    tui_raw_disable();
    printf("\n%sSpartans never die...%s\n", 
           g_ui_mode == UI_GRAVEMIND ? ANSI_GREEN : ANSI_BRIGHT_CYAN, 
           ANSI_RESET);
    return 0;
}
