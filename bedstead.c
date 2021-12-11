#include "bedstead.h"

static uint16_t bed_map_char(int row_code, int col_code, bool is_alpha, bool is_graph, bool is_contiguous);

void bed_get_display_char(
    int row_code, 
    int col_code, 
    bool is_alpha, 
    bool is_graph, 
    bool is_contiguous, 
    struct bed_character_code *code)
{
    uint16_t m = bed_map_char(row_code, col_code, is_alpha, is_graph, is_contiguous);
    code->code = m;
    code->native_code = ((col_code << 4) + row_code) & 0x7F;
}

static uint16_t bed_map_char(int row_code, int col_code, bool is_alpha, bool is_graph, bool is_contiguous)
{
    if (row_code < 0 || row_code > 15 || col_code < 0 || col_code > 7) {
        //  Return space if out of bounds
        return 0x20;
    }

    if (col_code == 2 && is_alpha) {
        switch (row_code) {
        case 3: return 0xA3;
        default: return 0x20 + row_code;
        }
    }
    else if (col_code == 2 && is_graph) {
        return (is_contiguous ? 0xEE00 : 0xEE20) + row_code;
    }
    else if (col_code == 3 && is_alpha) {
        return 0x30 + row_code;
    }
    else if (col_code == 3 && is_graph) {
        return (is_contiguous ? 0xEE10 : 0xEE30) + row_code;
    }
    else if (col_code == 4) {
        return 0x40 + row_code;
    }
    else if (col_code == 5) {
        switch (row_code) {
        case 11: return 0x2190;
        case 12: return 0xBD;
        case 13: return 0x2192;
        case 14: return 0x2191;
        case 15: return 0x23;
        default: return 0x50 + row_code;
        }
    }
    else if (col_code == 6 && is_alpha) {
        switch (row_code) {
        case 0: return 0x2013;
        default: return 0x60 + row_code;
        }
    }
    else if (col_code == 6 && is_graph) {
        return (is_contiguous ? 0xEE40 : 0xEE60) + row_code;
    }
    else if (col_code == 7 && is_alpha) {
        switch (row_code) {
        case 11: return 0xBC;
        case 12: return 0x2016;
        case 13: return 0xBE;
        case 14: return 0xF7;
        case 15: return 0x25A0;
        default: return 0x70 + row_code;
        }
    }
    else if (col_code == 7 && is_graph) {
        return (is_contiguous ? 0xEE50 : 0xEE70) + row_code;
    }

    return 0x20;
}


