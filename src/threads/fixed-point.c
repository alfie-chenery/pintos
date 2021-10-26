#include "threads/fixed-point.h"

int32_t from_integer(int32_t n)
{
    return n * FIXED_POINT_SHIFT;
}

int32_t to_integer_round_0(int32_t x)
{
    return x / FIXED_POINT_SHIFT;
}

int32_t to_integer_round_nearest(int32_t x)
{
    return x >= 0 ? (x + FIXED_POINT_SHIFT / 2) / FIXED_POINT_SHIFT : 
        (x - FIXED_POINT_SHIFT / 2) / FIXED_POINT_SHIFT;
}

int32_t add_x_and_y(int32_t x, int32_t y)
{
    return x + y;
}

int32_t subtract_y_from_x(int32_t x, int32_t y)
{
    return x - y;
}

int32_t add_x_and_n(int32_t x, int32_t n)
{
    return x + n * FIXED_POINT_SHIFT;
}

int32_t subtract_n_from_x(int32_t x, int32_t n)
{
    return x - n * FIXED_POINT_SHIFT;
}

int32_t multiply_x_and_y(int32_t x, int32_t y)
{
    return ((int64_t)x) * y / FIXED_POINT_SHIFT;
}

int32_t multiply_x_and_n(int32_t x, int32_t n)
{
    return x * n;
}

int32_t divide_x_and_y(int32_t x, int32_t y)
{
    return ((int64_t)x) * FIXED_POINT_SHIFT / y;
}


int32_t divide_x_and_n(int32_t x, int32_t n)
{
    return x / n;
}

