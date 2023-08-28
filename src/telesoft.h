#ifndef TELESOFT_H
#define TELESOFT_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#define CHAR_THREE_QUARTERS (0b1111101)
#define CHAR_SPACE          (0b0100000)
#define CHAR_BAR            (0b1111100)

struct vt_tele_state
{
    int state;
    int frame_number;
    int running_checksum;
    int checksum;
    char frame_letter;
    int shift_offset;
    char control_code;
    bool ignore;
    bool in_frame;
    char filename[FILENAME_MAX + 1];
    int filename_len;
    bool end_of_file;
    bool invalid_checksum;
    bool end_of_frame;
    bool parity_error;
};

void vt_tele_reset(struct vt_tele_state *state);
bool vt_tele_decode_header(struct vt_tele_state *state, uint8_t *buffer, int count);
void vt_tele_decode(struct vt_tele_state *state, uint8_t *buffer, int count, int fd_out);

#endif
