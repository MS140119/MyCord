
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ===================== PROTOCOL ===================== */

enum {
    LOGIN        = 0,
    LOGOUT       = 1,
    MESSAGE_SEND = 2,
    MESSAGE_RECV = 10,
    DISCONNECT   = 12,
    SYSTEM       = 13
};

typedef struct __attribute__((packed)) {
    uint32_t m_type;     // network order
    uint32_t timeStamp;  // network order
    char username[32];
    char message[1024];
} message_t;

/* ===================== SETTINGS ===================== */

typedef struct {
    struct sockaddr_in server;
    bool quiet;
    int socket_fd;
    volatile sig_atomic_t running;
    char username[32];
} settings_t;

static settings_t settings;

static volatile sig_atomic_t shutdown_requested = 0; // SIGINT/SIGTERM
static volatile sig_atomic_t got_disconnect = 0;      // server DISCONNECT (do NOT LOGOUT)

static const char *COLOR_RED   = "\033[31m";
static const char *COLOR_GRAY  = "\033[90m";
static const char *COLOR_RESET = "\033[0m";

/* ===================== HELP ===================== */

static void help_menu(void) {
    printf("usage: ./client [-h] [--port PORT] [--ip IP] [--domain DOMAIN] [--quiet]\n\n");
    printf("mycord client\n\n");
    printf("options:\n");
    printf("  --help                show this help message and exit\n");
    printf("  --port PORT           port to connect to (default: 8080)\n");
    printf("  --ip IP               IP to connect to (default: \"127.0.0.1\")\n");
    printf("  --domain DOMAIN       Domain name to connect to (if domain is specified, IP must not be)\n");
    printf("  --quiet               do not perform alerts or mention highlighting\n\n");
    printf("examples:\n");
    printf("  ./client --help (prints the above message)\n");
    printf("  ./client --port 1738 (connects to a mycord server at 127.0.0.1:1738)\n");
    printf("  ./client --domain example.com (connects to a mycord server at example.com:8080)\n");
}

/* ===================== UTIL ===================== */

static void eprintf(const char *msg) {
    fprintf(stderr, "Error: %s\n", msg);
}

static int full_write(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static ssize_t full_read(int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, p + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break; // EOF
        off += (size_t)r;
    }
    return (ssize_t)off;
}

static int username_valid(const char *u) {
    if (!u || !u[0]) return 0;
    for (size_t i = 0; u[i]; i++) {
        unsigned char c = (unsigned char)u[i];
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

static int msg_valid(const char *s) {
    // must be 1..1023 chars, printable ASCII only, no newlines (we strip newline before calling)
    size_t L = strlen(s);
    if (L == 0 || L > 1023) return 0;
    for (size_t i = 0; i < L; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126) return 0;
    }
    return 1;
}

/* ===================== HIGHLIGHTING ===================== */

static void print_with_mentions(const char *msg, const char *username) {
    char needle[64];
    snprintf(needle, sizeof(needle), "@%s", username);
    size_t nlen = strlen(needle);

    const char *p = msg;
    while (1) {
        const char *m = strstr(p, needle);
        if (!m) {
            fputs(p, stdout);
            return;
        }
        fwrite(p, 1, (size_t)(m - p), stdout);
        fputc('\a', stdout);
        fputs(COLOR_RED, stdout);
        fputs(needle, stdout);
        fputs(COLOR_RESET, stdout);
        p = m + nlen;
    }
}

/* ===================== SIGNALS ===================== */

static void handle_signal(int sig) {
    (void)sig;
    shutdown_requested = 1;
    settings.running = 0;
}

/* ===================== ARG PARSING ===================== */

static int process_args(int argc, char *argv[]) {
    bool ip_set = false;
    bool domain_set = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            help_menu();
            exit(0);
        } else if (!strcmp(argv[i], "--quiet")) {
            settings.quiet = true;
        } else if (!strcmp(argv[i], "--port")) {
            if (i + 1 >= argc) { eprintf("--port requires a value"); return -1; }
            int port = atoi(argv[i + 1]);
            if (port <= 0 || port > 65535) { eprintf("invalid port"); return -1; }
            settings.server.sin_port = htons((uint16_t)port);
            i++;
        } else if (!strcmp(argv[i], "--ip")) {
            if (i + 1 >= argc) { eprintf("--ip requires a value"); return -1; }
            if (domain_set) { eprintf("cannot use --ip and --domain together"); return -1; }
            if (inet_pton(AF_INET, argv[i + 1], &settings.server.sin_addr) != 1) {
                eprintf("invalid IPv4 address");
                return -1;
            }
            settings.server.sin_family = AF_INET;
            ip_set = true;
            i++;
        } else if (!strcmp(argv[i], "--domain")) {
            if (i + 1 >= argc) { eprintf("--domain requires a value"); return -1; }
            if (ip_set) { eprintf("cannot use --ip and --domain together"); return -1; }

            struct hostent *h = gethostbyname(argv[i + 1]);
            if (!h || h->h_addrtype != AF_INET || !h->h_addr_list[0]) {
                eprintf("DNS lookup failed (no IPv4 found)");
                return -1;
            }
            memcpy(&settings.server.sin_addr, h->h_addr_list[0], sizeof(struct in_addr));
            settings.server.sin_family = AF_INET;
            domain_set = true;
            i++;
        } else {
            eprintf("unknown argument (use --help)");
            return -1;
        }
    }
    return 0;
}

/* ===================== USERNAME ===================== */

