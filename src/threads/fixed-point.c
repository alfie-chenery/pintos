#include "threads/fixed-point.h"

fp1714 from_integer (int32_t n)
{
    return n * FIXED_POINT_SHIFT;
}

int32_t to_integer_round_0 (fp1714 x)
{
    return x / FIXED_POINT_SHIFT;
}

int32_t to_integer_round_nearest (fp1714 x)
{
    return x >= 0 ? (x + FIXED_POINT_SHIFT / 2) / FIXED_POINT_SHIFT : 
        (x - FIXED_POINT_SHIFT / 2) / FIXED_POINT_SHIFT;
}

fp1714 add_x_and_y (fp1714 x, fp1714 y)
{
    return x + y;
}

fp1714 subtract_y_from_x (fp1714 x, fp1714 y)
{
    return x - y;
}

fp1714 add_x_and_n (fp1714 x, int32_t n)
{
    return x + n * FIXED_POINT_SHIFT;
}

fp1714 subtract_n_from_x (fp1714 x, int32_t n)
{
    return x - n * FIXED_POINT_SHIFT;
}

fp1714 multiply_x_and_y (fp1714 x, fp1714 y)
{
    return ((int64_t) x) * y / FIXED_POINT_SHIFT;
}

fp1714 multiply_x_and_n (fp1714 x, int32_t n)
{
    return x * n;
}

fp1714 divide_x_and_y (fp1714 x, fp1714 y)
{
    return ((int64_t)x) * FIXED_POINT_SHIFT / y;
}


fp1714 divide_x_and_n (fp1714 x, int32_t n)
{
    return x / n;
}

