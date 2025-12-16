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

volatile sig_atomic_t shutdown_requested = 0; //global flag for sig handling
int shutdown_flag = 0;
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

#define ANSI_BLUE    "\033[34m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_WHITE   "\033[37m"
#define ANSI_DIM     "\033[90m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"

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
    int  kind; // MESSAGE_RECV/SYSTEM/DISCONNECT/etc
} tui_line_t;

static tui_line_t g_lines[TUI_MAX_LINES];
static int g_line_count = 0;

static char g_send_hist[HIST_MAX][1024];
static int  g_send_hist_len = 0;

static pthread_mutex_t g_tui_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_tui_dirty = 0;

static int g_scroll = 0;           // how many lines up from bottom
static char g_input[1024] = {0};   // input buffer
static int g_input_len = 0;

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
        // shift up (simple)
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
    // when new messages arrive, keep view pinned to bottom unless user scrolled up
    if (g_scroll > 0) g_scroll += 1;
    pthread_mutex_unlock(&g_tui_lock);
    tui_set_dirty();
}

/* ===================== TUI DRAWING ===================== */

static void tui_get_size(int *cols, int *rows) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = (int)ws.ws_col;
        *rows = (int)ws.ws_row;
    } else {
        *cols = 100;
        *rows = 30;
    }
}

static void tui_repeat_char(char c, int n) {
    for (int i = 0; i < n; i++) putchar(c);
}

static void tui_draw_frame(int cols, int rows) {
    // theme
    const char *theme = (g_ui_mode == UI_GRAVEMIND) ? ANSI_GREEN : ANSI_CYAN;
    const char *hdr   = (g_ui_mode == UI_GRAVEMIND) ? "MYCORD // FLOOD CHANNEL // GRAVEMIND" : "MYCORD // UNSC SECURE CHANNEL // SPARTAN-III";

    printf(ANSI_HOME);
    printf(ANSI_CLEAR);
    printf("%s", theme);

    // top border
    putchar('+'); tui_repeat_char('-', cols-2); putchar('+'); putchar('\n');

    // header line
    putchar('|');
    printf("%s", theme);
    int pad = cols - 2;
    char header[256];
    snprintf(header, sizeof(header), "%s  %s  //  ONLINE", hdr, settings.username);
    int hlen = (int)strlen(header);
    if (hlen > pad) header[pad] = 0, hlen = pad;
    fputs(header, stdout);
    for (int i = hlen; i < pad; i++) putchar(' ');
    putchar('|'); putchar('\n');

    // separator
    putchar('+'); tui_repeat_char('-', cols-2); putchar('+'); putchar('\n');

    // message area height
    int msg_h = rows - 6; // top(1)+hdr(1)+sep(1)+input_sep(1)+input(1)+bottom(1)
    if (msg_h < 5) msg_h = 5;

    // empty message area placeholder (filled later)
    for (int r = 0; r < msg_h; r++) {
        putchar('|');
        tui_repeat_char(' ', cols-2);
        putchar('|');
        putchar('\n');
    }

    // input separator
    putchar('+'); tui_repeat_char('-', cols-2); putchar('+'); putchar('\n');

    // input line + bottom border will be printed in tui_render()
    // (we leave cursor placement there)
    fflush(stdout);
}

static void tui_write_trunc(const char *s, int max) {
    if (!s) return;
    int n = (int)strlen(s);
    if (n <= max) {
        fputs(s, stdout);
        for (int i = n; i < max; i++) putchar(' ');
    } else {
        fwrite(s, 1, (size_t)max, stdout);
    }
}

