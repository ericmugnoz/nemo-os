// nemo_reloc.c — autorreubicacion del programa al arrancar.
//
// LA LECCION IMPORTANTE de Nemo OS, resuelta de raiz: un .pro se
// enlaza en la direccion 0 y el loader lo copia a otro sitio sin
// reubicar nada, asi que cualquier PUNTERO guardado como DATO (una
// tabla de punteros a funcion, una cadena en un array de char*) queda
// apuntando a la direccion de enlazado, invalida. Los programas en C
// del sistema lo esquivaban por disciplina (nunca una tabla de
// punteros). Lua tiene cientos -- imposible esquivarlo.
//
// Solucion, la misma que usa el kernel de Linux en ARM64: compilar con
// -fPIE y enlazar con -pie, que deja en el binario una lista de
// "reubicaciones relativas" (.rela.dyn): cada entrada dice "en el
// desplazamiento X hay un puntero que vale Y respecto a la base".
// Al arrancar, antes de tocar nada, _start calcula donde ha caido de
// verdad el programa y suma esa base a cada puntero de la lista.
//
// Este archivo se ejecuta ANTES de la reubicacion: no puede usar
// ningun puntero guardado en datos ni pasar por la GOT. Por eso los
// simbolos del linker script se declaran 'hidden' (el compilador los
// direcciona relativo al PC) y el bucle es aritmetica pura.

#include <stdint.h>

typedef struct { uint64_t r_offset; uint64_t r_info; int64_t r_addend; } Elf64_Rela;

#if defined(__aarch64__)
#define R_RELATIVE 1027   /* R_AARCH64_RELATIVE */
#elif defined(__x86_64__)
#define R_RELATIVE 8      /* R_X86_64_RELATIVE */
#else
#error "arquitectura sin soporte de reubicacion"
#endif

extern const Elf64_Rela __rela_start[] __attribute__((visibility("hidden")));
extern const Elf64_Rela __rela_end[]   __attribute__((visibility("hidden")));

uint64_t nemo_reloc_base;
__attribute__((used))
void nemo_relocate(uint64_t base) {
    nemo_reloc_base = base;
    for (const Elf64_Rela *r = __rela_start; r < __rela_end; r++) {
        if ((uint32_t)r->r_info == R_RELATIVE) {
            *(uint64_t *)(base + r->r_offset) = base + (uint64_t)r->r_addend;
        }
    }
}
