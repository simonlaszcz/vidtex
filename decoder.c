#include <stdarg.h>
#include "decoder.h"

static void vt_decoder_new_frame(struct vt_decoder_state *state);
static void vt_decoder_next_row(struct vt_decoder_state *state);
static void vt_decoder_fill_end(struct vt_decoder_state *state);
static void vt_decoder_reset_flags(struct vt_decoder_state *state);
static void vt_set_attr(struct vt_decoder_state *state, struct vt_attr *attr);
static short vt_get_color_pair_number(enum vt_decoder_color fg, enum vt_decoder_color bg);
static void vt_decoder_apply_after_flags(struct vt_decoder_state *state);
static void vt_decoder_reset_after_flags(struct vt_decoder_state *state);
static void vt_get_char_code(struct vt_decoder_state *state, 
    bool is_alpha, bool is_contiguous, int row_code, int col_code, struct vt_char *ch);
static void vt_put_char(struct vt_decoder_state *state, 
    int row, int col, wchar_t ch, struct vt_attr *attr, bool trace);
static void vt_init_colors();
static void vt_trace(struct vt_decoder_state *state, char *format, ...);
static void vt_cursor(struct vt_decoder_state *state);

void 
vt_decoder_init(struct vt_decoder_state *state)
{
    if (has_colors()) {
        start_color();

        if (COLOR_PAIRS >= 64) {
            vt_init_colors();
        }

        vt_trace(state, "max_colors=%d\n", COLOR_PAIRS);
    }

    state->flags.is_cursor_on = false;
    curs_set(state->force_cursor);
    vt_decoder_new_frame(state);
    vt_get_char_code(state, true, false, 0, 2, &state->space);
}

