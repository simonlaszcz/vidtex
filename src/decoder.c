#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include "decoder.h"
#include "log.h"

#define UNPRINTABLE_DUMP_SUB    '~'

static void vt_new_frame(struct vt_decoder_state *state);
static void vt_next_row(struct vt_decoder_state *state);
static void vt_fill_end(struct vt_decoder_state *state);
static void vt_reset_flags(struct vt_decoder_state *state);
static void vt_set_attr(struct vt_decoder_state *state, struct vt_decoder_attr *attr);
static short vt_get_color_pair_number(enum vt_decoder_color fg, enum vt_decoder_color bg);
static void vt_apply_after_flags(struct vt_decoder_state *state);
static void vt_reset_after_flags(struct vt_decoder_state *state);
static void vt_get_char_code(struct vt_decoder_state *state, 
    bool is_alpha, bool is_contiguous, int row_code, int col_code, struct vt_decoder_char *ch);
static void vt_put_char(struct vt_decoder_state *state, 
    int row, int col, wchar_t ch, struct vt_decoder_attr *attr);
static void vt_init_colors(void);
static void vt_trace(struct vt_decoder_state *state, char *format, ...);
static void vt_dump(struct vt_decoder_state *state, uint8_t *buffer, int count);
void vt_move_cursor(struct vt_decoder_state *state);

void 
vt_decoder_init(struct vt_decoder_state *state)
{
    if (has_colors()) {
        start_color();

        if (COLOR_PAIRS >= 64) {
            vt_init_colors();
        }
    }

    curs_set(0);
    state->flags.is_cursor_on = false;
    vt_new_frame(state);
    vt_get_char_code(state, true, false, 0, 2, &state->space);
    wrefresh(state->win);
}

void
vt_decoder_save(struct vt_decoder_state *state, FILE *fout)
{
    if (state->frame_buffer_offset > 0) {
        size_t n = fwrite(state->frame_buffer, sizeof(uint8_t), state->frame_buffer_offset, fout);

        if (n < (size_t)state->frame_buffer_offset) {
            log_err();
        }
    }
}

