#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <ncursesw/curses.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>
#include "bedstead.h"
#include "decoder.h"
#include "telesoft.h"

#define vt_perror() fprintf(stderr, \
    "Error: %s (%d)\n\tat %s line %d\n", strerror(errno), errno, __FILE__, __LINE__)
#define vt_is_ctrl(x) ((x) & 0x1F)
#define RCFILE ".vidtexrc"
#define IO_BUFFER_LEN 2048
#define BUFFER_LEN 1024
#define MAX_AMBLE_LEN 10
#define POLL_PERIOD_MS -1

struct vt_rc_entry
{
    char *name;
    char *host;
    char *port;
    int preamble[MAX_AMBLE_LEN];
    int preamble_length;
    int postamble[MAX_AMBLE_LEN];
    int postamble_length;
};

struct vt_session_state
{
    FILE *dump_file;
    struct vt_rc_entry **rc_data;
    int rc_data_count;
    struct vt_rc_entry *selected_rc;
    char *host;
    char *port;
    int socket_fd;
    struct vt_decoder_state decoder_state;
    char *cwd;
    int flash_timer_fd;
    struct vt_tele_state tele_state;
    int download_fd;
};

static void vt_terminate(int signal);
static void vt_cleanup(struct vt_session_state *session);
static int vt_get_rc(const char *dir, struct vt_rc_entry ***rc_data, int *rc_data_count);
static char *vt_duplicate_token(char *token);
static int vt_scan_array(char *token, int buffer[]);
static void vt_free_rc(struct vt_rc_entry *rc_data[], int rc_data_count);
static int vt_parse_options(int argc, char *argv[], struct vt_session_state *session);
static struct vt_rc_entry *vt_show_rc_menu(struct vt_rc_entry **rc_data, int rc_data_count);
static int vt_connect(struct vt_session_state *session);
static bool vt_is_valid_fd(int fd);
static int vt_transform_input(int ch);
static void vt_usage();

volatile sig_atomic_t terminate_received = false;
volatile sig_atomic_t socket_closed = false;
struct vt_session_state session;