void 
vt_decode(struct vt_decoder_state *state, uint8_t *buffer, int count)
{
    vt_trace(state, "Decoding %d bytes\n", count);

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
            vt_trace(state, "NULL\n");
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

            vt_trace(state, "BS: row=%d, col=%d\n", state->row, state->col);
            vt_cursor(state);
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

            vt_trace(state, "h-tab: row=%d, col=%d\n", state->row, state->col);
            vt_cursor(state);
            continue;
        case 10:    //  LF (end of row)
            vt_decoder_next_row(state);
            vt_trace(state, "LF: row=%d, col=%d\n", state->row, state->col);
            vt_cursor(state);
            continue;
        case 11:    //  v-tab
            --state->row;

            if (state->row < 0) {
                state->row = MAX_ROWS - 1;
            }

            vt_trace(state, "v-tab: row=%d, col=%d\n", state->row, state->col);
            vt_cursor(state);
            continue;
        case 12:
            //  FF (new frame/clear screen)
            vt_decoder_new_frame(state);
            vt_trace(state, "FF: row=%d, col=%d\n", state->row, state->col);
            vt_cursor(state);
            continue;
        case 13:    //  CR
            vt_decoder_fill_end(state);
            state->col = 0;
            vt_trace(state, "CR: row=%d, col=%d\n", state->row, state->col);
            vt_cursor(state);
            continue;
        case 17:    //  DC1 - cursor on
            state->flags.is_cursor_on = true;
            vt_trace(state, "DC1\n");
            continue;
        case 20:    //  DC4 - cursor off
            state->flags.is_cursor_on = false;
            curs_set(state->force_cursor);
            vt_trace(state, "DC4\n");
            continue;
        case 30:    //  RS  - back to origin
            vt_decoder_fill_end(state);
            state->col = 0;
            state->row = 0;
            vt_trace(state, "RS: row=%d, col=%d\n", state->row, state->col);
            vt_cursor(state);
            continue;
        }

        int row_code = (b & 0xF);
        int col_code = (b & 0x70) >> 4;
        vt_trace(state, "row_code=%d, col_code=%d, is_escaped=%d\n", 
            row_code, col_code, state->flags.is_escaped);

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
                vt_trace(state, "row_code %d\n", row_code);
                break;
            case 8: //  Flash
                state->after_flags.is_flashing = true;
                vt_trace(state, "set-after-flash=true\n");
                break;
            case 9: //  Steady
                state->flags.is_flashing = false;
                vt_trace(state, "flash=false\n");
                break;
            case 10:    //  end box
                state->after_flags.is_boxing = false;
                vt_trace(state, "set-after-boxing=false\n");
                break;
            case 11:    //  start box 
                state->after_flags.is_boxing = true;
                vt_trace(state, "set-after-boxing=true\n");
                break;
            case 12:    //  normal height
                state->flags.is_double_height = false;
                state->flags.held_mosaic = state->space;
                vt_trace(state, "double-height=false, held-mosaic=' '\n");
                break;
            case 13:    //  double height (but not on last row)
                if (state->row < (MAX_ROWS - 2)) {
                    state->after_flags.is_double_height = true;
                    vt_trace(state, "set-after-double-height=true\n");
                }
                break;
            default:
                state->after_flags.alpha_fg_color = row_code;
                vt_trace(state, "set-after-alpha-fg=%d\n", row_code);
                break;
            }
        }
        else if (col_code == 1) {
            switch (row_code) {
            case 0:     //  Data Link Escape (graphics black at level 2.5+)
                vt_trace(state, "DLE\n");
                break;
            case 8:     //  conceal display
                state->flags.is_concealed = true;
                vt_trace(state, "is-concealed=true\n");
                break;
            case 9:
                state->flags.is_contiguous = true;
                vt_trace(state, "is-contiguous=true\n");
                break;
            case 10:
                state->flags.is_contiguous = false;
                vt_trace(state, "is-contiguous=false\n");
                break;
            case 11:    //  escape - do not print
                state->flags.is_escaped = true;
                vt_trace(state, "is-escaped=true\n");
                continue;
            case 12:    //  black bg
                state->flags.bg_color = BLACK;
                vt_trace(state, "bg-color=BLACK\n");
                break;
            case 13:    //  new bg, i.e. use the fg for the bg
                state->flags.bg_color = state->flags.is_alpha ? 
                    state->flags.alpha_fg_color : state->flags.mosaic_fg_color;
                vt_trace(state, "bg-color=%d\n", state->flags.bg_color);
                break;
            case 14:    //  hold graphics
                state->flags.is_mosaic_held = true;
                vt_trace(state, "is-mosaic-held=true\n");
                break;
            case 15:    //  release graphics
                state->after_flags.is_mosaic_held = false;
                vt_trace(state, "set-after-is-mosaic-held=false\n");
                break;
            default:
                state->after_flags.mosaic_fg_color = row_code;
                vt_trace(state, "set-after-mosaic-fg=%d\n", state->after_flags.mosaic_fg_color);
                break;
            }
        }

        if (state->row != state->dheight_low_row) {
            struct vt_attr attr;
            vt_set_attr(state, &attr);
            struct vt_char ch;

            if (col_code == 0 || col_code == 1) {
                ch = state->flags.is_mosaic_held ? state->flags.held_mosaic : state->space;

                if (state->flags.is_double_height) {
                    vt_put_char(state, state->row, state->col, ch.upper, &attr, true);
                }
                else {
                    vt_put_char(state, state->row, state->col, ch.single, &attr, true);
                }

                if (state->row == 0) {
                    state->header_row[state->col] = SPACE;
                }
            }
            else {
                vt_get_char_code(state, state->flags.is_alpha, 
                    state->flags.is_contiguous, row_code, col_code, &ch);

                if (state->flags.is_double_height) {
                    vt_put_char(state, state->row, state->col, ch.upper, &attr, true);
                }
                else {
                    vt_put_char(state, state->row, state->col, ch.single, &attr, true);
                }

                if (!state->flags.is_alpha) {
                    state->flags.held_mosaic = ch;
                    vt_trace(state, "held-mosaic='%lc'\n", state->flags.held_mosaic);
                }

                if (state->row == 0) {
                    state->header_row[state->col] = ch.single;
                }
            }

            if (state->flags.is_double_height) {
                vt_put_char(state, state->dheight_low_row, state->col, ch.lower, &attr, true);
            }
        }

        vt_decoder_apply_after_flags(state);

        if (state->after_flags.is_double_height == TRI_TRUE) {
            state->dheight_low_row = state->row + 1;
            vt_trace(state, "dheight_low_row=%d\n", state->dheight_low_row);
        }

        vt_decoder_reset_after_flags(state);
        ++state->col;

        //  Automatically start a new row if we've got a full row
        if (state->col == MAX_COLS) {
            vt_decoder_next_row(state);
        }

        wmove(state->win, state->row, state->col);
        wrefresh(state->win);
    }

    //  Move the cursor to the last character displayed
    curs_set(state->force_cursor || state->flags.is_cursor_on);
    wmove(state->win, state->row, state->col);
    wrefresh(state->win);
}

void 
vt_toggle_flash(struct vt_decoder_state *state)
{
    state->screen_flash_state = !state->screen_flash_state;
    vt_trace(state, "flash=%d\n", state->screen_flash_state);

    for (int r = 0; r < MAX_ROWS; ++r) {
        for (int c = 0; c < MAX_COLS; ++c) {
            struct vt_decoder_cell *cell = &state->cells[r][c];

            if (cell->attr.has_flash) {
                vt_put_char(state, r, c, cell->character, &cell->attr, false);
            }
        }
    }

    wrefresh(state->win);
}

