#include <stdarg.h>
#include "decoder.h"
#define ATTRS_IGNORED_TO_EOL_MASK (~A_UNDERLINE)

static void vt_decoder_new_frame(struct vt_decoder_state *state);
static void vt_decoder_next_row(struct vt_decoder_state *state);
static void vt_decoder_fill_end(struct vt_decoder_state *state);
static void vt_decoder_reset_flags(struct vt_decoder_flags *flags);
static attr_t vt_get_attr(struct vt_decoder_state *state);
static short vt_get_color(struct vt_decoder_state *state);
static short vt_get_color_pair_number(enum vt_decoder_color fg, enum vt_decoder_color bg);
static void vt_decoder_apply_after_flags(struct vt_decoder_after_flags *after, struct vt_decoder_flags *flags);
static void vt_decoder_reset_after_flags(struct vt_decoder_after_flags *flags);
static wchar_t vt_get_char_code(struct vt_decoder_state *state, int row_code, int col_code);
static void vt_put_char(struct vt_decoder_state *state, int row, int col, wchar_t ch, attr_t attr, short color, enum vt_decoder_markup_hint markup_hint);
static void vt_init_colors();
static void vt_trace(struct vt_decoder_state *state, char *format, ...);

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
}

void 
vt_decode(struct vt_decoder_state *state, uint8_t *buffer, int count)
{
    //  n.b. After evaluating chars, we 'continue' if it shouldn't be displayed. 'break' if it should
    for (int bidx = 0; bidx < count; ++bidx) {
        uint8_t b = buffer[bidx];

        if (state->frame_buffer_offset < FRAME_BUFFER_MAX) {
            state->frame_buffer[state->frame_buffer_offset++] = b;
        }

        //  ASCII control codes (codes < 32)
        //  Rows are either 40 chars long exactly or less than 40 chars and terminated by CRLF or CR or LF
        switch (b) {
            case 0:     //  NULL
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

                if (state->force_cursor) {
                    move(state->row, state->col);
                    refresh();
                }
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

                if (state->force_cursor) {
                    move(state->row, state->col);
                    refresh();
                }
                continue;
            case 10:    //  LF (end of row)
                vt_decoder_next_row(state);
                vt_trace(state, "LF: row=%d, col=%d\n", state->row, state->col);

                if (state->force_cursor) {
                    move(state->row, state->col);
                    refresh();
                }
                continue;
            case 11:    //  v-tab
                --state->row;

                if (state->row < 0) {
                    state->row = MAX_ROWS - 1;
                }

                vt_trace(state, "v-tab: row=%d, col=%d\n", state->row, state->col);

                if (state->force_cursor) {
                    move(state->row, state->col);
                    refresh();
                }
                continue;
            case 12:
                //  FF (new frame/clear screen)
                vt_decoder_new_frame(state);
                vt_trace(state, "FF: row=%d, col=%d\n", state->row, state->col);

                if (state->force_cursor) {
                    move(state->row, state->col);
                    refresh();
                }
                continue;
            case 13:    //  CR
                vt_decoder_fill_end(state);
                state->col = 0;
                vt_trace(state, "CR: row=%d, col=%d\n", state->row, state->col);

                if (state->force_cursor) {
                    move(state->row, state->col);
                    refresh();
                }
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

                if (state->force_cursor) {
                    move(state->row, state->col);
                    refresh();
                }
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
                    vt_trace(state, "boxing=false\n");
                    break;
                case 11:    //  start box 
                    state->after_flags.is_boxing = true;
                    vt_trace(state, "boxing=true\n");
                    break;
                case 12:    //  normal height
                    state->flags.is_double_height = false;
                    state->flags.held_mosaic = SPACE;
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
            }
        }

        if (state->row != state->double_height_lower_row) {
            attr_t attr = vt_get_attr(state);
            short color = vt_get_color(state);

            //  Row 2 of double height text has no fg data
            //  When processing the next row, we need to ensure that we don't overwrite it
            if (state->flags.is_double_height) {
                short color_below = state->cells[state->double_height_lower_row][state->col].color_pair;
                short fg, bg;
                pair_content(color_below, &fg, &bg);
                color_below = vt_get_color_pair_number(fg, state->flags.bg_color);
                vt_put_char(state, state->double_height_lower_row, state->col, SPACE, 0, color_below, MH_LOWER_ROW);
            }

            if (col_code == 0 || col_code == 1) {
                wchar_t ch = state->flags.is_mosaic_held ? state->flags.held_mosaic : SPACE;
                enum vt_decoder_markup_hint hint = 
                    state->flags.is_mosaic_held ? MH_MOSAIC_HELD : MH_NONE;
                vt_put_char(state, state->row, state->col, ch, attr, color, hint);

                if (state->row == 0) {
                    state->header_row[state->col] = SPACE;
                }
            }
            else {
                state->last_character = vt_get_char_code(state, row_code, col_code);
                vt_put_char(state, state->row, state->col, state->last_character, attr, color, MH_NONE);

                if (!state->flags.is_alpha) {
                    state->flags.held_mosaic = state->last_character;
                    vt_trace(state, "held-mosaic='%lc'\n", state->flags.held_mosaic);
                }

                if (state->row == 0) {
                    state->header_row[state->col] = state->last_character;
                }
            }
        }

        vt_decoder_apply_after_flags(&state->after_flags, &state->flags);

        if (state->after_flags.is_double_height == TRI_TRUE) {
            state->double_height_lower_row = state->row + 1;
        }

        vt_decoder_reset_after_flags(&state->after_flags);
        ++state->col;

        //  Automatically start a new row if we've got a full row
        if (state->col == MAX_COLS) {
            vt_decoder_next_row(state);
        }

        move(state->row, state->col);
        refresh();
    }

    //  Move the cursor to the last character displayed
    curs_set(state->force_cursor || state->flags.is_cursor_on);
    move(state->row, state->col);
    refresh();
}