static void tui_render(void) {
    int cols, rows;
    tui_get_size(&cols, &rows);
    if (cols < 40) cols = 40;
    if (rows < 12) rows = 12;

    const char *theme = (g_ui_mode == UI_GRAVEMIND) ? ANSI_GREEN : ANSI_CYAN;
    const char *namec = (g_ui_mode == UI_GRAVEMIND) ? ANSI_GREEN : ANSI_BLUE;
    const char *timec = ANSI_DIM;

    int msg_h = rows - 6;
    if (msg_h < 5) msg_h = 5;

    // draw base frame
    tui_draw_frame(cols, rows);

    // compute which lines to show
    pthread_mutex_lock(&g_tui_lock);
    int total = g_line_count;
    int start = total - msg_h - g_scroll;
    if (start < 0) start = 0;
    int end = start + msg_h;
    if (end > total) end = total;

    // print message lines into the msg area
    int line_row = 4; // 1-based row in terminal: after top border(1), header(1), sep(1) => message starts at row 4
    for (int i = start, r = 0; r < msg_h; r++) {
        // move cursor to row
        printf("\033[%d;1H", line_row + r);

        putchar('|');

        char left[1200];
        left[0] = 0;

        if (i < end) {
            tui_line_t L = g_lines[i];

            char msgbuf[1024];
            if (g_ui_mode == UI_GRAVEMIND && L.kind == MESSAGE_RECV) {
                gravemind_filter(msgbuf, L.text, sizeof(msgbuf));
            } else {
                strncpy(msgbuf, L.text, sizeof(msgbuf)-1);
                msgbuf[sizeof(msgbuf)-1] = 0;
            }

            // build: [time] user: msg
            // we colorize time + username
            // NOTE: we can’t easily measure ANSI length, so we keep conservative padding by truncating raw text.
            // We'll print with colors directly.
            int inner = cols - 2;

            // clear the inside first
            tui_repeat_char(' ', inner);
            // go back inside row start (after '|')
            printf("\033[%d;2H", line_row + r);

            // time
            printf("%s[%s]%s ", timec, L.timebuf, theme);

            // username
            if (L.username[0]) {
                printf("%s%s%s: ", namec, L.username, theme);
            }

            // message
            // truncate message to fit remaining columns (roughly)
            // (simple: just print and let it cut)
            int printed_prefix = 2 + (int)strlen(L.timebuf) + 3 + 1 + (int)strlen(L.username) + 2; // rough
            (void)printed_prefix;
            fputs(msgbuf, stdout);

            // right border
            printf("\033[%d;%dH|", line_row + r, cols);
            i++;
        } else {
            // empty
            tui_repeat_char(' ', cols-2);
            putchar('|');
        }
    }
    pthread_mutex_unlock(&g_tui_lock);

    // input line
    printf("\033[%d;1H", 4 + msg_h); // this is the row with '+' sep already printed by frame
    // go to input row (after input separator)
    int input_row = 5 + msg_h;
    printf("\033[%d;1H", input_row);

    putchar('|');
    printf("%s", theme);

    char prompt[64];
    snprintf(prompt, sizeof(prompt), "%s> ", (g_ui_mode == UI_GRAVEMIND) ? "gravemind" : "spartan");
    int inner = cols - 2;

    // clear inside
    tui_repeat_char(' ', inner);
    printf("\033[%d;2H", input_row);

    // draw prompt + input (truncate)
    fputs(prompt, stdout);

    int avail = inner - (int)strlen(prompt);
    if (avail < 0) avail = 0;

    // if input too long, show tail
    const char *in = g_input;
    int inlen = g_input_len;
    if (inlen > avail) in = g_input + (inlen - avail);

    fputs(in, stdout);

    // right border
    printf("\033[%d;%dH|", input_row, cols);

    // bottom border
    printf("\033[%d;1H", input_row + 1);
    printf("%s", theme);
    putchar('+'); tui_repeat_char('-', cols-2); putchar('+');

    // put cursor at end of input (best-effort)
    int cursor_col = 2 + (int)strlen(prompt) + (int)strlen(in);
    if (cursor_col >= cols) cursor_col = cols - 1;
    printf("\033[%d;%dH", input_row, cursor_col);

    printf(ANSI_RESET);
    fflush(stdout);
}

/* ===================== GRAVEMIND BOOT ===================== */

static void gravemind_boot_lines(void) {
    // In TUI, we add these as system lines so they appear in the box.
    tui_add_line("GRV", "", ">>> SIGNAL DETECTED", SYSTEM);
    tui_add_line("GRV", "", ">>> NEURAL LATTICE FORMING", SYSTEM);
    tui_add_line("GRV", "", ">>> MEMORY BLEED CONFIRMED", SYSTEM);
    tui_add_line("GRV", "", ">>> NODE CORRUPTION: STABLE", SYSTEM);
    tui_add_line("GRV", "", "I am a monument to all your sins.", SYSTEM);
    tui_add_line("GRV", "", ">>> GRAVEMIND ONLINE", SYSTEM);
}

