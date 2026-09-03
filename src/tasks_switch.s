// tasks_switch.s — Nemo OS
//
// Cambio de contexto cooperativo. Segun el ABI estandar de AArch64
// (AAPCS64), una funcion que llama a otra SOLO tiene garantizado que
// los registros x19-x28, x29 (frame pointer), x30 (link register), y
// TAMBIEN d8-d15 (la mitad baja de los registros de coma flotante)
// sobreviven a esa llamada -- el resto (x0-x18, d0-d7, d16-d31) son
// "de usar y tirar" para quien los necesite. Por eso, para "pausar"
// una tarea a mitad de ejecucion y arrancar otra, basta con guardar
// ESOS registros (los unicos que de verdad importan para que la
// tarea pueda continuar exactamente donde lo dejo) y cambiar de pila
// -- exactamente lo mismo que hacen setjmp/longjmp o las corutinas de
// otros lenguajes.
//
// d8-d15 se añadieron cuando el compilador autohospedado (nbc.pro)
// empezo a usar coma flotante de verdad para su propio lexer/AST --
// sin guardarlos aqui, cualquier tarea que usara flotantes y cediera
// el control a mitad de un calculo (via nb_pump/task_yield) podia ver
// su resultado corrompido por lo que OTRA tarea hiciera con esos
// mismos registros mientras tanto.

.section .text
.global task_switch

// void task_switch(uint64_t *old_sp_save, uint64_t new_sp)
//
// Guarda el contexto de la tarea actual (los registros que el ABI
// exige preservar) en SU PROPIA pila, guarda el puntero de pila
// resultante en *old_sp_save, cambia a la pila 'new_sp', y restaura
// desde ahi el contexto que se guardo la ULTIMA VEZ que esa otra
// tarea llamo a task_switch -- con lo que el 'ret' final no vuelve
// aqui, sino a donde esa otra tarea se quedo.
task_switch:
    sub     sp, sp, #160
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    stp     d8,  d9,  [sp, #96]
    stp     d10, d11, [sp, #112]
    stp     d12, d13, [sp, #128]
    stp     d14, d15, [sp, #144]

    mov     x2, sp
    str     x2, [x0]        // *old_sp_save = nuestro sp actual

    mov     sp, x1          // saltamos a la pila de la otra tarea

    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    ldp     d8,  d9,  [sp, #96]
    ldp     d10, d11, [sp, #112]
    ldp     d12, d13, [sp, #128]
    ldp     d14, d15, [sp, #144]
    add     sp, sp, #160

    ret