void 
vt_decoder_decode(struct vt_decoder_state *state, uint8_t *buffer, int count)
{
    if (state->trace_file != NULL) {
        vt_dump(state, buffer, count);
    }

    //  n.b. After evaluating chars, we 'continue' if it shouldn't be displayed.
    //  'break' if it should
    for (int bidx = 0; bidx < count; ++bidx) {
        uint8_t b = buffer[bidx];

        if (state->frame_buffer_offset < FRAME_BUFFER_MAX) {
            state->frame_buffer[state->frame_buffer_offset++] = b;
        }

        //  ASCII control codes (codes < 32)
        //  Rows are either 40 chars long exactly or less than 40 chars and 
        //  terminated by CRLF or CR or LF
        switch (b) {
        case 0:     //  NULL
            vt_trace(state, "NULL");
            continue;
        case 8:     //  Backspace
            --state->col;

            if (state->col < 0) {
                state->col = MAX_COLS - 1;
                --state->row;

                if (state->row < 0) {
                    state->row = MAX_ROWS - 1;
                }
            }

            vt_move_cursor(state);
            vt_trace(state, "BS");
            continue;
        case 9:     //  h-tab
            ++state->col;

            if (state->col >= MAX_COLS) {
                state->col = 0;
                ++state->row;

                if (state->row >= MAX_ROWS) {
                    state->row = 0;
                }
            }

            vt_move_cursor(state);
            vt_trace(state, "H-TAB");
            continue;
        case 10:    //  LF
            ++state->row;

            if (state->row >= MAX_ROWS) {
                state->row = 0;
            }

            vt_reset_flags(state);
            vt_reset_after_flags(state);
            vt_move_cursor(state);
            vt_trace(state, "LF (new row)");
            continue;
        case 11:    //  v-tab
            --state->row;

            if (state->row < 0) {
                state->row = MAX_ROWS - 1;
            }

            vt_move_cursor(state);
            vt_trace(state, "V-TAB");
            continue;
        case 12:
            //  FF (new frame/clear screen)
            vt_new_frame(state);
            vt_move_cursor(state);
            vt_trace(state, "FF (new frame)");
            continue;
        case 13:    //  CR
            vt_fill_end(state);
            state->col = 0;
            vt_move_cursor(state);
            vt_trace(state, "CR (fill to end)");
            continue;
        case 17:    //  DC1 - cursor on
            curs_set(1);
            state->flags.is_cursor_on = true;
            vt_move_cursor(state);
            vt_trace(state, "DC1 (cursor on)");
            continue;
        case 20:    //  DC4 - cursor off
            curs_set(0);
            state->flags.is_cursor_on = false;
            vt_move_cursor(state);
            vt_trace(state, "DC4 (cursor off)");
            continue;
        case 30:    //  RS  - back to origin
            vt_fill_end(state);
            state->col = 0;
            state->row = 0;
            vt_move_cursor(state);
            vt_trace(state, "RS (fill to end, back to origin)");
            continue;
        }

        int row_code = (b & 0xF);
        int col_code = (b & 0x70) >> 4;

        if (state->flags.is_escaped) {
            //  we're only interested in the first bit
            col_code &= 1;
            state->flags.is_escaped = false;
        }            

        if (col_code == 0) {
            switch (row_code) {
            case 0:     //  NUL (alpha black at level 2.5+)
            case 14:    //  Shift Out
            case 15:    //  Shift In
                vt_trace(state, "either alpha-black or shift in/out (ignored)");
                break;
            case 8: //  Flash
                state->after_flags.is_flashing = true;
                vt_trace(state, "flash=true (set-after)");
                break;
            case 9: //  Steady
                state->flags.is_flashing = false;
                vt_trace(state, "flash=false (set-immediate)");
                break;
            case 10:    //  end box
                state->after_flags.is_boxing = false;
                vt_trace(state, "boxing=false (set-after)");
                break;
            case 11:    //  start box 
                state->after_flags.is_boxing = true;
                vt_trace(state, "boxing=true (set-after)");
                break;
            case 12:    //  normal height
                state->flags.is_double_height = false;
                state->flags.held_mosaic = state->space;
                vt_trace(state, "double-height=false, held-mosaic=' ' (set-immediate)");
                break;
            case 13:    
                //  double height (but not on last row) and not if we're on the
                //  lower half of a double height row
                if (state->row < (MAX_ROWS - 2) && state->row != state->dheight_low_row) {
                    state->after_flags.is_double_height = true;
                    vt_trace(state, "double-height=true (set-after)");
                }
                break;
            default:
                state->after_flags.alpha_fg_color = row_code;
                vt_trace(state, "alpha-fg=%d (set-after)", row_code);
                break;
            }
        }
        else if (col_code == 1) {
            switch (row_code) {
            case 0:     //  Data Link Escape (graphics black at level 2.5+)
                vt_trace(state, "DLE (ignored)");
                break;
            case 8:     //  conceal display
                state->flags.is_concealed = true;
                vt_trace(state, "is-concealed=true (set-immediate)");
                break;
            case 9:
                state->flags.is_contiguous = true;
                vt_trace(state, "is-contiguous=true (set-immediate)");
                break;
            case 10:
                state->flags.is_contiguous = false;
                vt_trace(state, "is-contiguous=false (set-immediate)");
                break;
            case 11:    //  escape - do not print
                state->flags.is_escaped = true;
                vt_trace(state, "is-escaped=true (set-immediate)");
                continue;
            case 12:    //  black bg
                state->flags.bg_color = BLACK;
                vt_trace(state, "bg-color=BLACK (set-immediate)");
                break;
            case 13:    //  new bg, i.e. use the fg for the bg
                state->flags.bg_color = state->flags.is_alpha ? 
                    state->flags.alpha_fg_color : state->flags.mosaic_fg_color;
                vt_trace(state, "bg-color=%d (set-immediate)", state->flags.bg_color);
                break;
            case 14:    //  hold graphics
                state->flags.is_mosaic_held = true;
                vt_trace(state, "is-mosaic-held=true (set-immediate)");
                break;
            case 15:    //  release graphics
                state->after_flags.is_mosaic_held = false;
                vt_trace(state, "is-mosaic-held=false (set-after)");
                break;
            default:
                state->after_flags.mosaic_fg_color = row_code;
                vt_trace(state, "mosaic-fg=%d (set-after)", state->after_flags.mosaic_fg_color);
                break;
            }
        }

        if (state->row != state->dheight_low_row) {
            struct vt_decoder_attr attr;
            vt_set_attr(state, &attr);
            struct vt_decoder_char ch;

            if (col_code == 0 || col_code == 1) {
                ch = state->flags.is_mosaic_held ? state->flags.held_mosaic : state->space;

                if (state->flags.is_double_height) {
                    vt_trace(state, "%lc %04x (double height row upper half spacing character or held mosaic)", ch.upper, ch.upper);
                    vt_put_char(state, state->row, state->col, ch.upper, &attr);
                }
                else {
                    vt_trace(state, "%lc %04x (spacing character or held mosaic)", ch.single, ch.single);
                    vt_put_char(state, state->row, state->col, ch.single, &attr);
                }

                if (state->row == 0) {
                    state->header_row[state->col] = SPACE;
                }
            }
            else {
                vt_get_char_code(state, state->flags.is_alpha, 
                    state->flags.is_contiguous, row_code, col_code, &ch);

                if (state->flags.is_double_height) {
                    vt_trace(state, "%lc %04x (double height row upper half)", ch.upper, ch.upper);
                    vt_put_char(state, state->row, state->col, ch.upper, &attr);
                }
                else {
                    vt_trace(state, "%lc %04x", ch.single, ch.single);
                    vt_put_char(state, state->row, state->col, ch.single, &attr);
                }

                if (!state->flags.is_alpha) {
                    state->flags.held_mosaic = ch;
                    vt_trace(state, "held-mosaic (single-height)='%lc'", state->flags.held_mosaic.single);
                }

                if (state->row == 0) {
                    state->header_row[state->col] = ch.single;
                }
            }

            if (state->flags.is_double_height) {
                vt_trace(state, "%lc %04x (double height row lower half @ row %d)", ch.lower, ch.lower, state->dheight_low_row);
                vt_put_char(state, state->dheight_low_row, state->col, ch.lower, &attr);
            }
        }

        vt_apply_after_flags(state);

        if (state->after_flags.is_double_height == TRI_TRUE) {
            state->dheight_low_row = state->row + 1;
            vt_trace(state, "row %d will be treated as the lower half of a double height row", state->dheight_low_row);
        }

        vt_reset_after_flags(state);
        ++state->col;

        //  Automatically start a new row if we've got a full row
        if (state->col == MAX_COLS) {
            vt_next_row(state);
        }

        wmove(state->win, state->row, state->col);
        wrefresh(state->win);
    }
}