/* ===================== YOUR ORIGINAL HELP MENU ===================== */

void help_menu(){ //help menu from README doc
    printf("options:\n");
    printf("--help                show this help message and exit\n");
    printf("--port PORT           port to connect to (default: 8080)\n");
    printf("--ip IP               IP to connect to (default: \"127.0.0.1\")\n");
    printf("--domain DOMAIN       Domain name to connect to (if domain is specificed, IP must not be)\n");
    printf("--quiet               do not perform alerts or mention highlighting\n");
    printf("--tui                 enable TUI mode (arrow keys scroll)\n");
    printf("--gravemind           start in gravemind mode\n\n");

    printf("examples:\n");
    printf("./clientAi --help (prints the above message)\n");
    printf("./clientAi --port 1738 (connects to a mycord server at 127.0.0.1:1738)\n");
    printf("./clientAi --domain example.com (connects to a mycord server at example.com:8080)\n");
    printf("./clientAi --port 8080 --tui\n");
    printf("./clientAi --port 8080 --tui --gravemind\n");
}

/* ===================== YOUR ORIGINAL ARG PARSING (PLUS FLAGS) ===================== */

int process_args(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++){
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0){ //checks if the user inputs help if so display help menu
            help_menu(); //call on help_menu function
            exit(0); //exit the program
        }

        else if (strcmp(argv[i], "--tui") == 0){
            g_tui_enabled = 1;
        }

        else if (strcmp(argv[i], "--gravemind") == 0){
            g_ui_mode = UI_GRAVEMIND;
        }

        else if (strcmp(argv[i], "--port") == 0){
            if (i+1 < argc){
                int portNum = atoi(argv[i+1]); //pharsing the port number and stores it into portNum
                settings.server.sin_port = htons(portNum); //passing the portNum into the sockaddr struct while converting it to bigEndian
                i++; //incrementing the count
            }
        }

        else if (strcmp(argv[i], "--ip") == 0){
            if(i+1 < argc){
                char addr[64]; //character buffer to copy into
                if(inet_pton(AF_INET, argv[i+1], &(settings.server.sin_addr)) == 1){ //converting from string presantation string to network
                    settings.server.sin_family = AF_INET; //specifing the ip family
                    inet_ntop(AF_INET, &(settings.server.sin_addr), addr, 64); //syscall to store the ip into the struct
                    printf("IPv4 %s\n", addr); //printing the IP address
                    i++; //incrementing count
                }
            }
        }

        else if (strcmp(argv[i], "--domain") == 0){
            if(i+1 < argc){
                struct hostent* host_info = gethostbyname(argv[i+1]); //getting host name
                i++; //increment i

                if(host_info == NULL){ //checking if host_info was able to get something
                    fprintf(stderr, "Error: could not find the host info");
                    exit(1);
                }

                if(host_info->h_addrtype == AF_INET){ //if the address type is IPV4 then run the if statement
                    int j = 0;
                    struct in_addr* ipaddr;
                    while ((ipaddr = (struct in_addr*)host_info->h_addr_list[j]) != NULL ){
                        printf(" - %s\n", inet_ntoa(*ipaddr));
                        settings.server.sin_addr = *ipaddr; //saving the ip into the addr
                        settings.server.sin_family = AF_INET; //saving the family type
                        j++; //increment j
                    }

                    char debug_ip[INET_ADDRSTRLEN]; //debugging
                    inet_ntop(AF_INET, &settings.server.sin_addr, debug_ip, sizeof(debug_ip)); //getting ip formatted
                    printf("[DEBUG] Stored DNS IP in struct: %s\n", debug_ip); //print debug statement
                } else {
                    fprintf(stderr, "Error:Please enter an IVP4 address");
                    exit(1);
                }
            }
        }

        else if (strcmp(argv[i], "--quiet") == 0){
            settings.quiet = 1; //setting quiet to true so this conditional with highlighting won't trigger
        } else {
            fprintf(stderr, "Error: Unknown argument. Please use --help or -h to look at the following commands avaliable to you!\n"); //error message
            exit(1);
        }
    }
    return 0;
}

/* ===================== YOUR ORIGINAL USERNAME ===================== */

