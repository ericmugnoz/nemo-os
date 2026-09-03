// nb_runtime_nemo.c — implementacion de nb_fatal para cuando el
// compilador corre DENTRO de Nemo OS de verdad (usa las syscalls del
// kernel en vez de fprintf/exit). Nunca se compila junto con
// nb_runtime_host.c -- son alternativas, se elige una u otra segun
// el destino final.
#include "nb_runtime.h"
#include "nb_syscalls.h"
#include "nblibc.h"

void nb_fatal(int line, const char *msg, const char *found) {
    nb_write_string("ERROR");
    if (line > 0) {
        char buf[12];
        nb_itoa(line, buf, sizeof(buf));
        nb_write_string(" (linea ");
        nb_write_string(buf);
        nb_write_string(")");
    }
    nb_write_string(": ");
    nb_write_string(msg);
    if (found) {
        nb_write_string(" -- '");
        nb_write_string(found);
        nb_write_string("'");
    }
    nb_write_string("\n");

    nb_exit();
    for (;;) { nb_pump(); } // por si SYS_EXIT no detiene la tarea de inmediato -- esto SI cede el control (un bucle vacio bloquearia todo el sistema)
}
