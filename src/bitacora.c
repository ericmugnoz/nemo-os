// bitacora.c — Nemo OS. Ver bitacora.h.
#include "bitacora.h"
#include "fat.h"

// 64 KB. El arranque con el USB hablando suele quedarse en unos pocos
// miles de caracteres, asi que sobra; y 64 KB de memoria estatica en
// una placa con gigabytes no es una decision que haya que pensar.
//
// Es estatica A PROPOSITO, no del monton: la bitacora tiene que estar
// viva desde el PRIMER caracter del arranque, mucho antes de que
// exista un asignador de memoria. Si dependiera del monton, se
// perderia justo la parte que mas interesa.
#define BITACORA_MAX (64u * 1024u)

static char  buf[BITACORA_MAX];
static uint32_t largo = 0;
static bool  lleno = false;
static bool  volcando = false;

void bitacora_anadir(char c) {
    // Mientras se escribe el archivo, el propio driver de FAT puede
    // sacar mensajes por el UART. Sin esta guarda, esos mensajes
    // entrarian en el bufer que se esta volcando: el archivo crece
    // mientras se copia y la cuenta deja de cuadrar.
    if (volcando) return;
    if (largo >= BITACORA_MAX) { lleno = true; return; }
    buf[largo++] = c;
}

uint32_t bitacora_largo(void) { return largo; }
bool bitacora_se_lleno(void)  { return lleno; }

bool bitacora_volcar(void) {
    if (largo == 0) return false;
    volcando = true;

    // Si se lleno, se deja constancia EN EL PROPIO ARCHIVO. Un
    // registro cortado sin avisar hace perder el tiempo a quien lo
    // lee buscando el final que nunca llego.
    if (lleno) {
        const char *aviso = "\r\n*** BITACORA LLENA: a partir de aqui falta texto ***\r\n";
        uint32_t i = 0;
        // se escribe sobre el final del bufer, que es lo mas viejo que
        // se puede sacrificar sin perder el arranque
        uint32_t donde = BITACORA_MAX - 64;
        while (aviso[i] && donde + i < BITACORA_MAX) { buf[donde + i] = aviso[i]; i++; }
    }

    bool ok = fat_write_file("NEMO.LOG", buf, largo);
    volcando = false;
    return ok;
}
