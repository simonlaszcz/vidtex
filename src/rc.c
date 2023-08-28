#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <inttypes.h>
#include "rc.h"
#include "log.h"

#ifdef SYSCONFDIR
const char *sys_conf_dir = SYSCONFDIR;
#else
const char *sys_conf_dir = NULL;
#endif

#define BUFFER_LEN          (1024)

static int vt_get_rc(const char *dir, struct vt_rc_entry ***rc_data, int *rc_data_count);
static int vt_scan_array(char *token, uint8_t buffer[]);
static void vt_free_rc(struct vt_rc_entry *rc[], int count);

int 
vt_rc_load(struct vt_rc_state *state)
{
    if (sys_conf_dir != NULL) {
        if (vt_get_rc(sys_conf_dir, &state->rc_data, &state->rc_data_count) != EXIT_SUCCESS) {
            goto abend;
        }
    }

    state->home = getenv("HOME");
    if (state->home != NULL) {
        if (vt_get_rc(state->home, &state->rc_data, &state->rc_data_count) != EXIT_SUCCESS) {
            goto abend;
        }
    }

    state->cwd = getcwd(NULL, 0);
    if (state->cwd != NULL) {
        if (strncmp(state->home, state->cwd, FILENAME_MAX) != 0) {
            if (vt_get_rc(state->cwd, &state->rc_data, &state->rc_data_count) != EXIT_SUCCESS) {
                goto abend;
            }
        }
    }

    return EXIT_SUCCESS;
abend:
    return EXIT_FAILURE;
}

void 
vt_rc_free(struct vt_rc_state *state)
{
    if (state->rc_data_count > 0) {
        vt_free_rc(state->rc_data, state->rc_data_count);
    }

    if (state->cwd != NULL) {
        free(state->cwd);
    }
}

char *
vt_rc_duplicate_token(char *token)
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
        log_err();
    }

    return copy;
}


struct vt_rc_entry *
vt_rc_show_menu(struct vt_rc_state *state)
{
    if (state->rc_data_count < 0) {
        return NULL;
    }

    printf("%3s %-20s\n", "#", "Name");

    for (int i = 0; i < state->rc_data_count; ++i) {
        struct vt_rc_entry *e = state->rc_data[i];
        printf("%3d %-20s\n", i, e->name);
    }

    int choice = -1;

    while (choice < 0 || choice >= state->rc_data_count) {
        printf("? ");
        scanf("%d", &choice);

        if (choice == EOF) {
            return NULL;
        }
    }

    return state->rc_data[choice];
}

static int 
vt_get_rc(const char *dir, struct vt_rc_entry ***rc_data, int *rc_data_count)
{
    char path_buffer[BUFFER_LEN] = {0};
    int sz = snprintf(path_buffer, BUFFER_LEN, "%s/%s", dir, RCFILE);

    if (sz < 0) {
        log_err();
        return EXIT_FAILURE;
    }
    else if (sz >= BUFFER_LEN) {
        errno = EOVERFLOW;
        log_err();
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
            log_err();
            goto abend;
        }

        char *token = strtok(sin, "\t\n,|");
        int field_num = 0;

        while (token != NULL) {
            switch (++field_num) {
            case 1:
                entry->name = vt_rc_duplicate_token(token);
                if (entry->name == NULL) {
                    fprintf(stderr, "No name specified at line %d\n", count + 1);
                    goto abend;
                }
                break;
            case 2:
                entry->host = vt_rc_duplicate_token(token);
                if (entry->host == NULL) {
                    fprintf(stderr, "No host specified at line %d\n", count + 1);
                    goto abend;
                }
                break;
            case 3:
                entry->port = vt_rc_duplicate_token(token);
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
            log_err();
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

static int
vt_scan_array(char *token, uint8_t buffer[])
{
    int count = sscanf(token, 
        "%"SCNu8 " %"SCNu8 " %"SCNu8 " %"SCNu8 " %"SCNu8 " %"SCNu8 " %"SCNu8 " %"SCNu8 " %"SCNu8 " %"SCNu8,
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
