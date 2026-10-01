// nemo_setjmp.S — setjmp/longjmp para ARM64, freestanding.
//
// Lua los usa para recuperarse de errores (lua_error / pcall): guarda
// el contexto en setjmp y, si algo falla dentro, longjmp vuelve ahi
// deshaciendo la pila de golpe. Se ensambla con aarch64-elf-gcc.
//
// Guardamos exactamente lo que la ABI ARM64 exige conservar entre
// llamadas: x19-x28, x29 (fp), x30 (lr), sp, y d8-d15 (Lua usa
// flotantes -- si no se guardan, un error dentro de una operacion
// con numeros deja los registros FP corruptos al volver).
// jmp_buf = 22 palabras de 64 bits.

    .text
    .global nemo_setjmp
    .type nemo_setjmp, %function
nemo_setjmp:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x1, sp
    str x1, [x0, #96]
    stp d8,  d9,  [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov x0, #0
    ret

    .global nemo_longjmp
    .type nemo_longjmp, %function
nemo_longjmp:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x2, [x0, #96]
    mov sp, x2
    ldp d8,  d9,  [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    // longjmp(buf, 0) tiene que devolver 1, nunca 0 (regla de C)
    cmp x1, #0
    csinc x0, x1, xzr, ne
    ret
