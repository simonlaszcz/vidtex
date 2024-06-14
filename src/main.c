#include <arpa/inet.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <ncursesw/curses.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "bedstead.h"
#include "decoder.h"
#include "telesoft.h"
#include "log.h"
#include "rc.h"

#ifdef VERSION
const char *version = VERSION;
#else
const char *version = "0.0.0";
#endif

#define vt_is_ctrl(x)       ((x) & 0x1F)
#define KEY_REVEAL          'r'
#define KEY_DOWNLOAD        'g'
#define KEY_BOLD            'b'
#define KEY_SAVE_FRAME      'f'
#define IO_BUFFER_LEN       (2048)
#define POLL_PERIOD_MS      (-1)
#define TIMESTR_MAX         (15)

struct vt_session_state
{
    struct vt_rc_state rc_state;
    struct vt_rc_entry *selected_rc;
    struct vt_decoder_state decoder_state;
    struct vt_tele_state tele_state;
    bool show_menu;
    bool show_help;
    bool show_version;
    FILE *load_file;
    // either from command line or shortcut to selected rc
    char *host;
    char *port;
    FILE *dump_file;
    int socket_fd;
    int flash_timer_fd;
    int download_fd;
};

static void vt_cleanup(void);
static void vt_terminate(int signal);
static int vt_parse_options(int argc, char *argv[], struct vt_session_state *session);
static int vt_show_file(struct vt_session_state *state);
static int vt_connect(struct vt_session_state *session);
static bool vt_is_valid_fd(int fd);
static int vt_transform_input(int ch);
static void vt_usage(void);
static void vt_version(void);
static void vt_trace(struct vt_session_state *session, char *format, ...);
static void vt_save(struct vt_session_state *session);

volatile sig_atomic_t terminate_received = false;
volatile sig_atomic_t socket_closed = false;
struct vt_session_state session;