void
vt_move_cursor(struct vt_decoder_state *state)
{
    wmove(state->win, state->row, state->col);

    if (state->flags.is_cursor_on) {
        wrefresh(state->win);
    }
}

void 
vt_decoder_toggle_flash(struct vt_decoder_state *state)
{
    bool needs_refresh = false;
    int curs = curs_set(0);
    state->screen_flash_state = !state->screen_flash_state;

    if (curs) {
        wrefresh(state->win);
    }

    for (int r = 0; r < MAX_ROWS; ++r) {
        for (int c = 0; c < MAX_COLS; ++c) {
            struct vt_decoder_cell *cell = &state->cells[r][c];

            if (cell->attr.has_flash) {
                vt_put_char(state, r, c, cell->character, &cell->attr);
                needs_refresh = true;
            }
        }
    }

    curs_set(curs);

    if (needs_refresh || curs) {
        //  restore cursor position
        wmove(state->win, state->row, state->col);
        wrefresh(state->win);
    }
}

void 
vt_decoder_toggle_reveal(struct vt_decoder_state *state)
{
    bool needs_refresh = false;
    int curs = curs_set(0);
    state->screen_revealed_state = !state->screen_revealed_state;

    if (curs) {
        wrefresh(state->win);
    }

    for (int r = 0; r < MAX_ROWS; ++r) {
        for (int c = 0; c < MAX_COLS; ++c) {
            struct vt_decoder_cell *cell = &state->cells[r][c];

            if (cell->attr.has_concealed) {
                vt_put_char(state, r, c, cell->character, &cell->attr);
                needs_refresh = true;
            }
        }
    }

    curs_set(curs);

    if (needs_refresh || curs) {
        //  restore cursor position
        wmove(state->win, state->row, state->col);
        wrefresh(state->win);
    }
}