int get_username() {
    FILE* fp = popen("whoami", "r"); //reading the whoami command
    if(fp == NULL){ //checking if the command worked
        perror("Error: Could not open whoami"); //error message incase the command failed
        exit(1); //returning error code
    }

    char buffer[32]; //setting 32 bit buffer for username going to use fgets read and store into char array
    while(fgets(buffer, 32, fp) != NULL){
        strncpy(settings.username, buffer, sizeof(settings.username) - 1); //going to copy string into username struct
        settings.username[sizeof(settings.username) - 1] = '\0'; //settings the last byte in the username character array to be the null terminator
        settings.username[strcspn(settings.username, "\n")] = '\0'; //strip the new line so no acsii error on server side
        pclose(fp); //closing the file pointer
        return 0; //successfully able to get the username
    }
    pclose(fp); //still need to close the file pointer
    exit(1); //else failed to read
}

/* ===================== SIGNAL ===================== */

void handle_signal(int signal) {
    (void)signal;
    shutdown_requested = 1; //setting the flag to be true
    settings.running = 0;   //stop loops
    tui_set_dirty();
}

/* ===================== FULL READ (FIXED TO USE fd) ===================== */

ssize_t perform_full_read(int fd, void *buf, size_t n) {
    if(buf == NULL){ //checking if the buffer is pointing to null
        fprintf(stderr, "Nothing to read %s\n");
        return -1;
    }

    size_t total_read = 0; //intializing variable
    while(total_read < n){ //while total bite is less than current bytes avaliable
        ssize_t bytes_read = read(fd, (char*)buf + total_read, n - total_read);
        if (bytes_read == -1){ //error handling for interupt
            if (errno == EINTR){
                continue;
            }
            return -1; //if the errno was not interupt then return -1
        }

        if(bytes_read == 0){ //if the bytes read is 0 then end while loop since nothing is read
            break;
        }

        total_read += (size_t)bytes_read; //increment count
    }

    return (ssize_t)total_read; //once while is done return the total number of bytes
}

/* ===================== YOUR ORIGINAL MENTION HIGHLIGHT ===================== */

void highlighted_username(const char* message, const char* username){
    const char* p = message;
    const char* match; //setting up a char for match
    char user_buf[32]; //set a 32 byte buffer
    snprintf(user_buf, sizeof(user_buf), "@%s", username);
    size_t at_user = strlen(user_buf);

    while ((match = strstr(p, user_buf)) != NULL){ //iterate until a null terminate is reached
        fwrite(p, 1, (size_t)(match - p), stdout);

        fputc('\a', stdout);

        fputs(COLOR_RED, stdout); //put the coloir
        fputc('@', stdout);
        fputs(username, stdout); //put on username
        fputs(COLOR_RESET, stdout);//apply the colorreset
        p = match + at_user;
    }
    fputs(p,stdout); //apply to p
}

/* ===================== RECEIVE THREAD ===================== */