void 
vt_toggle_reveal(struct vt_decoder_state *state)
{
    state->screen_revealed_state = !state->screen_revealed_state;
    vt_trace(state, "revealed=%d\n", state->screen_revealed_state);

    for (int r = 0; r < MAX_ROWS; ++r) {
        for (int c = 0; c < MAX_COLS; ++c) {
            struct vt_decoder_cell *cell = &state->cells[r][c];

            if (cell->attr.has_concealed) {
                vt_put_char(state, r, c, cell->character, &cell->attr, false);
            }
        }
    }

    wrefresh(state->win);
}

static void 
vt_decoder_new_frame(struct vt_decoder_state *state)
{
    state->row = 0;
    state->col = 0;
    state->dheight_low_row = -1;
    state->frame_buffer_offset = 0;
    state->screen_revealed_state = false;
    vt_decoder_reset_flags(state);
    vt_decoder_reset_after_flags(state);
    memset(state->cells, 0, sizeof(state->cells));

    for (int i = 0; i < MAX_COLS; ++i) {
        state->header_row[i] = SPACE;
    }

    struct vt_attr attr;
    memset(&attr, 0, sizeof(struct vt_attr));

    for (int r = 0; r < MAX_ROWS; ++r) {
        for (int c = 0; c < MAX_COLS; ++c) {
            state->cells[r][c].character = SPACE;
            vt_put_char(state, r, c, SPACE, &attr, false);
        }
    }
}

static void 
vt_decoder_next_row(struct vt_decoder_state *state)
{
    if ((state->row + 1) < MAX_ROWS) {
        ++state->row;
    }
    else {
        state->row = 0;
    }
    
    state->col = 0;
    vt_decoder_reset_flags(state);
    vt_decoder_reset_after_flags(state);
}

static void 
vt_decoder_fill_end(struct vt_decoder_state *state)
{
    if (state->col > 0) {
        struct vt_decoder_cell *prev = &state->cells[state->row][state->col - 1];
        struct vt_attr attr;
        memset(&attr, 0, sizeof(struct vt_attr));
        attr.attr = prev->attr.attr;
        attr.color_pair = prev->attr.color_pair;

        for (int col = state->col; col < MAX_COLS; ++col) {
            wchar_t ch = state->cells[state->row][col].character;
            vt_put_char(state, state->row, col, ch, &attr, true);
        }
    }
}

static void 
vt_decoder_apply_after_flags(struct vt_decoder_state *state)
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
    }
}

static void 
vt_decoder_reset_flags(struct vt_decoder_state *state)
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
    //  Leave cursor ON
}

static void 
vt_decoder_reset_after_flags(struct vt_decoder_state *state)
{
    state->after_flags.alpha_fg_color = NONE;
    state->after_flags.mosaic_fg_color = NONE;
    state->after_flags.is_flashing = TRI_UNDEF;
    state->after_flags.is_boxing = TRI_UNDEF;
    state->after_flags.is_mosaic_held = TRI_UNDEF;
    state->after_flags.is_double_height = TRI_UNDEF;
}

static void 
vt_set_attr(struct vt_decoder_state *state, struct vt_attr *attr)
{
    memset(attr, 0, sizeof(struct vt_attr));
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
    struct vt_char *ch)
{
    ch->single = state->map_char(row_code, col_code, is_alpha, is_contiguous, false, false);
    ch->upper = state->map_char(row_code, col_code, is_alpha, is_contiguous, true, false);
    ch->lower = state->map_char(row_code, col_code, is_alpha, is_contiguous, true, true);
}

static void
vt_put_char(struct vt_decoder_state *state, int row, int col, wchar_t ch, struct vt_attr *attr, bool trace)
{
    struct vt_decoder_cell *cell = &state->cells[row][col];
    short display_color = state->mono_mode ? 0 : attr->color_pair;
    wchar_t display_ch = ch;
    attr_t display_attr = attr->attr;

    if (attr->has_concealed && !state->screen_revealed_state) {
        display_ch = SPACE;
    }

    if (attr->has_flash && !state->screen_flash_state) {
        display_ch = SPACE;
    }

    wchar_t vchar[2] = {display_ch, L'\0'};
    cchar_t cc;
    setcchar(&cc, vchar, display_attr, display_color, 0);
    mvwadd_wch(state->win, row, col, &cc);

    cell->attr = *attr;
    cell->character = ch;

    if (trace) {
        vt_trace(state, 
            "putchar: char='%lc', display='%lc', code=%d, attr=%d, color=%d, flashing=%d, concealed=%d, row=%d, col=%d\n", 
            ch, display_ch, ch, attr->attr, attr->color_pair, state->flags.is_flashing, state->flags.is_concealed, row, col);
    }
}

static void 
vt_init_colors()
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
    if (state->trace_file != NULL) {
        va_list args;
        va_start(args, format);
        vfprintf(state->trace_file, format, args);
        va_end(args);
    }
}


static void
vt_cursor(struct vt_decoder_state *state)
{
    if (state->force_cursor) {
        wmove(state->win, state->row, state->col);
        wrefresh(state->win);
    }
}
