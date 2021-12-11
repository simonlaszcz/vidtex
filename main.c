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
#include <sys/types.h>
#include <unistd.h>
#include "bedstead.h"
#include "decoder.h"

#define vt_perror() fprintf(stderr, \
    "Error: %s (%d)\n\tat %s line %d\n", strerror(errno), errno, __FILE__, __LINE__)
#define RCFILE ".vidtexrc"
#define USAGE "Usage:\n"
#define BUFFER_LEN 1024
#define MAX_AMBLE_LEN 10
#define SLOW_SLEEP_MUS 50000

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
    bool slow_mode;
    FILE *dump_file;
    struct vt_rc_entry **rc_data;
    int rc_data_count;
    struct vt_rc_entry *selected_rc;
    char *host;
    char *port;
    int socket_fd;
    struct vt_decoder_state decoder_state;
};

static void vt_terminate(int signal);
static void vt_cleanup(struct vt_session_state *session);
static int vt_get_rc(struct vt_rc_entry ***rc_data, int *rc_data_count);
static char *vt_duplicate_token(char *token);
static int vt_scan_array(char *token, int buffer[]);
static void vt_free_rc(struct vt_rc_entry **rc_data, int rc_data_count);
static int vt_parse_options(int argc, char *argv[], struct vt_session_state *session);
static int vt_connect(struct vt_session_state *session);
static bool vt_is_valid_fd(int fd);
static int vt_wait_for_data(int socket_fd, int timeout);

volatile sig_atomic_t terminate_received = false;
volatile sig_atomic_t socket_closed = false;
struct vt_session_state session;

int 
main(int argc, char *argv[])
{
    memset(&session, 0, sizeof(struct vt_session_state));
    session.socket_fd = -1;

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

    if (vt_get_rc(&session.rc_data, &session.rc_data_count) != EXIT_SUCCESS) {
        goto abend;
    }

    if (vt_parse_options(argc, argv, &session) != EXIT_SUCCESS) {
        goto abend;
    }

    if (vt_connect(&session) != EXIT_SUCCESS) {
        goto abend;
    }

    setlocale(LC_ALL, "");
    initscr();
    vt_decoder_init(&session.decoder_state);
    cbreak();
    nodelay(stdscr, true);
    noecho();

    uint8_t buffer[BUFFER_LEN];
    int read_max = session.slow_mode ? 2 : BUFFER_LEN;;

    while (!(terminate_received || socket_closed)) {
        int sz = vt_wait_for_data(session.socket_fd, 100);

        if (sz > 0) {
            sz = read(session.socket_fd, buffer, read_max);

            if (sz > 0) {
                vt_decode(&session.decoder_state, buffer, sz);

                if (session.dump_file != NULL) {
                    fwrite(buffer, sizeof(uint8_t), sz, session.dump_file);
                }

                if (session.slow_mode) {
                    usleep(SLOW_SLEEP_MUS);
                }
            }
        }

        int ch = getch();

        if (ch != EOF) {
            sz = write(session.socket_fd, &ch, 1);

            if (sz < 1) {
                socket_closed = true;
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
        
        if (session->selected_rc->postamble_length > 0) {
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

    if (session->dump_file != NULL) {
        if (fclose(session->dump_file) == -1) {
            vt_perror();
        }
    }

    if (session->decoder_state.trace_file != NULL) {
        if (fclose(session->decoder_state.trace_file) == -1) {
            vt_perror();
        }
    }

    if (session->rc_data_count > 0) {
        vt_free_rc(session->rc_data, session->rc_data_count);
    }

    //  These may have been freed via session->rc_data
    if (session->host != NULL) {
        free(session->host);
    }

    if (session->port != NULL) {
        free(session->port);
    }
}

static int 
vt_get_rc(struct vt_rc_entry ***rc_data, int *rc_data_count)
{
    *rc_data = NULL;
    *rc_data_count = 0;
    FILE *fin = fopen(RCFILE, "rt");

    if (fin == NULL) {
        return EXIT_SUCCESS;
    }

    char buffer[BUFFER_LEN];
    int count = 0;
    struct vt_rc_entry **root = NULL;
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
    if (root != NULL) {
        vt_free_rc(root, count);
    }
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
vt_free_rc(struct vt_rc_entry **rc, int count)
{
}

static int
vt_parse_options(int argc, char *argv[], struct vt_session_state *session)
{
    int optrv = 0;
    int optidx = 0;
    int rcidx = 0;
    struct option long_options[] = {
        {"host", required_argument, 0, 0},
        {"port", required_argument, 0, 0},
        {"dump", required_argument, 0, 0},
        {"index", required_argument, 0, 0},
        {"cursor", no_argument, 0, 0},
        {"slow", no_argument, 0, 0},
        {"markup", no_argument, 0, 0},
        {"mono", no_argument, 0, 0},
        {"trace", required_argument, 0, 0},
        {0, 0, 0, 0}
    };

	while ((optrv = getopt_long(argc, argv, "", long_options, &optidx)) != -1) {
		switch (optrv) {
        case 0:
            switch (optidx) {
            case 0:
                session->host = optarg;
                break;
            case 1:
                session->port = optarg;
                break;
            case 2:
                if (session->dump_file != NULL) {
                    fprintf(stderr, USAGE);
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
                    rcidx = atoi(optarg);

                    if (rcidx > -1 && rcidx < session->rc_data_count) {
                        session->selected_rc = session->rc_data[rcidx];
                        session->host = (*session->selected_rc).host;
                        session->port = (*session->selected_rc).port;
                    }
                    else {
                        fprintf(stderr, "Index %d not found in %s\n", rcidx, RCFILE);
                        goto abend;
                    }
                }
                else {
                    fprintf(stderr, "%s is empty\n", RCFILE);
                    goto abend;
                }
                break;
            case 4:
                session->decoder_state.force_cursor = true;
                break;
            case 5:
                session->slow_mode = true;
                break;
            case 6:
                session->decoder_state.markup_mode = true;
                break;
            case 7:
                session->decoder_state.mono_mode = true;
                break;
            case 8:
                if (session->decoder_state.trace_file != NULL) {
                    fprintf(stderr, USAGE);
                    goto abend;
                }
                session->decoder_state.trace_file = fopen(optarg, "wt");
                if (session->decoder_state.trace_file == NULL) {
                    vt_perror();
                    goto abend;
                }
                setbuf(session->decoder_state.trace_file, NULL);
                break;
            }
            break;
        case '?':
            fprintf(stderr, USAGE);
            goto abend;
        }
    }

    if (session->host == NULL || session->port == NULL) {
        fprintf(stderr, USAGE);
        goto abend;
    }

    return EXIT_SUCCESS;

abend:
    return EXIT_FAILURE;
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

//TODO: use rc preamble

    int preamble[4] = {22, 255, 253, 3};
    int sz = write(session->socket_fd, preamble, 4);

    if (sz < 1) {
        if (sz == 0) {
            fprintf(stderr, "Failed to write preamble\n");
        }
        else if (sz == -1) {
            vt_perror();
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
vt_wait_for_data(int socket_fd, int timeout)
{
    struct pollfd recv_poll_data = {.fd = socket_fd, .events = POLLIN};
    return poll(&recv_poll_data, 1, timeout);
}
