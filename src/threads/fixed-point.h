#ifndef FIXED_POINT_H
#define FIXED_POINT_H

#include<stdint.h>

/* Macros defined for the 17.14 fixed point implementation */
#define FIXED_POINT_P 17
#define FIXED_POINT_Q 14
#define FIXED_POINT_SHIFT (1 << FIXED_POINT_Q)

int32_t convert_integer_to_fixed_point(int32_t n);
int32_t convert_fixed_point_to_integer_rounding_to_zero(int32_t x);
int32_t convert_fixed_point_to_integer_rounding_to_nearest(int32_t x);
int32_t add_x_and_y_fixed_point(int32_t x, int32_t y);
int32_t subtract_y_from_x_fixed_point(int32_t x, int32_t y);
int32_t add_fixed_point_and_integer(int32_t x, int32_t n);
int32_t subtract_n_from_x_fixed_point_and_integer(int32_t x, int32_t y);
int32_t multiply_x_and_y_fixed_point(int32_t x, int32_t y);
int32_t multiply_x_and_n_fixed_point_and_integer(int32_t x, int32_t n);
int32_t divide_x_and_y_fixed_point(int32_t x, int32_t y);
int32_t divide_x_and_n_fixed_point_and_integer(int32_t x, int32_t n);