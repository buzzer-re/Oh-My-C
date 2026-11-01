#include "util.h"

int add(int a, int b) {
    return a + b;
}

int point_sum(const struct point *p) {
    return add(p->x, p->y);
}