static int get_username(void) {
    settings.username[0] = 0;

    FILE *fp = popen("whoami", "r");
    if (fp) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            buf[strcspn(buf, "\n")] = 0;
            strncpy(settings.username, buf, sizeof(settings.username) - 1);
        }
        pclose(fp);
    }

    if (!settings.username[0]) {
        const char *u = getenv("USER");
        if (u && *u) strncpy(settings.username, u, sizeof(settings.username) - 1);
    }

    settings.username[sizeof(settings.username) - 1] = 0;

    if (!username_valid(settings.username)) {
        eprintf("invalid username (must be non-empty and alphanumeric / ._-)");
        return -1;
    }
    return 0;
}

/* ===================== RECEIVE THREAD ===================== */

static void *receive_thread(void *arg) {
    (void)arg;

    message_t msg;
    while (settings.running) {
        ssize_t r = full_read(settings.socket_fd, &msg, sizeof(msg));
        if (r < 0) {
            if (errno == EINTR) continue;
            eprintf("read from server failed");
            settings.running = 0;
            break;
        }
        if (r == 0) {
            settings.running = 0;
            break;
        }
        if (r != (ssize_t)sizeof(msg)) {
            eprintf("protocol short read");
            settings.running = 0;
            break;
        }

        msg.username[31] = 0;
        msg.message[1023] = 0;

        uint32_t mt = ntohl(msg.m_type);

        if (mt == MESSAGE_RECV) {
            time_t t = (time_t)ntohl(msg.timeStamp);
            struct tm *info = localtime(&t);
            char tb[64];
            strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", info);

            printf("[%s] %s: ", tb, msg.username);
            if (!settings.quiet) print_with_mentions(msg.message, settings.username);
            else fputs(msg.message, stdout);
            putchar('\n');
            fflush(stdout);
        } else if (mt == SYSTEM) {
            // all gray: [SYSTEM] message
            fputs(COLOR_GRAY, stdout);
            fputs("[SYSTEM] ", stdout);
            fputs(msg.message, stdout);
            fputs(COLOR_RESET, stdout);
            fputc('\n', stdout);
            fflush(stdout);
        } else if (mt == DISCONNECT) {
            // all red: [DISCONNECT] reason
            fputs(COLOR_RED, stdout);
            fputs("[DISCONNECT] ", stdout);
            fputs(msg.message, stdout);
            fputs(COLOR_RESET, stdout);
            fputc('\n', stdout);
            fflush(stdout);

            got_disconnect = 1;
            settings.running = 0;
            break;
        }
    }

    return NULL;
}

/* ===================== MAIN ===================== */

int main(int argc, char *argv[]) {
    memset(&settings, 0, sizeof(settings));
    settings.server.sin_family = AF_INET;
    settings.server.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &settings.server.sin_addr);

    // SIGINT + SIGTERM
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        eprintf("sigaction failed");
        return 1;
    }

    if (get_username() != 0) return 1;
    if (process_args(argc, argv) != 0) return 1;

    settings.socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (settings.socket_fd < 0) {
        eprintf("socket() failed");
        return 1;
    }

    if (connect(settings.socket_fd, (struct sockaddr *)&settings.server, sizeof(settings.server)) != 0) {
        eprintf("connect() failed");
        close(settings.socket_fd);
        return 1;
    }

    settings.running = 1;

    // LOGIN (type + username only)
    message_t login;
    memset(&login, 0, sizeof(login));
    login.m_type = htonl(LOGIN);
    strncpy(login.username, settings.username, sizeof(login.username) - 1);

    if (full_write(settings.socket_fd, &login, sizeof(login)) != 0) {
        eprintf("failed to send LOGIN");
        close(settings.socket_fd);
        return 1;
    }

    pthread_t rx;
    if (pthread_create(&rx, NULL, receive_thread, NULL) != 0) {
        eprintf("pthread_create failed");
        close(settings.socket_fd);
        return 1;
    }

    printf("Type '!disconnect' (or !disconect) to disconnect\n");

    // STDIN loop
    char *line = NULL;
    size_t cap = 0;

    while (settings.running && !got_disconnect) {
        errno = 0;
        ssize_t nread = getline(&line, &cap, stdin);

        if (nread < 0) {
            if (errno == EINTR) {
                if (shutdown_requested) break;
                clearerr(stdin);
                continue;
            }
            // EOF or real error -> graceful logout path below
            break;
        }

        line[strcspn(line, "\n")] = 0;

        if (!strcmp(line, "!disconnect") || !strcmp(line, "!disconect")) {
            break;
        }

        if (!msg_valid(line)) {
            // never send invalid message (prevents DISCONNECT)
            if (strlen(line) == 0) eprintf("message too short");
            else if (strlen(line) > 1023) eprintf("message too long");
            else eprintf("message contains non-printable ASCII");
            continue;
        }

        message_t out;
        memset(&out, 0, sizeof(out));
        out.m_type = htonl(MESSAGE_SEND);
        strncpy(out.message, line, sizeof(out.message) - 1);

        if (full_write(settings.socket_fd, &out, sizeof(out)) != 0) {
            eprintf("write() failed");
            break;
        }
    }

    settings.running = 0;

    // LOGOUT only if server did NOT DISCONNECT us
    if (!got_disconnect) {
        message_t logout;
        memset(&logout, 0, sizeof(logout));
        logout.m_type = htonl(LOGOUT);
        (void)full_write(settings.socket_fd, &logout, sizeof(logout));
    }

    shutdown(settings.socket_fd, SHUT_RDWR);
    close(settings.socket_fd);

    pthread_join(rx, NULL);

    free(line);
    line = NULL;

    printf("Bye!\n");
    return 0;
}