int 
main(int argc, char *argv[])
{
    memset(&session, 0, sizeof(struct vt_session_state));
    session.socket_fd = -1;
    session.flash_timer_fd = -1;
    session.download_fd = -1;
    atexit(vt_cleanup);

    struct sigaction new_action;
    new_action.sa_handler = vt_terminate;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    if (sigaction(SIGINT, &new_action, NULL) == -1) {
        log_err();
        goto abend;
    }
    if (sigaction(SIGTERM, &new_action, NULL) == -1) {
        log_err();
        goto abend;
    }
    if (sigaction(SIGQUIT, &new_action, NULL) == -1) {
        log_err();
        goto abend;
    }
    if (sigaction(SIGHUP, &new_action, NULL) == -1) {
        log_err();
        goto abend;
    }
    if (sigaction(SIGPIPE, &new_action, NULL) == -1) {
        log_err();
        goto abend;
    }

    if (vt_rc_load(&session.rc_state) != EXIT_SUCCESS) {
        goto abend;
    }
    if (vt_parse_options(argc, argv, &session) != EXIT_SUCCESS) {
        goto abend;
    }

    if (session.show_help) {
        vt_usage();
        exit(0);
    }
    if (session.show_version) {
        vt_version();
        exit(0);
    }
    if (session.load_file != NULL) {
        exit(vt_show_file(&session));
    }

    if (session.show_menu) {
        session.selected_rc = vt_rc_show_menu(&session.rc_state);

        if (session.selected_rc != NULL) {
            session.host = session.selected_rc->host;
            session.port = session.selected_rc->port;
        }
        else {
            fprintf(stderr, "No configuration found\n");
            goto abend;
        }
    }

    if (session.host == NULL || session.port == NULL) {
        vt_usage();
        goto abend;
    }

    if (vt_connect(&session) != EXIT_SUCCESS) {
        goto abend;
    }

    session.flash_timer_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    if (session.flash_timer_fd == -1) {
        log_err();
        goto abend;
    }

    struct itimerspec flash_time = {{1, 0}, {1, 0}};
    if (timerfd_settime(session.flash_timer_fd, TFD_TIMER_ABSTIME, &flash_time, NULL) == -1) {
        log_err();
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
            log_err();
            goto abend;
        }

        if (prv < 1) {
            continue;
        }

        if (poll_data[0].revents & POLLIN) {
            int nread = read(session.socket_fd, buffer, IO_BUFFER_LEN);

            if (nread > 0) {
                if (session.dump_file != NULL) {
                    fwrite(buffer, sizeof(uint8_t), nread, session.dump_file);
                }

                vt_decoder_decode(&session.decoder_state, buffer, nread);

                if (!is_downloading) {
                    can_download = vt_tele_decode_header(&session.tele_state, buffer, nread);
                }
                else {
                    vt_tele_decode(&session.tele_state, buffer, nread, session.download_fd);

                    if (session.tele_state.end_of_file || session.tele_state.end_of_frame) {
                        if (session.tele_state.end_of_file) {
                            if (close(session.download_fd) == -1) {
                                log_err();
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

        if (poll_data[1].revents & POLLIN) {
            int ch = vt_transform_input(getch());

            switch (ch) {
            case EOF:
                break;
            case vt_is_ctrl(KEY_REVEAL):
                vt_decoder_toggle_reveal(&session.decoder_state);
                break;
            case vt_is_ctrl(KEY_DOWNLOAD):
                if (can_download) {
                    is_downloading = true;
                    session.download_fd 
                        = open(session.tele_state.filename, O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU);

                    if (session.download_fd == -1) {
                        log_err();
                        goto abend;
                    }

                    if (write(session.socket_fd, &more, 1) < 1) {
                        socket_closed = true;
                    }
                }
                break;
            case vt_is_ctrl(KEY_SAVE_FRAME):
                vt_save(&session);
                break;
            case vt_is_ctrl(KEY_BOLD):
                session.decoder_state.bold_mode = !session.decoder_state.bold_mode;
                break;
            default:
                if (write(session.socket_fd, &ch, 1) < 1) {
                    socket_closed = true;
                }
                break;
            }
        }

        if (poll_data[2].revents & POLLIN) {
            uint64_t elapsed = 0;
            if (read(session.flash_timer_fd, &elapsed, sizeof(uint64_t)) > 0) {
                vt_decoder_toggle_flash(&session.decoder_state);
            }
        }
    }

    if (socket_closed) {
        printf("Connection closed by host\n");
    }
    printf("Session terminated\nGoodbye\n");
    return 0;
abend:
    return 1;
}

static void 
vt_cleanup(void)
{
    endwin();

    if (session.socket_fd > -1) {
        uint8_t default_buffer[4] = {'*', '9', '0', '_'};
        uint8_t *buffer = default_buffer;
        int len = 4;
        
        if (session.selected_rc != NULL && session.selected_rc->postamble_length > 0) {
            buffer = session.selected_rc->postamble;
            len = session.selected_rc->postamble_length;
        }

        //  If write fails it was closed by the host first
        if (write(session.socket_fd, buffer, len) == len) {
            vt_trace(&session, "postamble: ");
            for (int i = 0; i < len; ++i) {
                vt_trace(&session, "%d '%c' ", buffer[i], buffer[i]);
            }
            vt_trace(&session, "\n");

            if (shutdown(session.socket_fd, SHUT_RDWR) == -1) {
                log_err();
            }

            if (close(session.socket_fd) == -1) {
                log_err();
            }
        }
    }

    if (session.flash_timer_fd > -1) {
        if (close(session.flash_timer_fd) == -1) {
            log_err();
        }
    }

    if (session.dump_file != NULL) {
        if (fclose(session.dump_file) == -1) {
            log_err();
        }
    }

    if (session.download_fd > -1) {
        if (close(session.download_fd) == -1) {
            log_err();
        }
    }

    if (session.decoder_state.trace_file != NULL) {
        if (fclose(session.decoder_state.trace_file) == -1) {
            log_err();
        }
    }

    vt_rc_free(&session.rc_state);

    if (session.selected_rc == NULL) {
        //  Otherwise they'll be freed by vt_rc_free()
        free(session.host);
        free(session.port);
    }

    if (session.load_file != NULL) {
        fclose(session.load_file);
    }
}

static void
vt_terminate(int signal)
{
    terminate_received = true;

    if (signal == SIGPIPE) {
        socket_closed = true;
    }
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
        {"mono", no_argument, 0, 0},
        {"trace", required_argument, 0, 0},
        {"bold", no_argument, 0, 0},
        {"galax", no_argument, 0, 0},
        {"file", required_argument, 0, 0},
        {"help", no_argument, 0, 0},
        {"version", no_argument, 0, 0},
        {0, 0, 0, 0}
    };

    while ((optrv = getopt_long(argc, argv, "", long_options, &optidx)) != -1) {
        switch (optrv) {
        case 0:
            switch (optidx) {
            case 0:
                session->host = vt_rc_duplicate_token(optarg);
                break;
            case 1:
                session->port = vt_rc_duplicate_token(optarg);
                break;
            case 2:
                if (session->dump_file != NULL) {
                    vt_usage();
                    goto abend;
                }
                session->dump_file = fopen(optarg, "wb");
                if (session->dump_file == NULL) {
                    log_err();
                    goto abend;
                }
                setbuf(session->dump_file, NULL);
                break;
            case 3:
                session->show_menu = true;
                break;
            case 4:
                session->decoder_state.mono_mode = true;
                break;
            case 5:
                if (session->decoder_state.trace_file != NULL) {
                    vt_usage();
                    goto abend;
                }
                session->decoder_state.trace_file = fopen(optarg, "wt");
                if (session->decoder_state.trace_file == NULL) {
                    log_err();
                    goto abend;
                }
                setbuf(session->decoder_state.trace_file, NULL);
                break;
            case 6:
                session->decoder_state.bold_mode = true;
                break;
            case 7:
                session->decoder_state.map_char = &gal_map_char;
                break;
            case 8:
                session->load_file = fopen(optarg, "rb");
                if (session->load_file == NULL) {
                    log_err();
                    goto abend;
                }
                break;
            case 9:
                session->show_help = true;
                break;
            case 10:
                session->show_version = true;
                break;
            }
            break;
        case '?':
            vt_usage();
            goto abend;
        }
    }

    return EXIT_SUCCESS;
abend:
    return EXIT_FAILURE;
}

static int
vt_show_file(struct vt_session_state *state)
{
    state->flash_timer_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    if (state->flash_timer_fd == -1) {
        log_err();
        goto abend;
    }

    struct itimerspec flash_time = {{1, 0}, {1, 0}};
    if (timerfd_settime(state->flash_timer_fd, TFD_TIMER_ABSTIME, &flash_time, NULL) == -1) {
        log_err();
        goto abend;
    }

    setlocale(LC_ALL, "");
    initscr();
    if (state->decoder_state.map_char == NULL) {
        state->decoder_state.map_char = &bed_map_char;
    }
    vt_decoder_init(&state->decoder_state);
    state->decoder_state.win = stdscr;
    cbreak();
    nodelay(state->decoder_state.win, true);
    noecho();
    keypad(state->decoder_state.win, true);

    uint8_t buffer[IO_BUFFER_LEN];
    ssize_t nread = 0;
    while ((nread = fread(buffer, sizeof(uint8_t), IO_BUFFER_LEN, state->load_file)) > 0) {
        vt_decoder_decode(&state->decoder_state, buffer, nread);
    }

    struct pollfd poll_data[2] = {
        {.fd = STDIN_FILENO, .events = POLLIN},
        {.fd = state->flash_timer_fd, .events = POLLIN}
    };

    while (!terminate_received) {
        int prv = poll(poll_data, 2, POLL_PERIOD_MS);

        if (prv == -1 && errno != EINTR) {
            log_err();
            goto abend;
        }

        if (prv < 1) {
            continue;
        }

        if (poll_data[0].revents & POLLIN) {
            int ch = vt_transform_input(getch());

            switch (ch) {
            case vt_is_ctrl(KEY_REVEAL):
                vt_decoder_toggle_reveal(&state->decoder_state);
                break;
            default:
                break;
            }
        }

        if (poll_data[1].revents & POLLIN) {
            uint64_t elapsed = 0;
            if (read(state->flash_timer_fd, &elapsed, sizeof(uint64_t)) > 0) {
                vt_decoder_toggle_flash(&state->decoder_state);
            }
        }
    }

    return 0;
abend:
    return 1;
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

    uint8_t preamble[MAX_AMBLE_LEN + 1] = {0};
    preamble[0] = 22;
    int preamble_len = 1;

    if (session->selected_rc != NULL && session->selected_rc->preamble_length > 0) {
        preamble_len += session->selected_rc->preamble_length;

        for (int i = 0; i < session->selected_rc->preamble_length; ++i) {
            preamble[i + 1] = session->selected_rc->preamble[i];
        }
    }

    int sz = write(session->socket_fd, preamble, preamble_len);

    if (sz != preamble_len) {
        if (sz == -1) {
            log_err();
        }
        else {
            fprintf(stderr, "Failed to write preamble\n");
        }
        goto abend;
    }

    vt_trace(session, "preamble: ");
    for (int i = 0; i < preamble_len; ++i) {
        vt_trace(session, "%d '%c' ", preamble[i], preamble[i]);
    }
    vt_trace(session, "\n");

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
vt_usage(void)
{
    printf("Version: %s\n", version);
    printf("Usage: vidtex [options]\nOptions:\n");
    printf("%-16s\tOutput bold brighter colours\n", "--bold");
    printf("%-16s\tDump all bytes read from host to file\n", "--dump filename");
    printf("%-16s\tLoad and display a saved frame\n", "--file filename");
    printf("%-16s\tOutput char codes for Mode7 font\n", "--galax");
    printf("%-16s\tShow this help\n", "--help");
    printf("%-16s\tViewdata service host\n", "--host name");
    printf("%-16s\tCreate menu from vidtexrc\n", "--menu");
    printf("%-16s\tMonochrome display\n", "--mono");
    printf("%-16s\tViewdata service host port\n", "--port number");
    printf("%-16s\tWrite trace to file\n", "--trace filename");
    printf("%-16s\tPrint the version number\n", "--version");
}

static void
vt_version(void)
{
    printf("%s\n", version);
}

static void
vt_trace(struct vt_session_state *session, char *format, ...)
{
    if (session->decoder_state.trace_file != NULL) {
        va_list args;
        va_start(args, format);
        vfprintf(session->decoder_state.trace_file, format, args);
        va_end(args);
    }
}

static
void vt_save(struct vt_session_state *session)
{
    char timestr[TIMESTR_MAX] = {0};
    char filename[FILENAME_MAX] = {0};
    time_t ticks;
    struct tm *localt;
    FILE *fout = NULL;
    bool have_hostname = session->selected_rc != NULL && session->selected_rc->name != NULL;

    time(&ticks);
    localt = localtime(&ticks);
    strftime(timestr, TIMESTR_MAX, "%Y%m%d%H%M%S", localt);

    snprintf(filename, FILENAME_MAX, "%s/%s%s%s.frame", 
        session->rc_state.cwd != NULL ? session->rc_state.cwd : session->rc_state.home, 
        have_hostname ? session->selected_rc->name : "",
        have_hostname ? "_" : "",
        timestr);

    if ((fout = fopen(filename, "w")) == NULL) {
        log_err();
        return;
    }

    vt_decoder_save(&session->decoder_state, fout);
    fclose(fout);
}