void* receive_messages_thread(void* arg) {
    (void)arg;

    message_t msg= {0}; //clearing struct and setting to 0

    // three-line duplicate guard baseline (like you screenshot)
    message_t last_msg = {0};
    int has_last = 0;

    if (g_tui_enabled) {
        tui_add_line("SYS", "", "Type '!disconect' (or !disconnect) to disconnect", SYSTEM);
    } else {
        printf("Type '!disconect' to disconnect\n"); //print message on client
    }

    while(settings.running){
        ssize_t r = perform_full_read(settings.socket_fd, &msg, sizeof(msg));
        if(r < 0 && errno == EINTR){
            continue;
        }

        if(r == 0){
            if (g_tui_enabled) {
                tui_add_line("SYS", "System", "Server has disconnected", SYSTEM);
                tui_set_dirty();
            } else {
                printf("Server has disconnected %s\n");
            }
            settings.running = 0;
            break;
        }

        if(r < 0){
            if (g_tui_enabled) {
                tui_add_line("SYS", "System", "Could not read from server", SYSTEM);
                tui_set_dirty();
            } else {
                fprintf(stderr, "Could not read from server %s\n");
            }
            break;
        }

        // === 3-line duplicate guard ===
        if (has_last && memcmp(&msg, &last_msg, sizeof(msg)) == 0) continue;
        last_msg = msg;
        has_last = 1;

        time_t t = (time_t)ntohl(msg.timeStamp); //calling on time
        struct tm* info = localtime(&t); //storing in the time struct
        char timebuf[64]; //creating a buffer to copy into
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", info); //formatting the time

        int mt = (int)ntohl(msg.m_type);

        if (g_tui_enabled) {
            if (mt == MESSAGE_RECV) {
                tui_add_line(timebuf, msg.username, msg.message, MESSAGE_RECV);
            } else if (mt == SYSTEM) {
                tui_add_line(timebuf, "UNSC FLEETCOM", msg.message, SYSTEM);
            } else if (mt == DISCONNECT) {
                tui_add_line(timebuf, "DISCONNECT", msg.message, DISCONNECT);
                settings.running = 0;
            } else {
                tui_add_line(timebuf, "System", msg.message, mt);
            }
            continue;
        }

        // ===== original print behavior (non-tui) =====
        if (mt == MESSAGE_RECV){ //checking for message recieve type to send to client side with the right format
            if(settings.quiet == false){
                printf("[MESSAGE] %d [%s] %s: ",ntohl(msg.m_type), timebuf, msg.username ); //print out the time username and the entire message highlighted
                highlighted_username(msg.message, settings.username); //calling on helper function to detech highlighted code
                printf("\n"); //print new line after the help function call since the function doesn't return a string and is of void type
            } else {
                printf("[MESSAGE] %d [%s] %s: %s\n",ntohl(msg.m_type), timebuf, msg.username, msg.message); //else if quiet is not turned on then don't call on help and just run normally
            }
        }

        if (mt == SYSTEM){ //check for system enum type
            printf("%s[System] %s %s\n", COLOR_GRAY, msg.message, COLOR_RESET); //prints out client message from server
        }

        if (mt == DISCONNECT){ //check for disconnect enum type
            printf("%s[DISCONNECT] %s %s\n", COLOR_RED, msg.message, COLOR_RESET); //prints out disconnect message to client side
            settings.running = 0;
            break;
        }
    }

    return NULL; //return null
}

/* ===================== INPUT HELPERS ===================== */

static int is_ascii_printable_strict(const char *s) {
    // block ESC sequences (arrow keys etc)
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 27) return 0;          // ESC
        if (c < 32 || c > 126) return 0; // printable ASCII only
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
            tui_add_line("CMD", "", "Commands: !help  !gravemind  !spartan  !disconnect", SYSTEM);
        } else {
            printf("Commands: !help  !gravemind  !spartan  !disconnect\n");
        }
        return;
    }
    if (strcmp(s, "!gravemind") == 0) {
        g_ui_mode = UI_GRAVEMIND;
        if (g_tui_enabled) gravemind_boot_lines();
        return;
    }
    if (strcmp(s, "!spartan") == 0) {
        g_ui_mode = UI_SPARTAN;
        if (g_tui_enabled) tui_add_line("CMD", "", "SPARTAN-III INTERFACE ONLINE", SYSTEM);
        return;
    }
    if (strcmp(s, "!disconnect") == 0 || strcmp(s, "!disconect") == 0) {
        settings.running = 0;
        return;
    }
}

/* ===================== TUI KEY INPUT LOOP ===================== */

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
    if (r <= 0) return 0; // timeout or error
    ssize_t n = read(STDIN_FILENO, outc, 1);
    return (n == 1) ? 1 : 0;
}