static void 
vt_new_frame(struct vt_decoder_state *state)
{
    state->row = 0;
    state->col = 0;
    state->dheight_low_row = -1;
    state->frame_buffer_offset = 0;
    state->screen_revealed_state = false;
    vt_reset_flags(state);
    vt_reset_after_flags(state);
    memset(state->cells, 0, sizeof(state->cells));

    for (int i = 0; i < MAX_COLS; ++i) {
        state->header_row[i] = SPACE;
    }

    struct vt_decoder_attr attr;
    memset(&attr, 0, sizeof(struct vt_decoder_attr));

    for (int r = 0; r < MAX_ROWS; ++r) {
        for (int c = 0; c < MAX_COLS; ++c) {
            state->cells[r][c].character = WSPACE;
            vt_put_char(state, r, c, WSPACE, &attr);
        }
    }
}

static void 
vt_next_row(struct vt_decoder_state *state)
{
    if ((state->row + 1) < MAX_ROWS) {
        ++state->row;
    }
    else {
        state->row = 0;
    }
    
    state->col = 0;
    vt_reset_flags(state);
    vt_reset_after_flags(state);
}

static void 
vt_fill_end(struct vt_decoder_state *state)
{
    if (state->col > 0) {
        struct vt_decoder_cell *prev = &state->cells[state->row][state->col - 1];
        struct vt_decoder_attr attr;
        memset(&attr, 0, sizeof(struct vt_decoder_attr));
        attr.attr = prev->attr.attr;
        attr.color_pair = prev->attr.color_pair;

        for (int col = state->col; col < MAX_COLS; ++col) {
            wchar_t ch = state->cells[state->row][col].character;
            vt_trace(state, "%lc %04x (end fill)", ch, ch);
            vt_put_char(state, state->row, col, ch, &attr);
        }
    }
}

static void 
vt_apply_after_flags(struct vt_decoder_state *state)
{
    struct vt_decoder_after_flags *after = &state->after_flags;
    struct vt_decoder_flags *flags = &state->flags;
    bool was_alpha = flags->is_alpha;

    if (after->alpha_fg_color != NONE) {
        flags->alpha_fg_color = after->alpha_fg_color;
        flags->is_alpha = true;
        flags->is_concealed = false;
    }
    else if (after->mosaic_fg_color != NONE) {
        flags->mosaic_fg_color = after->mosaic_fg_color;
        flags->is_alpha = false;
        flags->is_concealed = false;
    }

    if (flags->is_alpha != was_alpha) {
        flags->held_mosaic = state->space;
    }

    if (after->is_flashing == TRI_TRUE) {
        flags->is_flashing = true;
    }

    if (after->is_boxing != TRI_UNDEF) {
        flags->is_boxing = after->is_boxing == TRI_TRUE;
    }

    if (after->is_mosaic_held == TRI_FALSE) {
        flags->is_mosaic_held = false;
    }

    if (after->is_double_height == TRI_TRUE) {
        flags->is_double_height = true;
        vt_trace(state, "now double height");
    }
}

static void 
vt_reset_flags(struct vt_decoder_state *state)
{
    state->flags.bg_color = BLACK;
    state->flags.alpha_fg_color = WHITE;
    state->flags.mosaic_fg_color = WHITE;
    state->flags.is_alpha = true;
    state->flags.is_flashing = false;
    state->flags.is_escaped = false;
    state->flags.is_boxing = false;
    state->flags.is_concealed = false;
    state->flags.is_contiguous = true;
    state->flags.is_mosaic_held = false;
    state->flags.held_mosaic = state->space;
    state->flags.is_double_height = false;
    //  leave cursor as is
}

static void 
vt_reset_after_flags(struct vt_decoder_state *state)
{
    state->after_flags.alpha_fg_color = NONE;
    state->after_flags.mosaic_fg_color = NONE;
    state->after_flags.is_flashing = TRI_UNDEF;
    state->after_flags.is_boxing = TRI_UNDEF;
    state->after_flags.is_mosaic_held = TRI_UNDEF;
    state->after_flags.is_double_height = TRI_UNDEF;
}

