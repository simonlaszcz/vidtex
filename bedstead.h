#ifndef __BEDSTEAD_H
#define __BEDSTEAD_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

struct bed_character_code
{
    uint16_t code;
    uint8_t native_code;
}; 

void bed_get_display_char(
    int row_code, 
    int col_code, 
    bool is_alpha, 
    bool is_graph, 
    bool is_contiguous, 
    struct bed_character_code *code);

#endif
