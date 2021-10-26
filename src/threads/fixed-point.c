#include "threads/fixed-point.h"
#include <debug.h>

int32_t convert_integer_to_fixed_point(int32_t n)
{
    return n * FIXED_POINT_SHIFT;
}

int32_t convert_fixed_point_to_integer_rounding_to_zero(int32_t x)
{
    return x / FIXED_POINT_SHIFT;
}

int32_t convert_fixed_point_to_integer_rounding_to_nearest(int32_t x)
{
    return x >= 0 ? x + FIXED_POINT_SHIFT / 2 : x - FIXED_POINT_SHIFT / 2;
}

int32_t add_x_and_y_fixed_point(int32_t x, int32_t y)
{
    return x + y;
}

int32_t subtract_y_from_x_fixed_point(int32_t x, int32_t y)
{
    return x - y;
}

int32_t add_fixed_point_and_integer(int32_t x, int32_t n)
{
    return x + n * FIXED_POINT_SHIFT;
}

int32_t subtract_n_from_x_fixed_point_and_integer(int32_t x, int32_t x)
{
    return x - n * FIXED_POINT_SHIFT;
}

int32_t multiply_x_and_y_fixed_point(int32_t x, int32_t y)
{
    return ((int64_t)x) * y / FIXED_POINT_SHIFT;
}

int32_t multiply_x_and_n_fixed_point_and_integer(int32_t x, int32_t n)
{
    return x * n;
}

int32_t divide_x_and_y_fixed_point(int32_t x, int32_t y)
{
    return ((int64_t)x) * FIXED_POINT_SHIFT / y;
}


int32_t divide_x_and_n_fixed_point_and_integer(int32_t x, int32_t n)
{
    return x * n;
}