static void 
vt_set_attr(struct vt_decoder_state *state, struct vt_decoder_attr *attr)
{
    memset(attr, 0, sizeof(struct vt_decoder_attr));
    attr->attr = state->bold_mode ? A_BOLD : 0;
    attr->has_flash = state->flags.is_flashing;
    attr->has_concealed = state->flags.is_concealed;

    if (has_colors()) {
        enum vt_decoder_color fg = state->flags.is_alpha ? 
            state->flags.alpha_fg_color : state->flags.mosaic_fg_color;
        attr->color_pair = vt_get_color_pair_number(fg, state->flags.bg_color);
    }
}

static short 
vt_get_color_pair_number(enum vt_decoder_color fg, enum vt_decoder_color bg)
{
    if (fg == COLOR_WHITE && bg == COLOR_BLACK) {
        return 0;
    }

    //  Never redefine color pair 0 (white on black)
    //  Use 7 bits at most
    return (fg << 3) + bg;
}

static void 
vt_get_char_code(
    struct vt_decoder_state *state, 
    bool is_alpha, bool is_contiguous, 
    int row_code, int col_code, 
    struct vt_decoder_char *ch)
{
    ch->single = state->map_char(row_code, col_code, is_alpha, is_contiguous, false, false);
    ch->upper = state->map_char(row_code, col_code, is_alpha, is_contiguous, true, false);
    ch->lower = state->map_char(row_code, col_code, is_alpha, is_contiguous, true, true);
}

static void
vt_put_char(struct vt_decoder_state *state, int row, int col, wchar_t ch, struct vt_decoder_attr *attr)
{
    struct vt_decoder_cell *cell = &state->cells[row][col];
    short display_color = state->mono_mode ? 0 : attr->color_pair;
    wchar_t display_ch = ch;
    attr_t display_attr = attr->attr;

    if (attr->has_concealed && !state->screen_revealed_state) {
        display_ch = WSPACE;
    }

    if (attr->has_flash && !state->screen_flash_state) {
        display_ch = WSPACE;
    }

    wchar_t vchar[2] = {display_ch, L'\0'};
    cchar_t cc;
    setcchar(&cc, vchar, display_attr, display_color, 0);
    mvwadd_wch(state->win, row, col, &cc);

    cell->attr = *attr;
    cell->character = ch;
}

static void 
vt_init_colors(void)
{
    for (int fg = 0 ; fg < 8; ++fg) {
        for (int bg = 0; bg < 8; ++bg) {
            //  White on black is always pair number 0
            //  It's standard and doesn't need to be initialized
            if (!(fg == COLOR_WHITE && bg == COLOR_BLACK)) {
                init_pair(vt_get_color_pair_number(fg, bg), fg, bg);
            }
        }
    }
}

static void
vt_trace(struct vt_decoder_state *state, char *format, ...)
{
    int cy = 0;
    int cx = 0;

    if (state->trace_file != NULL) {
        getyx(state->win, cy, cx);
        fprintf(state->trace_file, "%02d,%02d (%02d,%02d)\t", state->row, state->col, cy, cx);
        va_list args;
        va_start(args, format);
        vfprintf(state->trace_file, format, args);
        va_end(args);
        fprintf(state->trace_file, "\n");
    }
}

static void 
vt_dump(struct vt_decoder_state *state, uint8_t *buffer, int count)
{
    fprintf(state->trace_file, "\n>>>>>>>>>>\n");
    fprintf(state->trace_file, "received %d bytes. control character='%c'.\n", count, UNPRINTABLE_DUMP_SUB);

    int ridx = 0;
    while (ridx < count) {
        int cidx = 0;
        while (ridx < count && cidx < 80) {
            fprintf(state->trace_file, "%c", isprint(buffer[ridx]) ? buffer[ridx] : UNPRINTABLE_DUMP_SUB);
            ++cidx;
            ++ridx;
        }
        fprintf(state->trace_file, "\n");
    }

    ridx = 0;
    while (ridx < count) {
        int cidx = 0;
        while (ridx < count && cidx < 25) {
            fprintf(state->trace_file, "%02x ", buffer[ridx]);
            ++cidx;
            ++ridx;
        }
        fprintf(state->trace_file, "\n");
    }
    fprintf(state->trace_file, "<<<<<<<<<<\n\n");
}
