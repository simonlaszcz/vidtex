#include "telesoft.h"

static int vt_parity(int v);

void
vt_tele_reset(struct vt_tele_state *state)
{
    memset(state, 0, sizeof(struct vt_tele_state));
}

bool
vt_tele_decode_header(struct vt_tele_state *state, uint8_t *buffer, int count)
{
    vt_tele_decode(state, buffer, count, -1);

    /*
    If we have a state->filename and the frame number is 1, 
    then we assume that we have correctly decoded a header
    If we return true, the caller can proceed with the download by 
    sending the name frame/packet
    If false, the caller should send the next packet and test again
    */
    return state->frame_number == 1 && state->filename != NULL;
}

void
vt_tele_decode(struct vt_tele_state *state, uint8_t *buffer, int count, int fd_out)
{
    char c;
    state->end_of_frame = false;
    state->parity_error = false;

    for (int bidx = 0; bidx < count; ++bidx) {
        int b = buffer[bidx];

        if (state->in_frame && vt_parity(b) != (b >> 7)) {
            state->parity_error = true;
        }

        //  Discard parity bit
        b &= 0b01111111;

        if (b == CHAR_BAR) {
            //  Next char will be a control code
            state->state = 1;
            continue;
        }

        if (state->in_frame && state->state == 0) {
            state->running_checksum ^= b;

            if (!state->ignore) {
                //  The first frame is the header
                if (state->frame_number == 1) {
                    if (state->control_code == 'I') {
                        state->filename[state->filename_len++] = b;    
                    }
                }
                else {
                    if (b == CHAR_THREE_QUARTERS) {
                        c = CHAR_SPACE + state->shift_offset;
                    }
                    else {
                        c = b + state->shift_offset;
                    }

                    if (fd_out != -1) {
                        write(fd_out, &c, 1);
                    }
                }
            }
        }
        else if (state->state >= 1) {
            if (state->state == 1) {
                state->control_code = b;

                if (state->control_code != 'A' && state->control_code != 'Z') {
                    state->running_checksum ^= 124;
                    state->running_checksum ^= b;
                }
            }

            switch (state->control_code) {
            case '0':
                state->shift_offset = 0;
                state->state = 0;
                continue;
            case '1':
                state->shift_offset = -64;
                state->state = 0;
                continue;
            case '2':
                state->shift_offset = 64;
                state->state = 0;
                continue;
            case '3':
                state->shift_offset = 96;
                state->state = 0;
                continue;
            case '4':
                state->shift_offset = 128;
                state->state = 0;
                continue;
            case '5':
                state->shift_offset = 160;
                state->state = 0;
                continue;
            case CHAR_THREE_QUARTERS:
                c = CHAR_THREE_QUARTERS + state->shift_offset;
                if (fd_out != -1) {
                    write(fd_out, &c, 1);
                }
                state->state = 0;
                continue;
            case 'A':   // start frame
                ++state->frame_number;
                state->running_checksum = 0;
                state->state = 0;
                state->in_frame = true;
                continue;
            case 'D':   // start of data section
                state->state = 0;
                continue;
            case 'E':   // CHAR_BAR char
                c = CHAR_BAR + state->shift_offset;
                if (fd_out != -1) {
                    write(fd_out, &c, 1);
                }
                state->state = 0;
                continue;
            case 'F':   // end of file
                state->end_of_file = true;
                state->state = 0;
                state->in_frame = false;
                continue;
            case 'G':   // frame letter
                switch (state->state) {
                case 1:
                    ++state->state;
                    break;
                case 2:
                    state->running_checksum ^= b;
                    state->frame_letter = b;
                    state->state = 0;
                    //  We may now get the data block number (0..9) 
                    //  and the number of the last data block in the frame but 
                    //  we ignore them
                    state->ignore = true;
                    break;
                }
                continue;
            case 'I':   // escape sequence terminator
                state->ignore = false;
                state->state = 0;
                continue;
            case 'L':   // end of line
                state->state = 0;

                //  Ignore frame count in the header
                if (state->frame_number == 1) {
                    state->ignore = true;
                }
                else {
                    c = 13;
                    if (fd_out != -1) {
                        write(fd_out, &c, 1);
                    }
                }
                continue;
            case 'T':   // start of header section
                state->state = 0;
                continue;
            case 'Z':   // end of frame
                //  The next 3 bytes after Z store the expected checksum
                switch (state->state) {
                    case 1:
                        state->checksum = 0;
                        ++state->state;
                        state->in_frame = false;
                        break;
                    case 2:
                        state->checksum += (b - 48) * 100;
                        ++state->state;
                        break;
                    case 3:
                        state->checksum += (b - 48) * 10;
                        ++state->state;
                        break;
                    case 4:
                        state->checksum += b - 48;

                        if (state->checksum != state->running_checksum) {
                            state->invalid_checksum = true;
                        }

                        state->state = 0;
                        state->end_of_frame = true;
                        break;
                }
                continue;
            default:
                //  Unrecognized. Ignore everything until |I is found
                state->ignore = true;
                state->state = 0;
                continue;
            }
        }            
    }
}

static int
vt_parity(int v)
{
    int count = (v & 1) + (v >> 1 & 1) + (v >> 2 & 1) + (v >> 3 & 1) + 
        (v >> 4 & 1) + (v >> 5 & 1) + (v >> 6 & 1);

    return count % 2;
}
