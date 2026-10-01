// otros_programas.c -- ver otros_programas.h
//
// Lee el paquete que genera herramientas/empaquetar_otros.py y lo vuelca en
// NemoFS. El formato esta descrito en ese script; aqui solo se recuerda lo
// justo para seguir el codigo:
//
//   "NPAK" | version u32 | n u32 | { len_ruta u16, ruta, len_datos u32, datos }
//
// Todo little-endian y sin relleno. Las rutas vienen con '/' y pueden llevar
// espacios ("nemo plataforma demo/juego1.nb"): quien las parte es este
// archivo, creando por el camino las carpetas que falten.

#include <stdint.h>
#include <stdbool.h>

#include "otros_programas.h"
#include "nemofs.h"
#include "uart.h"

// El blob: lo mete objcopy a partir de build_otros/otros.npak.
//
// La guarda OTROS_PAQUETE_EXTERNO permite incluir este archivo desde un
// programa de host y pasarle el paquete desde un fichero, en vez del blob que
// mete objcopy. Sirve para desempaquetar y examinar un .npak en el ordenador
// de desarrollo usando EL MISMO codigo que lo lee dentro del sistema: una
// copia del formato escrita aparte se quedaria atras a la primera.
#ifndef OTROS_PAQUETE_EXTERNO
extern const uint8_t _binary_otros_npak_start[];
extern const uint8_t _binary_otros_npak_end[];
#endif

#define CARPETA_RAIZ "otros programas"

// Lectura sin suponer alineacion: el paquete se recorre byte a byte y los
// campos caen donde caigan. Un u32 leido de golpe sobre una direccion impar
// es justo el tipo de fallo que solo aparece en la placa.
static uint32_t leer_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t leer_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// Busca o crea una carpeta hija. Igual que nemofs_ensure_dir de kernel.c: en
// el segundo arranque ya existe, y crearla a secas fallaria siempre.
static int32_t carpeta(uint32_t padre, const char *nombre) {
    int32_t idx = nemofs_find_child(padre, nombre);
    if (idx >= 0) return idx;
    return nemofs_create(padre, nombre, NEMOFS_TYPE_DIR);
}

static void uart_dec(uint32_t v) {
    char b[11];
    int i = 0;
    if (v == 0) { uart_puts("0"); return; }
    while (v > 0 && i < (int)sizeof b - 1) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    char s[12];
    int k = 0;
    while (i > 0) s[k++] = b[--i];
    s[k] = '\0';
    uart_puts(s);
}

// Instala UNA entrada: crea las carpetas intermedias de su ruta y escribe el
// archivo. Devuelve false y dice por que si algo falla, sin abortar el resto:
// que un programa no quepa no es razon para quedarse sin los demas.
static bool instalar_entrada(uint32_t raiz, const char *ruta, uint16_t ruta_len,
                             const uint8_t *datos, uint32_t datos_len) {
    uint32_t padre = raiz;
    uint16_t i = 0;
    char trozo[NEMOFS_MAX_NAME + 1];

    while (i < ruta_len) {
        uint16_t fin = i;
        while (fin < ruta_len && ruta[fin] != '/') fin++;
        uint16_t largo = (uint16_t)(fin - i);
        if (largo == 0 || largo > NEMOFS_MAX_NAME) return false;  // el packer ya lo comprueba
        for (uint16_t k = 0; k < largo; k++) trozo[k] = ruta[i + k];
        trozo[largo] = '\0';

        if (fin < ruta_len) {                 // queda ruta: es una carpeta
            int32_t dir = carpeta(padre, trozo);
            if (dir < 0) return false;
            padre = (uint32_t)dir;
            i = (uint16_t)(fin + 1);
            continue;
        }

        // Ultimo trozo: el archivo.
        int32_t inode = nemofs_find_child(padre, trozo);
        if (inode < 0) inode = nemofs_create(padre, trozo, NEMOFS_TYPE_FILE);
        if (inode < 0) return false;
        return nemofs_write_file_if_changed((uint32_t)inode, datos, datos_len);
    }
    return false;
}

void otros_programas_instalar(void) {
    const uint8_t *p = _binary_otros_npak_start;
    const uint8_t *fin = _binary_otros_npak_end;

    if ((uint32_t)(fin - p) < 12) return;   // paquete vacio: nada que hacer
    if (p[0] != 'N' || p[1] != 'P' || p[2] != 'A' || p[3] != 'K') {
        uart_puts("otros programas: el paquete no empieza por NPAK, no se instala\n");
        return;
    }
    if (leer_u32(p + 4) != 1u) {
        uart_puts("otros programas: version de paquete desconocida, no se instala\n");
        return;
    }
    uint32_t n = leer_u32(p + 8);
    p += 12;

    int32_t raiz = carpeta(NEMOFS_ROOT_INODE, CARPETA_RAIZ);
    if (raiz < 0) {
        uart_puts("otros programas: no se pudo crear la carpeta, no se instalan\n");
        return;
    }

    uint32_t puestos = 0;
    for (uint32_t e = 0; e < n; e++) {
        if ((uint32_t)(fin - p) < 2) break;
        uint16_t ruta_len = leer_u16(p);
        p += 2;
        if ((uint32_t)(fin - p) < ruta_len) break;
        const char *ruta = (const char *)p;
        p += ruta_len;
        if ((uint32_t)(fin - p) < 4) break;
        uint32_t datos_len = leer_u32(p);
        p += 4;
        if ((uint32_t)(fin - p) < datos_len) break;
        const uint8_t *datos = p;
        p += datos_len;

        if (instalar_entrada((uint32_t)raiz, ruta, ruta_len, datos, datos_len)) {
            puestos++;
        } else {
            uart_puts("otros programas: fallo instalando ");
            for (uint16_t k = 0; k < ruta_len; k++) {
                char c[2] = { ruta[k], 0 };
                uart_puts(c);
            }
            uart_puts("\n");
        }
    }

    // La cuenta siempre, como en embedded_lua: "instalados" a secas pasara lo
    // que pasara es justo lo que impide ver que algo se quedo fuera.
    uart_puts("otros programas: ");
    uart_dec(puestos);
    uart_puts(" de ");
    uart_dec(n);
    uart_puts(" archivos en /" CARPETA_RAIZ);
    uart_puts(puestos == n ? " (todos).\n" : " -- FALTAN.\n");
}
