#ifndef NEMO_TIME_H
#define NEMO_TIME_H
typedef long long time_t;
typedef long long clock_t;
#define CLOCKS_PER_SEC 1000
time_t time(time_t *t);
clock_t clock(void);
#endif
