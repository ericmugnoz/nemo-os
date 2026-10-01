// nmz.c -- Nemo OS. Ver nmz.h.

#include "nmz.h"

static const char *motivo = "";
const char *nmz_motivo(void) { return motivo; }

static uint32_t leer32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t leer16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool nmz_nombre_seguro(const char *n, uint32_t largo) {
    if (largo == 0 || largo > NMZ_MAX_NOMBRE) return false;
    for (uint32_t i = 0; i < largo; i++) {
        char c = n[i];
        // Cualquier separador de carpetas convierte un nombre en una
        // ruta, y una ruta convierte "escribe este archivo aqui" en
        // "escribe este archivo donde yo diga".
        if (c == '/' || c == '\\' || c == ':') return false;
        // Los de control no pintan nada en un nombre y sirven para
        // disimular: un nombre con un retorno de carro dentro se ve
        // distinto en un listado que en el disco.
        if ((unsigned char)c < 32 || (unsigned char)c == 127) return false;
    }
    // Ni "." ni ".." ni nada que empiece por dos puntos seguidos.
    if (n[0] == '.' && (largo == 1 || (largo == 2 && n[1] == '.'))) return false;
    for (uint32_t i = 0; i + 1 < largo; i++)
        if (n[i] == '.' && n[i + 1] == '.') return false;
    return true;
}

int32_t nmz_desempaquetar(const nmz_io_t *io, uint32_t tam_paquete,
                          char *primero, uint32_t primero_max) {
    motivo = "";
    uint8_t cab[8];
    if (tam_paquete < 8 || io->leer(io->ctx, 0, cab, 8) != 8) {
        motivo = "el paquete esta cortado";
        return -1;
    }
    if (cab[0] != 'N' || cab[1] != 'M' || cab[2] != 'Z' || cab[3] != '1') {
        motivo = "no es un paquete NMZ";
        return -1;
    }
    uint32_t n = leer32(cab + 4);
    if (n == 0 || n > NMZ_MAX_ARCHIVOS) {
        motivo = "numero de archivos absurdo";
        return -1;
    }

    // --- Primera pasada: leer la tabla entera y comprobarla ---
    //
    // Se valida TODO antes de escribir nada. Si se fuera escribiendo
    // sobre la marcha, un paquete que resulta estar mal a la mitad
    // dejaria media pagina en el disco -- archivos sueltos que nadie
    // sabe de donde salieron y que el visor intentaria abrir.
    struct { char nombre[NMZ_MAX_NOMBRE + 1]; uint32_t largo, tam, off; } tabla[NMZ_MAX_ARCHIVOS];

    uint32_t pos = 8;
    uint64_t suma = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b[4];
        if (io->leer(io->ctx, pos, b, 2) != 2) { motivo = "tabla cortada"; return -1; }
        uint32_t largo = leer16(b);
        pos += 2;
        if (largo == 0 || largo > NMZ_MAX_NOMBRE) { motivo = "nombre de largo imposible"; return -1; }
        if (io->leer(io->ctx, pos, tabla[i].nombre, largo) != (int32_t)largo) {
            motivo = "tabla cortada"; return -1;
        }
        tabla[i].nombre[largo] = 0;
        tabla[i].largo = largo;
        pos += largo;
        if (io->leer(io->ctx, pos, b, 4) != 4) { motivo = "tabla cortada"; return -1; }
        tabla[i].tam = leer32(b);
        pos += 4;

        if (!nmz_nombre_seguro(tabla[i].nombre, largo)) {
            motivo = "un nombre del paquete no es seguro";
            return -1;
        }
        if (tabla[i].tam > NMZ_MAX_ARCHIVO) { motivo = "un archivo es demasiado grande"; return -1; }
        suma += tabla[i].tam;
    }

    // Dos nombres iguales: el segundo pisaria al primero y el paquete
    // quedaria con menos archivos de los que dice. Con 64 como tope,
    // comparar todos contra todos son 2016 comparaciones: nada.
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i + 1; j < n; j++) {
            if (tabla[i].largo != tabla[j].largo) continue;
            uint32_t k = 0;
            while (k < tabla[i].largo && tabla[i].nombre[k] == tabla[j].nombre[k]) k++;
            if (k == tabla[i].largo) { motivo = "hay dos archivos con el mismo nombre"; return -1; }
        }

    // Los contenidos empiezan justo detras de la tabla, pegados.
    uint32_t off = pos;
    for (uint32_t i = 0; i < n; i++) { tabla[i].off = off; off += tabla[i].tam; }

    if ((uint64_t)pos + suma > (uint64_t)tam_paquete) {
        motivo = "la tabla promete mas bytes de los que hay";
        return -1;
    }

    // --- Segunda pasada: escribir ---
    uint8_t buf[4096];
    for (uint32_t i = 0; i < n; i++) {
        int32_t id = io->crear(io->ctx, tabla[i].nombre);
        if (id < 0) { motivo = "no se pudo crear un archivo"; return -1; }
        uint32_t queda = tabla[i].tam, desde = tabla[i].off;
        while (queda > 0) {
            uint32_t trozo = queda < sizeof buf ? queda : (uint32_t)sizeof buf;
            if (io->leer(io->ctx, desde, buf, trozo) != (int32_t)trozo) {
                motivo = "el paquete se corto al copiar"; return -1;
            }
            if (!io->escribir(io->ctx, id, buf, trozo)) {
                motivo = "no se pudo escribir (disco lleno?)"; return -1;
            }
            desde += trozo; queda -= trozo;
        }
    }

    if (primero && primero_max > 0) {
        uint32_t k = 0;
        while (k + 1 < primero_max && tabla[0].nombre[k]) { primero[k] = tabla[0].nombre[k]; k++; }
        primero[k] = 0;
    }
    return (int32_t)n;
}
