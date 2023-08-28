#ifndef GALAX_H
#define GALAX_H

#include <stdint.h>
#include <stdbool.h>

uint16_t gal_map_char(
    int row_code, 
    int col_code, 
    bool is_alpha, 
    bool is_contiguous,
    bool is_dheight,
    bool is_dheight_lower);

#endif
