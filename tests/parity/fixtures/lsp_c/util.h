#ifndef UTIL_H
#define UTIL_H

/* Adds two integers. */
int add(int a, int b);

struct point {
    int x;
    int y;
};

int point_sum(const struct point *p);

#endif