int 
main(int argc, char *argv[])
{
    memset(&session, 0, sizeof(struct vt_session_state));
    session.socket_fd = -1;
    session.flash_timer_fd = -1;

    struct sigaction new_action;
    new_action.sa_handler = vt_terminate;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    if (sigaction(SIGINT, &new_action, NULL) == -1) {
        vt_perror();
        goto abend;
    }
    if (sigaction(SIGTERM, &new_action, NULL) == -1) {
        vt_perror();
        goto abend;
    }
    if (sigaction(SIGQUIT, &new_action, NULL) == -1) {
        vt_perror();
        goto abend;
    }
    if (sigaction(SIGHUP, &new_action, NULL) == -1) {
        vt_perror();
        goto abend;
    }
    if (sigaction(SIGPIPE, &new_action, NULL) == -1) {
        vt_perror();
        goto abend;
    }

    char *home = getenv("HOME");
    if (home != NULL) {
        if (vt_get_rc(home, &session.rc_data, &session.rc_data_count) != EXIT_SUCCESS) {
            goto abend;
        }
    }

    session.cwd = getcwd(NULL, 0);
    if (session.cwd != NULL) {
        if (strcmp(home, session.cwd) != 0) {
            if (vt_get_rc(session.cwd, &session.rc_data, &session.rc_data_count) != EXIT_SUCCESS) {
                goto abend;
            }
        }
    }

    if (vt_parse_options(argc, argv, &session) != EXIT_SUCCESS) {
        goto abend;
    }

    if (vt_connect(&session) != EXIT_SUCCESS) {
        goto abend;
    }

    session.flash_timer_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    if (session.flash_timer_fd == -1) {
        vt_perror();
        goto abend;
    }

    struct itimerspec flash_time = {{1, 0}, {1, 0}};
    if (timerfd_settime(session.flash_timer_fd, TFD_TIMER_ABSTIME, &flash_time, NULL) == -1) {
        vt_perror();
        goto abend;
    }

    setlocale(LC_ALL, "");
    initscr();
    if (session.decoder_state.map_char == NULL) {
        session.decoder_state.map_char = &bed_map_char;
    }
    vt_decoder_init(&session.decoder_state);
    vt_tele_reset(&session.tele_state);
    session.decoder_state.win = stdscr;
    cbreak();
    nodelay(session.decoder_state.win, true);
    noecho();
    keypad(session.decoder_state.win, true);

    const char more = '_';
    bool can_download = false;
    bool is_downloading = false;
    uint8_t buffer[IO_BUFFER_LEN];
    struct pollfd poll_data[3] = {
        {.fd = session.socket_fd, .events = POLLIN},
        {.fd = STDIN_FILENO, .events = POLLIN},
        {.fd = session.flash_timer_fd, .events = POLLIN}
    };

    while (!(terminate_received || socket_closed)) {
        int prv = poll(poll_data, 3, POLL_PERIOD_MS);

        if (prv == -1 && errno != EINTR) {
            vt_perror();
            goto abend;
        }

        if (prv < 1) {
            continue;
        }

        if ((poll_data[0].revents & POLLIN) == POLLIN) {
            int nread = read(session.socket_fd, buffer, IO_BUFFER_LEN);

            if (nread > 0) {
                if (session.dump_file != NULL) {
                    fwrite(buffer, sizeof(uint8_t), nread, session.dump_file);
                }

                vt_decode(&session.decoder_state, buffer, nread);

                if (!is_downloading) {
                    can_download = vt_tele_decode_header(&session.tele_state, buffer, nread);
                }
                else {
                    vt_tele_decode(&session.tele_state, buffer, nread, session.download_fd);

                    if (session.tele_state.end_of_file || session.tele_state.end_of_frame) {
                        if (session.tele_state.end_of_file) {
                            if (close(session.download_fd) == -1) {
                                vt_perror();
                                goto abend;
                            }

                            is_downloading = false;
                            can_download = false;
                            vt_tele_reset(&session.tele_state);
                            session.download_fd = -1;
                        }

                        if (write(session.socket_fd, &more, 1) < 1) {
                            socket_closed = true;
                        }
                    }
                }
            }
        }

        if ((poll_data[1].revents & POLLIN) == POLLIN) {
            int ch = vt_transform_input(getch());

            switch (ch) {
            case EOF:
                break;
            case vt_is_ctrl('r'):
                vt_toggle_reveal(&session.decoder_state);
                break;
            case vt_is_ctrl('g'):
                if (can_download) {
                    is_downloading = true;
                    session.download_fd 
                        = open(session.tele_state.filename, O_CREAT | O_WRONLY | O_TRUNC, S_IRWXU);

                    if (session.download_fd == -1) {
                        vt_perror();
                        goto abend;
                    }

                    if (write(session.socket_fd, &more, 1) < 1) {
                        socket_closed = true;
                    }
                }
                break;
            default:
                if (write(session.socket_fd, &ch, 1) < 1) {
                    socket_closed = true;
                }
                break;
            }
        }

        if ((poll_data[2].revents & POLLIN) == POLLIN) {
            uint64_t elapsed = 0;
            if (read(session.flash_timer_fd, &elapsed, sizeof(uint64_t)) > 0) {
                vt_toggle_flash(&session.decoder_state);
            }
        }
    }

    vt_cleanup(&session);
    if (socket_closed) {
        printf("Connection closed by host\n");
    }
    printf("Session terminated\nGoodbye\n");
    exit(EXIT_SUCCESS);

abend:
    printf("Unexpected error. Session terminated\nGoodbye\n");
    vt_cleanup(&session);
    exit(EXIT_FAILURE);
}

