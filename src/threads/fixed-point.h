#ifndef FIXED_POINT_H
#define FIXED_POINT_H

#include <stdint.h>

/* Macros defined for the 17.14 fixed point implementation */
#define FIXED_POINT_P 17
#define FIXED_POINT_Q 14
#define FIXED_POINT_SHIFT (1 << FIXED_POINT_Q)
typedef int32_t fp1714;

fp1714 from_integer (int32_t n);
int32_t to_integer_round_0 (int32_t x);
int32_t to_integer_round_nearest (int32_t x);
fp1714 add_x_and_y (int32_t x, int32_t y);
fp1714 subtract_y_from_x (int32_t x, int32_t y);
fp1714 add_x_and_n (int32_t x, int32_t n);
fp1714 subtract_n_from_x (int32_t x, int32_t y);
fp1714 multiply_x_and_y (int32_t x, int32_t y);
fp1714 multiply_x_and_n (int32_t x, int32_t n);
fp1714 divide_x_and_y (int32_t x, int32_t y);
fp1714 divide_x_and_n (int32_t x, int32_t n);

#endif