#ifndef __DECODER_H
#define __DEODER_H

#define _XOPEN_SOURCE 700
#include <ncursesw/curses.h>
#include <stdbool.h>
#include <string.h>
#include "bedstead.h"
#include "galax.h"

#define MAX_ROWS 24
#define MAX_COLS 40
#define FRAME_BUFFER_MAX 2000
#define SPACE L' '

enum vt_decoder_color
{
    BLACK = COLOR_BLACK,
    RED = COLOR_RED,
    GREEN = COLOR_GREEN,
    YELLOW = COLOR_YELLOW,
    BLUE = COLOR_BLUE,
    MAGENTA = COLOR_MAGENTA,
    CYAN = COLOR_CYAN,
    WHITE = COLOR_WHITE,
    //  NULL/unspecified
    NONE = -1 
};

enum vt_decoder_tristate
{
    TRI_FALSE = 0,
    TRI_TRUE = 1,
    TRI_UNDEF = -1
};

struct vt_decoder_flags
{
    //  Bg colour. The 'black bg' and 'new bg' commands are Set-At.
    enum vt_decoder_color bg_color;
    //  Alphanumeric (G0 charset) fg colour. Selects alpha characters. Set-After
    enum vt_decoder_color alpha_fg_color;
    //  Graphics/mosaic (G1 charset) fg colour. Selects mosaic characters. Set-After
    enum vt_decoder_color mosaic_fg_color;
    //  When TRUE we're rendering alphanumerics, otherwise graphics. Set-After in accordance with fg colours
    bool is_alpha;
    //  When TRUE we're rendering contiguous graphics, otherwise separated. Set-At
    bool is_contiguous;
    //  True if flashing. True is Set-After, FALSE (steady) is Set-At
    bool is_flashing;
    //  When TRUE, the next char is a control code. N.b. is not used to select alternate G0 sets. 
    //  Esc characters are not displayed as space. Set-After
    bool is_escaped;
    //  True after a start box command. Set-After
    bool is_boxing;
    //  When TRUE, the text is concealed. Set-At
    bool is_concealed;
    /*
    Hold Mosaics ("Set-At")
    Generally, all spacing attributes are displayed as spaces, implying at least one space
    between characters or mosaics with different colours in the same row. In mosaics mode, the
    "Hold Mosaics" option allows a limited range of attribute changes without intervening
    spaces. A mosaic character from the G1 set (referred to as the "Held-Mosaic" character) is
    displayed in place of the character "SPACE" corresponding to a control character.
    Substitution only takes place in mosaics mode when Hold Mosaics mode is in force. At a
    screen location where substitution is permitted, the "Held-Mosaic" character inserted is the
    most recent mosaics character with bit 6 = '1' in its code on that row. The "Held-Mosaic"
    character is reset to "SPACE" at the start of each row, on a change of
    alphanumeric/mosaics mode or on a change of size. It is not reset by reinforcement of the
    existing size setting. It is not reset by a change in Hold Mosaics mode.
    The "Held-Mosaic" character is always displayed in its original contiguous or separated form
    regardless of the mode prevailing at the time of substitution.
    Setting False is Set-After
    */
    bool is_mosaic_held;
    wchar_t held_mosaic;
    bool is_double_height;
    //  DC1 = on, DC4 = off. Set-At
    bool is_cursor_on;
};

struct vt_decoder_after_flags
{
    enum vt_decoder_color alpha_fg_color;
    enum vt_decoder_color mosaic_fg_color;
    enum vt_decoder_tristate is_flashing;
    enum vt_decoder_tristate is_boxing;
    enum vt_decoder_tristate is_mosaic_held;
    enum vt_decoder_tristate is_double_height;
};

struct vt_attr
{
    //  Curses attr
    attr_t attr;
    //  Curses color pair
    short color_pair;
    //  Other flags
    bool has_flash;
    bool has_concealed;
};

struct vt_decoder_cell
{
    struct vt_attr attr;
    wchar_t character;
};

struct vt_decoder_state
{
    WINDOW *win;
    bool force_cursor;
    bool mono_mode;
    bool bold_mode;
    FILE *trace_file;
    struct vt_decoder_flags flags;
    struct vt_decoder_after_flags after_flags;
    int row;
    int col;
    //  Set when we need to ignore double height row 2 in the input stream
    int dheight_low_row;
    wchar_t last_character;
    wchar_t frame_buffer[FRAME_BUFFER_MAX];
    int frame_buffer_offset;
    //  Store the characters written to the first row so we can check for the page number
    wchar_t header_row[MAX_COLS + 1];
    bool screen_flash_state;
    bool screen_revealed_state;
    struct vt_decoder_cell cells[MAX_ROWS][MAX_COLS];

    uint16_t (*map_char)(int row_code, int col_code, bool is_alpha, 
        bool is_contiguous, bool is_dheight, bool is_dheight_lower);
};

void vt_decoder_init(struct vt_decoder_state *state);
void vt_decode(struct vt_decoder_state *state, uint8_t *buffer, int count);
void vt_toggle_flash(struct vt_decoder_state *state);
void vt_toggle_reveal(struct vt_decoder_state *state);

#endif