static void
vt_terminate(int signal)
{
    terminate_received = true;

    if (signal == SIGPIPE) {
        socket_closed = true;
    }
}

static void 
vt_cleanup(struct vt_session_state *session)
{
    endwin();

    if (session->socket_fd > -1) {
        int default_buffer[4] = {'*', '9', '0', '_'};
        int *buffer = default_buffer;
        int len = 4;
        
        if (session->selected_rc != NULL && session->selected_rc->postamble_length > 0) {
            buffer = session->selected_rc->postamble;
            len = session->selected_rc->postamble_length;
        }

        //  If write fails it was closed by the host first
        if (write(session->socket_fd, buffer, len) == len) {
            if (shutdown(session->socket_fd, SHUT_RDWR) == -1) {
                vt_perror();
            }

            if (close(session->socket_fd) == -1) {
                vt_perror();
            }
        }
    }

    if (session->flash_timer_fd > -1) {
        if (close(session->flash_timer_fd) == -1) {
            vt_perror();
        }
    }

    if (session->dump_file != NULL) {
        if (fclose(session->dump_file) == -1) {
            vt_perror();
        }
    }

    if (session->download_fd > -1) {
        if (close(session->download_fd) == -1) {
            vt_perror();
        }
    }

    if (session->decoder_state.trace_file != NULL) {
        if (fclose(session->decoder_state.trace_file) == -1) {
            vt_perror();
        }
    }

    if (session->selected_rc == NULL) {
        //  Otherwise they'll be freed by vt_free_rc()
        if (session->host != NULL) {
            free(session->host);
        }

        if (session->port != NULL) {
            free(session->port);
        }
    }

    if (session->rc_data_count > 0) {
        vt_free_rc(session->rc_data, session->rc_data_count);
    }

    if (session->cwd != NULL) {
        free(session->cwd);
    }
}

static int 
vt_get_rc(const char *dir, struct vt_rc_entry ***rc_data, int *rc_data_count)
{
    char path_buffer[BUFFER_LEN] = {0};
    int sz = snprintf(path_buffer, BUFFER_LEN, "%s/%s", dir, RCFILE);

    if (sz < 0) {
        vt_perror();
        return EXIT_FAILURE;
    }
    else if (sz >= BUFFER_LEN) {
        errno = EOVERFLOW;
        vt_perror();
        return EXIT_FAILURE;
    }

    FILE *fin = fopen(path_buffer, "rt");

    if (fin == NULL) {
        return EXIT_SUCCESS;
    }

    char buffer[BUFFER_LEN];
    int count = *rc_data_count;
    struct vt_rc_entry **root = *rc_data;
    char *sin = fgets(buffer, BUFFER_LEN, fin);

    while (sin != NULL) {
        if (sin[0] == '#') {
            sin = fgets(buffer, BUFFER_LEN, fin);
            continue;
        }

        struct vt_rc_entry *entry = malloc(sizeof(struct vt_rc_entry));
        if (entry == NULL) {
            vt_perror();
            goto abend;
        }

        char *token = strtok(sin, "\t\n,|");
        int field_num = 0;

        while (token != NULL) {
            switch (++field_num) {
            case 1:
                entry->name = vt_duplicate_token(token);
                if (entry->name == NULL) {
                    fprintf(stderr, "No name specified at line %d\n", count + 1);
                    goto abend;
                }
                break;
            case 2:
                entry->host = vt_duplicate_token(token);
                if (entry->host == NULL) {
                    fprintf(stderr, "No host specified at line %d\n", count + 1);
                    goto abend;
                }
                break;
            case 3:
                entry->port = vt_duplicate_token(token);
                if (entry->port == NULL) {
                    fprintf(stderr, "No port specified at line %d\n", count + 1);
                    goto abend;
                }
                break;
            case 4: // optional
                entry->preamble_length = vt_scan_array(token, entry->preamble);
                break;
            case 5: // optional
                entry->postamble_length = vt_scan_array(token, entry->postamble);
                break;
            };
             
            token = strtok(NULL, "\t\n,|");
        }

        if (field_num < 3) {
            fprintf(stderr, "Too few fields at line %d\n", count + 1);
            goto abend;
        }

        if (root == NULL) {
            root = malloc(sizeof(struct vt_rc_entry));
        }
        else {
            root = realloc(root, sizeof(struct vt_rc_entry) * count);
        }

        if (root == NULL) {
            vt_perror();
            goto abend;
        }

        root[count++] = entry;
        sin = fgets(buffer, BUFFER_LEN, fin);
    }

    *rc_data = root;
    *rc_data_count = count;
    fclose(fin);
    return EXIT_SUCCESS;

abend:
    fprintf(stderr, "Errors found in configuration file\n");
    fclose(fin);
    return EXIT_FAILURE;
}

