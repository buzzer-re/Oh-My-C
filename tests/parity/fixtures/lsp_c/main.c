#include <stdio.h>
#include "util.h"

int main(void) {
    struct point p = {1, 2};
    int total = add(3, 4);
    int unused_var = 5;
    printf("%d %d\n", total, point_sum(&p));
    return missing_symbol;
}
