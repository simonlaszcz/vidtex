#include "galax.h"

uint16_t gal_map_char(int row_code, int col_code, bool is_alpha, bool is_contiguous, bool is_dheight, bool is_dheight_lower)
{
    bool is_graph = !is_alpha;

    if (row_code < 0 || row_code > 15 || col_code < 0 || col_code > 7) {
        //  Return space if out of bounds
        return '?';
    }
    
    uint16_t ch = 0x20;

    if (is_graph) {
        switch (col_code) {
        case 2:
            ch = 0xE200 + row_code;
            break;
        case 3:
            ch = 0xE210 + row_code;
            break;
        case 6:
            ch = 0xE220 + row_code;
            break;
        case 7:
            ch = 0xE230 + row_code;
            break;
        }

        if (!is_contiguous) {
            ch += 0xC0;
        }

        if (is_dheight) {
            if (is_dheight_lower) {
                ch += 0x80;
            }
            else {
                ch += 0x40;
            }
        }
    }
    else {
        switch (col_code) {
        case 2:
            switch (row_code) {
            case 3: 
                ch = 0xA3;
                break;
            default: 
                ch = 0x20 + row_code;
                break;
            }
            break;
        case 3:
            ch = 0x30 + row_code;
            break;
        case 4:
            ch = 0x40 + row_code;
            break;
        case 5:
            switch (row_code) {
            case 12:
                ch = 0xBD;
                break;
            case 15:
                ch = 0x23;
                break;
            default:
                ch = 0x50 + row_code;
                break;
            }
            break;
        case 6:
            ch = 0x60 + row_code;
            break;
        case 7:
            switch (row_code) {
            case 11:
                ch = 0xBC;
                break;
            case 13:
                ch = 0xBE;
                break;
            case 14:
                ch = 0xF7;
                break;
            case 15:
                ch = 0xB6;
                break;
            default:
                ch = 0x70 + row_code;
                break;
            }
            break;
        }

        if (is_dheight) {
            if (is_dheight_lower) {
                ch += 0xE100;
            }
            else {
                ch += 0xE000;
            }
        }
    }

    return ch;
}