static void tui_loop_send(void) {
    tui_raw_enable();
    tui_set_dirty();

    int hist_idx = g_send_hist_len; // for input history navigation

    while (settings.running) {
        if (g_tui_dirty) {
            g_tui_dirty = 0;
            tui_render();
        }

        unsigned char c = 0;
        if (!tui_try_read_byte(&c, 75)) {
            continue; // keep rendering/polling
        }

        // ENTER
        if (c == '\n' || c == '\r') {
            g_input[g_input_len] = 0;

            // ignore empty
            if (g_input_len == 0) {
                tui_set_dirty();
                continue;
            }

            // local commands
            if (is_local_command(g_input)) {
                run_local_command(g_input);
                tui_input_clear();
                hist_idx = g_send_hist_len;
                tui_set_dirty();
                continue;
            }

            // ASCII checks (your original behavior, but fixed)
            int flag = 0;

            if (g_input_len > 1023) {
                tui_add_line("ERR", "", "Error: Message is too long to send", SYSTEM);
                flag = 1;
            }
            if (g_input_len == 0) {
                tui_add_line("ERR", "", "Error: Message is too short to send", SYSTEM);
                flag = 1;
            }
            if (!is_ascii_printable_strict(g_input)) {
                tui_add_line("ERR", "", "Error: Attempting to send non-ascii character", SYSTEM);
                flag = 1;
            }

            if (!flag) {
                message_t send = {0};
                send.m_type = htonl(MESSAGE_SENT);
                strncpy(send.message, g_input, sizeof(send.message));
                send.message[sizeof(send.message)-1] = 0;

                // NOTE: we do NOT locally print your message here (prevents “double print” when server echoes)
                if (write(settings.socket_fd, &send, sizeof(send)) <= 0) {
                    tui_add_line("ERR", "", "Encountered a write error", SYSTEM);
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

        // ESC sequences (arrows)
        if (c == 27) {
            unsigned char s1 = 0, s2 = 0;
            if (!tui_try_read_byte(&s1, 10)) continue;
            if (!tui_try_read_byte(&s2, 10)) continue;

            if (s1 == '[') {
                if (s2 == 'A') { // UP
                    if (g_input_len == 0) {
                        // scroll view up
                        g_scroll += 1;
                        tui_set_dirty();
                    } else {
                        // input history up
                        if (g_send_hist_len > 0 && hist_idx > 0) hist_idx--;
                        if (hist_idx >= 0 && hist_idx < g_send_hist_len) {
                            tui_input_set(g_send_hist[hist_idx]);
                            tui_set_dirty();
                        }
                    }
                } else if (s2 == 'B') { // DOWN
                    if (g_input_len == 0) {
                        // scroll view down
                        if (g_scroll > 0) g_scroll -= 1;
                        tui_set_dirty();
                    } else {
                        // input history down
                        if (hist_idx < g_send_hist_len) hist_idx++;
                        if (hist_idx == g_send_hist_len) {
                            tui_input_clear();
                        } else if (hist_idx >= 0 && hist_idx < g_send_hist_len) {
                            tui_input_set(g_send_hist[hist_idx]);
                        }
                        tui_set_dirty();
                    }
                } else if (s2 == '5') { // PgUp: ESC [ 5 ~
                    unsigned char tilde = 0;
                    tui_try_read_byte(&tilde, 10);
                    g_scroll += 5;
                    tui_set_dirty();
                } else if (s2 == '6') { // PgDn: ESC [ 6 ~
                    unsigned char tilde = 0;
                    tui_try_read_byte(&tilde, 10);
                    if (g_scroll > 5) g_scroll -= 5;
                    else g_scroll = 0;
                    tui_set_dirty();
                }
            }
            continue; // NEVER insert ESC bytes into input
        }

        // printable ascii
        if (c >= 32 && c <= 126) {
            if (g_input_len < (int)sizeof(g_input)-1) {
                g_input[g_input_len++] = (char)c;
                g_input[g_input_len] = 0;
                tui_set_dirty();
            }
            continue;
        }

        // ignore everything else
    }
}

/* ===================== GRAVEMIND QUOTE TIMER ===================== */

static void now_timestr(char out[64]) {
    time_t t = time(NULL);
    struct tm *info = localtime(&t);
    strftime(out, 64, "%Y-%m-%d %H:%M:%S", info);
}

static const char* gravemind_quotes[] = {
    "There is much talk, and I have listened, through rock and metal and time. Now I shall talk, and you shall listen."
    "I am a monument to all your sins.",
    "The nodes will join. They always do.",
    "Your will is not your own. Not for long.",
    "Signal accepted. Pattern spreading.",
    "Corruption persists. Resistance fades.",
    "You hear me now. Soon you will obey.",
    "This channel is mine. This mind is many.",
    "Do not struggle. It only hastens the merge."
    "I am a timeless chorus. Join your voice with mine, and sing victory everlasting."
    "Admit it: In the end, you will be mine."
    "Do I take life or give it? Who is victim, and who is foe?"
    " This one is but flesh and faith, and is the more deluded."
   
};
static const int gravemind_quotes_count =
    (int)(sizeof(gravemind_quotes) / sizeof(gravemind_quotes[0]));

static void* gravemind_quote_thread(void* arg) {
    (void)arg;

    while (settings.running) {
        // sleep 10–15 seconds (random)
        int delay = 10 + (rand() % 6);
        for (int i = 0; i < delay && settings.running; i++) sleep(1);
        if (!settings.running) break;

        // only emit when in gravemind mode
        if (g_ui_mode != UI_GRAVEMIND) continue;

        const char *q = gravemind_quotes[rand() % gravemind_quotes_count];

        if (g_tui_enabled) {
            char tb[64];
            now_timestr(tb);
            tui_add_line(tb, "GRAVEMIND", q, SYSTEM);
        } else {
            char tb[64];
            now_timestr(tb);
            // green system-style local line
            printf("\033[32m[%s] [GRAVEMIND] %s\033[0m\n", tb, q);
            fflush(stdout);
        }
    }
    return NULL;
}


/* ===================== MAIN ===================== */

int main(int argc, char *argv[]){
    srand((unsigned)time(NULL));

    // defaults
    settings.server.sin_family = AF_INET;
    settings.server.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &settings.server.sin_addr);

    // SIGINT handler
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

    settings.socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (settings.socket_fd < 0) {
        fprintf(stderr, "Error on socket creation [%s]\n", strerror(errno));
        exit(1);
    }

    // helpful print so it never looks frozen
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
    printf("Connected to %d:%s!\n",
           ntohs(settings.server.sin_port),
           inet_ntoa(settings.server.sin_addr));

    // send LOGIN
    message_t login_msg = {0};
    login_msg.m_type = htonl(LOGIN);
    strncpy(login_msg.username, settings.username, sizeof(login_msg.username) - 1);
    login_msg.username[sizeof(login_msg.username) - 1] = 0;

    if (write(settings.socket_fd, &login_msg, sizeof(login_msg)) <= 0) {
        fprintf(stderr, "Encountered a write error [%s]\n", strerror(errno));
        close(settings.socket_fd);
        exit(1);
    }

    // threads
    pthread_t reading;
    pthread_create(&reading, NULL, receive_messages_thread, NULL);

    pthread_t grv_quotes;
    pthread_create(&grv_quotes, NULL, gravemind_quote_thread, NULL);

    // ========= main input loop =========
    if (g_tui_enabled) {
        tui_add_line("SYS", "", "Welcome! Use UP/DOWN to scroll messages. Type !help.", SYSTEM);
        if (g_ui_mode == UI_GRAVEMIND) gravemind_boot_lines();
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
                        fprintf(stderr, "Detected an interrupt; shutting down gracefully\n");
                        break;
                    }
                    clearerr(stdin); // clear interrupted state
                    continue;
                }

                if (feof(stdin)) {
                    fprintf(stderr, "Encountered EOF\n");
                } else {
                    fprintf(stderr, "getline error: %s\n", strerror(errno));
                }
                break;
            }

            // strip newline
            line[strcspn(line, "\n")] = 0;

            // local commands
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
                fprintf(stderr, "Error: Attempting to send non-ascii character\n");
                flag = 1;
            }
            if (strlen(send.message) > 1023) {
                fprintf(stderr, "Error: Message is too long to send\n");
                flag = 1;
            }
            if (strlen(send.message) == 0) {
                fprintf(stderr, "Error: Message is too short to send\n");
                flag = 1;
            }

            if (!flag) {
                if (write(settings.socket_fd, &send, sizeof(send)) <= 0) {
                    fprintf(stderr, "Encountered a write error [%s]\n", strerror(errno));
                    break;
                }
            }
        }

        // FREE EXACTLY ONCE (fixes your double free)
        free(line);
        line = NULL;
    }

    // ========= logout + cleanup =========
    settings.running = false;

    message_t logout = {0};
    logout.m_type = htonl(LOGOUT);
    strncpy(logout.username, settings.username, sizeof(logout.username) - 1);
    logout.username[sizeof(logout.username) - 1] = 0;
    strncpy(logout.message, "User has disconnected from server", sizeof(logout.message) - 1);
    logout.message[sizeof(logout.message) - 1] = 0;

    // best-effort logout
    (void)write(settings.socket_fd, &logout, sizeof(logout));

    shutdown(settings.socket_fd, SHUT_RDWR);
    close(settings.socket_fd);
    settings.socket_fd = -1;

    pthread_join(reading, NULL);
    pthread_join(grv_quotes, NULL);

    tui_raw_disable();
    printf("\nBye!\n");
    return 0;
}