static char *
vt_duplicate_token(char *token)
{
    if (token == NULL) {
        return NULL;
    }

    int len = strlen(token);
    int idx = len - 1;

    while (idx > -1 && isspace(token[idx])) {
        token[idx--] = 0;
    }

    len = idx + 1;

    if (len == 0) {
        return NULL;
    }

    char *copy = strndup(token, len);

    if (copy == NULL) {
        vt_perror();
    }

    return copy;
}

static int
vt_scan_array(char *token, int buffer[])
{
    int count = sscanf(token, "%d %d %d %d %d %d %d %d %d %d", 
        &buffer[0], &buffer[1], &buffer[2], &buffer[3], &buffer[4],
        &buffer[5], &buffer[6], &buffer[7], &buffer[8], &buffer[9]);

    return count > 0 ? count : 0;
} 

static void 
vt_free_rc(struct vt_rc_entry *rc[], int count)
{
    for (int i = 0; i < count; ++i) {
        struct vt_rc_entry *e = rc[i];

        free(e->name);
        free(e->host);
        free(e->port);

        e->name = NULL;
        e->host = NULL;
        e->port = NULL;
    }

    free(rc);
}

static int
vt_parse_options(int argc, char *argv[], struct vt_session_state *session)
{
    int optrv = 0;
    int optidx = 0;
    struct option long_options[] = {
        {"host", required_argument, 0, 0},
        {"port", required_argument, 0, 0},
        {"dump", required_argument, 0, 0},
        {"menu", no_argument, 0, 0},
        {"cursor", no_argument, 0, 0},
        {"mono", no_argument, 0, 0},
        {"trace", required_argument, 0, 0},
        {"bold", no_argument, 0, 0},
        {"galax", no_argument, 0, 0},
        {0, 0, 0, 0}
    };

	while ((optrv = getopt_long(argc, argv, "", long_options, &optidx)) != -1) {
		switch (optrv) {
        case 0:
            switch (optidx) {
            case 0:
                session->host = vt_duplicate_token(optarg);
                break;
            case 1:
                session->port = vt_duplicate_token(optarg);
                break;
            case 2:
                if (session->dump_file != NULL) {
                    vt_usage();
                    goto abend;
                }
                session->dump_file = fopen(optarg, "wb");
                if (session->dump_file == NULL) {
                    vt_perror();
                    goto abend;
                }
                setbuf(session->dump_file, NULL);
                break;
            case 3:
                if (session->rc_data_count > 0) {
                    session->selected_rc = 
                        vt_show_rc_menu(session->rc_data, session->rc_data_count);

                    if (session->selected_rc != NULL) {
                        session->host = (*session->selected_rc).host;
                        session->port = (*session->selected_rc).port;
                    }
                }
                else {
                    fprintf(stderr, "No configuration found\n");
                    goto abend;
                }
                break;
            case 4:
                session->decoder_state.force_cursor = true;
                break;
            case 5:
                session->decoder_state.mono_mode = true;
                break;
            case 6:
                if (session->decoder_state.trace_file != NULL) {
                    vt_usage();
                    goto abend;
                }
                session->decoder_state.trace_file = fopen(optarg, "wt");
                if (session->decoder_state.trace_file == NULL) {
                    vt_perror();
                    goto abend;
                }
                setbuf(session->decoder_state.trace_file, NULL);
                break;
            case 7:
                session->decoder_state.bold_mode = true;
                break;
            case 8:
                session->decoder_state.map_char = &gal_map_char;
                break;
            }
            break;
        case '?':
            vt_usage();
            goto abend;
        }
    }

    if (session->host == NULL || session->port == NULL) {
        vt_usage();
        goto abend;
    }

    return EXIT_SUCCESS;

abend:
    return EXIT_FAILURE;
}

