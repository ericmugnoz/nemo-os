// rtc.c — Nemo OS
#include "rtc.h"

#define PL031_BASE 0x09010000UL

uint32_t rtc_unix_timestamp(void) {
    volatile uint32_t *dr = (volatile uint32_t *)PL031_BASE;
    return *dr;
}

// civil_from_days: dias desde 1970-01-01 -> año/mes/dia. Algoritmo de
// Howard Hinnant (dominio publico), el estandar de facto para esta
// conversion sin depender de tablas ni de <time.h>.
static void civil_from_days(long z, int *year, int *month, int *day) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - era * 146097);
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long mp = (5 * doy + 2) / 153;
    unsigned long d = doy - (153 * mp + 2) / 5 + 1;
    unsigned long m = mp + (mp < 10 ? 3 : (unsigned long)-9);
    y += (m <= 2) ? 1 : 0;
    *year = (int)y;
    *month = (int)m;
    *day = (int)d;
}

void rtc_to_civil(uint32_t ts, int *year, int *month, int *day, int *hour, int *minute, int *second) {
    long days = (long)(ts / 86400);
    uint32_t secs_of_day = ts % 86400;
    *hour = (int)(secs_of_day / 3600);
    *minute = (int)((secs_of_day % 3600) / 60);
    *second = (int)(secs_of_day % 60);
    civil_from_days(days, year, month, day);
}