void 
vt_toggle_flash(struct vt_decoder_state *state)
{
    state->screen_flash_state = !state->screen_flash_state;

    for (int r = 0; r < MAX_ROWS; ++r) {
        struct vt_decoder_cell *row = state->cells[r];
        int c = 0;

        while (c < MAX_COLS) {
            while (c < MAX_COLS && !row[c].has_flash_attribute) {
                ++c;
            }

            if (c < MAX_COLS) {
                int start_c = c;
                int span = 1;
                attr_t start_attr = row[c].attribute;
                short start_color = row[c].color_pair;
                ++c;

                while (c < MAX_COLS 
                    && row[c].has_flash_attribute 
                    && row[c].color_pair == start_color
                    && row[c].attribute == start_attr) {
                    ++c;
                    ++span;
                }

                if (state->screen_flash_state) {
                    short fg, bg;
                    pair_content(start_color, &fg, &bg);
                    start_color = vt_get_color_pair_number(bg, fg);
                }

                mvchgat(r, start_c, span, start_attr, start_color, NULL);
                refresh();
            }

            ++c;
        }
    }
}

/*
    Load(b64: string): void {
        Reset();
        DecodeBase64(b64);
    }

    GetFrameAsBase64(): string {
        return btoa(String.fromCharCode(...frameBuffer.slice(0, state->frame_buffer_offset)));
    }

    FrameNumber(): string {
        const text = headerRow.join('');
        const matches = text.match(/ P?(\d+)|(\d+[a-z]) /);

        if (matches && matches.length > 1) {
            return matches[1];
        }

        return '0';
    }
*/