static struct vt_rc_entry *
vt_show_rc_menu(struct vt_rc_entry **rc_data, int rc_data_count)
{
    printf("%3s %-20s %-30s %-10s\n", "#", "Name", "Host", "Port");

    for (int i = 0; i < rc_data_count; ++i) {
        struct vt_rc_entry *e = rc_data[i];
        printf("%3d %-20s %-30s %-10s\n", i, e->name, e->host, e->port);
    }

    int choice = -1;

    while (choice < 0 || choice >= rc_data_count) {
        printf("? ");
        scanf("%d", &choice);

        if (choice == EOF) {
            return NULL;
        }
    }

    return rc_data[choice];
}

static int 
vt_connect(struct vt_session_state *session)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = 0;
    hints.ai_protocol = 0;

    struct addrinfo *result;
    int rv = getaddrinfo(session->host, session->port, &hints, &result);

    if (rv != 0) {
        goto abend;
    }

    session->socket_fd = -1;

    for (struct addrinfo *rp = result; rp != NULL; rp = rp->ai_next) {
        session->socket_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (session->socket_fd == -1) {
            continue;
        }
        if (connect(session->socket_fd, rp->ai_addr, rp->ai_addrlen) != -1) {
            break;
        }

        close(session->socket_fd);
    }

    freeaddrinfo(result);

    if (session->socket_fd == -1) {
        goto abend;
    }

    if (!vt_is_valid_fd(session->socket_fd)) {
        session->socket_fd = -1;
        goto abend;
    }

    int preamble[MAX_AMBLE_LEN + 1] = {0};
    preamble[0] = 22;
    int preamble_len = 1;

    if (session->selected_rc != NULL && session->selected_rc->preamble_length > 0) {
        int n = sizeof(int) * session->selected_rc->preamble_length;
        memcpy(&preamble[1], session->selected_rc->preamble, n);
        preamble_len += session->selected_rc->preamble_length;
    }

    int sz = write(session->socket_fd, preamble, preamble_len);

    if (sz != preamble_len) {
        if (sz == -1) {
            vt_perror();
        }
        else {
            fprintf(stderr, "Failed to write preamble\n");
        }
        goto abend;
    }

    return EXIT_SUCCESS;

abend:
    fprintf(stderr, "Failed to establish connection with host %s:%s\n", session->host, session->port);
    return EXIT_FAILURE;
}

static bool 
vt_is_valid_fd(int fd)
{
    int flags = fcntl(fd, F_GETFD);

    return !(flags == -1 || errno == EBADF);
}

static int
vt_transform_input(int ch)
{
    switch (ch) {
    case '#':
    case '\n':
        return '_';
    default:
        return ch;
    }
}

static void
vt_usage()
{
    fprintf(stderr, "Usage: vidtex [options]\nOptions:\n");
    fprintf(stderr, "\t%-10s\tViewdata service host\n", "--host");
    fprintf(stderr, "\t%-10s\tViewdata service host port\n", "--port");
    fprintf(stderr, "\t%-10s\tPresent the contents of .vidtexrc as a menu so that a host can be chosen\n", "--menu");
}
