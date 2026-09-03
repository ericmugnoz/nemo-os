// rtc.h — Nemo OS
//
// Reloj de tiempo real -- el PL031 de la maquina "virt" de QEMU, en
// 0x09010000. Es un unico registro de 32 bits (RTCDR) que da
// directamente el timestamp Unix actual (segundos desde 1970-01-01
// UTC) -- no hace falta manejar interrupciones para una simple
// consulta de "que hora es ahora".
#ifndef RTC_H
#define RTC_H
#include <stdint.h>

uint32_t rtc_unix_timestamp(void);

// Convierte un timestamp Unix a año/mes/dia/hora/minuto/segundo (UTC
// siempre, no hay zona horaria que aplicar). Algoritmo de calendario
// civil estandar (Howard Hinnant, dominio publico) para dias->fecha.
void rtc_to_civil(uint32_t ts, int *year, int *month, int *day, int *hour, int *minute, int *second);

#endif
