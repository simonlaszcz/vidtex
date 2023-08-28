#ifndef RC_H
#define RC_H

#define RCFILE              "vidtexrc"
#define MAX_AMBLE_LEN       (10)

struct vt_rc_entry
{
    char *name;
    char *host;
    char *port;
    uint8_t preamble[MAX_AMBLE_LEN];
    int preamble_length;
    uint8_t postamble[MAX_AMBLE_LEN];
    int postamble_length;
};

struct vt_rc_state
{
    // config loaded from file
    struct vt_rc_entry **rc_data;
    int rc_data_count;
    // home directory. set when rc is loaded
    char *home;
    // current working directory. set when rc is loaded
    char *cwd;
};

int vt_rc_load(struct vt_rc_state *state);
void vt_rc_free(struct vt_rc_state *state);
struct vt_rc_entry *vt_rc_show_menu(struct vt_rc_state *state);
char *vt_rc_duplicate_token(char *token);

#endif