static void 
vt_decoder_new_frame(struct vt_decoder_state *state)
{
    state->row = 0;
    state->col = 0;
    state->double_height_lower_row = -1;
    state->frame_buffer_offset = 0;
    vt_decoder_reset_flags(&state->flags);
    vt_decoder_reset_after_flags(&state->after_flags);
    memset(state->cells, 0, sizeof(state->cells));

    for (int i = 0; i < MAX_COLS; ++i) {
        state->header_row[i] = SPACE;
    }

    for (int r = 0; r < MAX_ROWS; ++r) {
        for (int c = 0; c < MAX_COLS; ++c) {
            state->cells[r][c].character = SPACE;
            vt_put_char(state, r, c, SPACE, 0, 0, MH_MASK);
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
    vt_decoder_reset_flags(&state->flags);
    vt_decoder_reset_after_flags(&state->after_flags);
}

static void 
vt_decoder_fill_end(struct vt_decoder_state *state)
{
    if (state->col > 0) {
        struct vt_decoder_cell *prev = &state->cells[state->row][state->col - 1];
        attr_t attr = prev->attribute & ATTRS_IGNORED_TO_EOL_MASK;
        short color = prev->color_pair;

        for (int col = state->col; col < MAX_COLS; ++col) {
            wchar_t ch = state->cells[state->row][col].character;
            vt_put_char(state, state->row, col, ch, attr, color, MH_FILL_END);
        }
    }
}

static void 
vt_decoder_apply_after_flags(struct vt_decoder_after_flags *after, struct vt_decoder_flags *flags)
{
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
        flags->held_mosaic = SPACE;
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
vt_decoder_reset_flags(struct vt_decoder_flags *flags)
{
    flags->bg_color = BLACK;
    flags->alpha_fg_color = WHITE;
    //flags->mosaic_fg_color = WHITE;
    flags->is_alpha = true;
    flags->is_flashing = false;
    flags->is_escaped = false;
    flags->is_boxing = false;
    flags->is_concealed = false;
    flags->is_contiguous = true;
    flags->is_mosaic_held = false;
    flags->held_mosaic = SPACE;
    flags->is_double_height = false;
    //  Leave cursor ON
}

static void 
vt_decoder_reset_after_flags(struct vt_decoder_after_flags *flags)
{
    flags->alpha_fg_color = NONE;
    flags->mosaic_fg_color = NONE;
    flags->is_flashing = TRI_UNDEF;
    flags->is_boxing = TRI_UNDEF;
    flags->is_mosaic_held = TRI_UNDEF;
    flags->is_double_height = TRI_UNDEF;
}

static attr_t 
vt_get_attr(struct vt_decoder_state *state)
{
    attr_t attr = 0;

    if (state->flags.is_concealed) {
        attr |= A_PROTECT;
    }

    if (state->flags.is_double_height) {
        attr |= A_UNDERLINE;
    }

    return attr;
}

static short 
vt_get_color(struct vt_decoder_state *state)
{
    if (!has_colors()) {
        return 0;
    }

    enum vt_decoder_color fg = state->flags.is_alpha ? 
        state->flags.alpha_fg_color : state->flags.mosaic_fg_color;

    return vt_get_color_pair_number(fg, state->flags.bg_color);
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

static wchar_t 
vt_get_char_code(struct vt_decoder_state *state, int row_code, int col_code)
{
    struct bed_character_code ch;

    bed_get_display_char(
        row_code,
        col_code, 
        state->flags.is_alpha, 
        !state->flags.is_alpha,
        state->flags.is_contiguous, 
        &ch);

    return ch.code;
}

static void
vt_put_char(struct vt_decoder_state *state, int row, int col, wchar_t ch, attr_t attr, short color, enum vt_decoder_markup_hint markup_hint)
{
    short display_color = state->mono_mode ? 0 : color;
    wchar_t display_ch = ch;

    if (state->markup_mode && markup_hint != MH_NONE) {
        switch (markup_hint) {
        case MH_FILL_END:
            display_ch = L']';
            break;
        case MH_MOSAIC_HELD:
            display_ch = L'}';
            break;
        case MH_MASK:
            display_ch = L'~';
            break;
        case MH_LOWER_ROW:
            display_ch = L'¬';
            break;
        default:
            display_ch = ch;
            break;
        }
    }

    wchar_t vchar[2] = {display_ch, L'\0'};
    cchar_t cc;
    setcchar(&cc, vchar, attr, display_color, 0);
    mvadd_wch(row, col, &cc);

    struct vt_decoder_cell *cell = &state->cells[row][col];
    cell->attribute = attr;
    cell->character = ch;
    cell->color_pair = color;
    cell->has_flash_attribute = state->flags.is_flashing;

    if (markup_hint != MH_MASK) {
        vt_trace(state, 
            "putchar: char='%lc', code=%d, attr=%d, color=%d, flashing=%d, row=%d, col=%d\n", 
            ch, ch, attr, color, state->flags.is_flashing, row, col);
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
