// Generado por el compilador Nemo-Blitz -- no editar a mano.
.section .text.start
.global _start
_start:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x0, #0
    str x0, [sp, #-16]!
    adrp x0, var_suma
    add x0, x0, :lo12:var_suma
    ldr x1, [sp], #16
    str x1, [x0]
    mov x0, #0
    mov x1, x0
    adrp x0, arr_numeros
    add x0, x0, :lo12:arr_numeros
    add x0, x0, x1, lsl #3
    str x0, [sp, #-16]!
    mov x0, #10
    ldr x1, [sp], #16
    str x0, [x1]
    mov x0, #1
    mov x1, x0
    adrp x0, arr_numeros
    add x0, x0, :lo12:arr_numeros
    add x0, x0, x1, lsl #3
    str x0, [sp, #-16]!
    mov x0, #20
    ldr x1, [sp], #16
    str x0, [x1]
    mov x0, #2
    mov x1, x0
    adrp x0, arr_numeros
    add x0, x0, :lo12:arr_numeros
    add x0, x0, x1, lsl #3
    str x0, [sp, #-16]!
    mov x0, #30
    ldr x1, [sp], #16
    str x0, [x1]
    adrp x0, var_i
    add x0, x0, :lo12:var_i
    str x0, [sp, #-16]!
    mov x0, #0
    ldr x1, [sp]
    str x0, [x1]
.Lfor_0:
    mov x0, #2
    str x0, [sp, #-16]!
    ldr x1, [sp, #16]
    ldr x0, [x1]
    ldr x2, [sp], #16
    cmp x0, x2
    bgt .Lfor_end_0
    adrp x0, var_suma
    add x0, x0, :lo12:var_suma
    ldr x0, [x0]
    str x0, [sp, #-16]!
    adrp x0, var_i
    add x0, x0, :lo12:var_i
    ldr x0, [x0]
    mov x1, x0
    adrp x0, arr_numeros
    add x0, x0, :lo12:arr_numeros
    add x0, x0, x1, lsl #3
    ldr x0, [x0]
    ldr x1, [sp], #16
    add x0, x1, x0
    str x0, [sp, #-16]!
    adrp x0, var_suma
    add x0, x0, :lo12:var_suma
    ldr x1, [sp], #16
    str x1, [x0]
    ldr x1, [sp]
    ldr x0, [x1]
    add x0, x0, #1
    str x0, [x1]
    b .Lfor_0
.Lfor_end_0:
    add sp, sp, #16
    adrp x0, var_suma
    add x0, x0, :lo12:var_suma
    ldr x0, [x0]
    str x0, [sp, #-16]!
    mov x0, #50
    ldr x1, [sp], #16
    cmp x1, x0
    cset x0, gt
    cmp x0, #0
    beq .Lelse_1
    adrp x0, str_0
    add x0, x0, :lo12:str_0
    str x0, [sp, #-16]!
    adrp x0, var_suma
    add x0, x0, :lo12:var_suma
    ldr x0, [x0]
    bl rt_int_to_str
    ldr x1, [sp], #16
    bl rt_str_concat
    mov x8, #11
    svc #0
    adrp x0, str_1
    add x0, x0, :lo12:str_1
    mov x8, #11
    svc #0
    b .Lend_1
.Lelse_1:
    adrp x0, var_suma
    add x0, x0, :lo12:var_suma
    ldr x0, [x0]
    str x0, [sp, #-16]!
    mov x0, #10
    ldr x1, [sp], #16
    cmp x1, x0
    cset x0, gt
    cmp x0, #0
    beq .Lelseif_2
    adrp x0, str_2
    add x0, x0, :lo12:str_2
    mov x8, #11
    svc #0
    adrp x0, str_1
    add x0, x0, :lo12:str_1
    mov x8, #11
    svc #0
    b .Lend_1
.Lelseif_2:
    adrp x0, str_3
    add x0, x0, :lo12:str_3
    mov x8, #11
    svc #0
    adrp x0, str_1
    add x0, x0, :lo12:str_1
    mov x8, #11
    svc #0
.Lend_1:
    mov x0, #0
    str x0, [sp, #-16]!
    adrp x0, var_contador
    add x0, x0, :lo12:var_contador
    ldr x1, [sp], #16
    str x1, [x0]
.Lwhile_3:
    adrp x0, var_contador
    add x0, x0, :lo12:var_contador
    ldr x0, [x0]
    str x0, [sp, #-16]!
    mov x0, #3
    ldr x1, [sp], #16
    cmp x1, x0
    cset x0, lt
    cmp x0, #0
    beq .Lwhile_end_3
    adrp x0, str_4
    add x0, x0, :lo12:str_4
    str x0, [sp, #-16]!
    adrp x0, var_contador
    add x0, x0, :lo12:var_contador
    ldr x0, [x0]
    bl rt_int_to_str
    ldr x1, [sp], #16
    bl rt_str_concat
    mov x8, #11
    svc #0
    adrp x0, str_1
    add x0, x0, :lo12:str_1
    mov x8, #11
    svc #0
    adrp x0, var_contador
    add x0, x0, :lo12:var_contador
    ldr x0, [x0]
    str x0, [sp, #-16]!
    mov x0, #1
    ldr x1, [sp], #16
    add x0, x1, x0
    str x0, [sp, #-16]!
    adrp x0, var_contador
    add x0, x0, :lo12:var_contador
    ldr x1, [sp], #16
    str x1, [x0]
    b .Lwhile_3
.Lwhile_end_3:
    mov x0, #0
    str x0, [sp, #-16]!
    adrp x0, var_r
    add x0, x0, :lo12:var_r
    ldr x1, [sp], #16
    str x1, [x0]
.Lrepeat_4:
    adrp x0, var_r
    add x0, x0, :lo12:var_r
    ldr x0, [x0]
    str x0, [sp, #-16]!
    mov x0, #1
    ldr x1, [sp], #16
    add x0, x1, x0
    str x0, [sp, #-16]!
    adrp x0, var_r
    add x0, x0, :lo12:var_r
    ldr x1, [sp], #16
    str x1, [x0]
    adrp x0, var_r
    add x0, x0, :lo12:var_r
    ldr x0, [x0]
    str x0, [sp, #-16]!
    mov x0, #4
    ldr x1, [sp], #16
    cmp x1, x0
    cset x0, ge
    cmp x0, #0
    beq .Lrepeat_4
    adrp x0, str_5
    add x0, x0, :lo12:str_5
    str x0, [sp, #-16]!
    adrp x0, var_r
    add x0, x0, :lo12:var_r
    ldr x0, [x0]
    bl rt_int_to_str
    ldr x1, [sp], #16
    bl rt_str_concat
    mov x8, #11
    svc #0
    adrp x0, str_1
    add x0, x0, :lo12:str_1
    mov x8, #11
    svc #0
    adrp x0, var_fila
    add x0, x0, :lo12:var_fila
    str x0, [sp, #-16]!
    mov x0, #1
    ldr x1, [sp]
    str x0, [x1]
.Lfor_5:
    mov x0, #2
    str x0, [sp, #-16]!
    ldr x1, [sp, #16]
    ldr x0, [x1]
    ldr x2, [sp], #16
    cmp x0, x2
    bgt .Lfor_end_5
    adrp x0, var_col
    add x0, x0, :lo12:var_col
    str x0, [sp, #-16]!
    mov x0, #1
    ldr x1, [sp]
    str x0, [x1]
.Lfor_6:
    mov x0, #2
    str x0, [sp, #-16]!
    ldr x1, [sp, #16]
    ldr x0, [x1]
    ldr x2, [sp], #16
    cmp x0, x2
    bgt .Lfor_end_6
    adrp x0, str_6
    add x0, x0, :lo12:str_6
    str x0, [sp, #-16]!
    adrp x0, var_fila
    add x0, x0, :lo12:var_fila
    ldr x0, [x0]
    bl rt_int_to_str
    ldr x1, [sp], #16
    bl rt_str_concat
    str x0, [sp, #-16]!
    adrp x0, str_7
    add x0, x0, :lo12:str_7
    ldr x1, [sp], #16
    bl rt_str_concat
    str x0, [sp, #-16]!
    adrp x0, var_col
    add x0, x0, :lo12:var_col
    ldr x0, [x0]
    bl rt_int_to_str
    ldr x1, [sp], #16
    bl rt_str_concat
    mov x8, #11
    svc #0
    adrp x0, str_1
    add x0, x0, :lo12:str_1
    mov x8, #11
    svc #0
    ldr x1, [sp]
    ldr x0, [x1]
    add x0, x0, #1
    str x0, [x1]
    b .Lfor_6
.Lfor_end_6:
    add sp, sp, #16
    ldr x1, [sp]
    ldr x0, [x1]
    add x0, x0, #1
    str x0, [x1]
    b .Lfor_5
.Lfor_end_5:
    add sp, sp, #16
.Lprogram_end:
    ldp x29, x30, [sp], #16
    ret

// Las dos rutinas de abajo comparten un unico 'pool' rotatorio
// de 8 huecos de 128 bytes cada uno (1024 en total) para sus
// resultados de texto -- huecos de tamaño FIJO a proposito,
// para no tener que calcular avances de longitud variable (mas
// facil de verificar a mano sin poder ensamblar y probar aqui
// mismo). 128 bytes de sobra para cualquier entero de 64 bits
// o concatenacion corta tipica.

// rt_int_to_str: convierte el entero de x0 a texto decimal,
// devuelve el puntero en x0.
rt_int_to_str:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    adrp x9, rt_str_pos
    add x9, x9, :lo12:rt_str_pos
    ldr x10, [x9]
    adrp x11, rt_str_pool
    add x11, x11, :lo12:rt_str_pool
    add x11, x11, x10
    add x10, x10, #128
    and x10, x10, #1023
    str x10, [x9]
    mov x12, x11
    mov x13, #0
    cmp x0, #0
    bge 1f
    mov x13, #1
    neg x0, x0
1:
    mov x14, #10
2:
    udiv x15, x0, x14
    msub x16, x15, x14, x0
    add x16, x16, #48
    strb w16, [x12], #1
    mov x0, x15
    cbnz x0, 2b
    cbz x13, 3f
    mov w16, #45
    strb w16, [x12], #1
3:
    mov w16, #0
    strb w16, [x12]
    mov x0, x11
    sub x1, x12, #1
4:
    cmp x0, x1
    bge 5f
    ldrb w2, [x0]
    ldrb w3, [x1]
    strb w3, [x0], #1
    strb w2, [x1], #-1
    b 4b
5:
    mov x0, x11
    ldp x29, x30, [sp], #16
    ret

// rt_str_concat: concatena la cadena de x1 con la de x0,
// devuelve el puntero al resultado en x0. Usa registros
// x9-x15 como bloc de notas (no hace falta guardarlos: esta
// rutina no llama a nada mas por dentro, asi que el ABI no
// exige preservarlos para quien nos llamo).
rt_str_concat:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x9, x1
    mov x10, x0
    adrp x11, rt_str_pos
    add x11, x11, :lo12:rt_str_pos
    ldr x12, [x11]
    adrp x13, rt_str_pool
    add x13, x13, :lo12:rt_str_pool
    add x13, x13, x12
    add x12, x12, #128
    and x12, x12, #1023
    str x12, [x11]
    mov x14, x13
rt_concat_copy_a:
    ldrb w15, [x9], #1
    cbz w15, rt_concat_copy_a_done
    strb w15, [x14], #1
    b rt_concat_copy_a
rt_concat_copy_a_done:
rt_concat_copy_b:
    ldrb w15, [x10], #1
    strb w15, [x14], #1
    cbnz w15, rt_concat_copy_b
    mov x0, x13
    ldp x29, x30, [sp], #16
    ret

// rt_draw_line: dibuja una linea de (x0,x1) a (x2,x3) color x4,
// interpolando el mayor de los dos ejes y llamando a
// SYS_DRAW_RECT una vez por punto (sencillo, no el algoritmo
// de Bresenham de verdad, pero correcto y facil de revisar).
rt_draw_line:
    stp x29, x30, [sp, #-64]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    mov x19, x0
    mov x20, x1
    mov x21, x2
    mov x22, x3
    mov x23, x4
    subs x24, x21, x19
    cneg x24, x24, mi
    subs x9, x22, x20
    cneg x9, x9, mi
    cmp x24, x9
    bge .Lline_horiz
    b .Lline_vert
.Lline_horiz:
    cmp x19, x21
    ble .Lline_h_fwd
    mov x9, x19
    mov x19, x21
    mov x21, x9
    mov x9, x20
    mov x20, x22
    mov x22, x9
.Lline_h_fwd:
    subs x24, x21, x19
    mov x9, #0
6:
    cmp x19, x21
    bgt .Lline_done
    cbz x24, .Lline_h_single
    sub x10, x22, x20
    mul x10, x10, x9
    sdiv x10, x10, x24
    add x10, x20, x10
    b .Lline_h_plot
.Lline_h_single:
    mov x10, x20
.Lline_h_plot:
    mov x0, x19
    mov x1, x10
    mov x2, #1
    mov x3, #1
    mov x4, x23
    mov x8, #30
    svc #0
    add x19, x19, #1
    add x9, x9, #1
    b 6b
.Lline_vert:
    cmp x20, x22
    ble .Lline_v_fwd
    mov x9, x19
    mov x19, x21
    mov x21, x9
    mov x9, x20
    mov x20, x22
    mov x22, x9
.Lline_v_fwd:
    subs x9, x22, x20
    mov x11, #0
7:
    cmp x20, x22
    bgt .Lline_done
    cbz x9, .Lline_v_single
    sub x10, x21, x19
    mul x10, x10, x11
    sdiv x10, x10, x9
    add x10, x19, x10
    b .Lline_v_plot
.Lline_v_single:
    mov x10, x19
.Lline_v_plot:
    mov x0, x10
    mov x1, x20
    mov x2, #1
    mov x3, #1
    mov x4, x23
    mov x8, #30
    svc #0
    add x20, x20, #1
    add x11, x11, #1
    b 7b
.Lline_done:
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x29, x30, [sp], #64
    ret

.section .rodata
str_0: .asciz "Suma grande: "
str_1: .asciz "\n"
str_2: .asciz "Suma media"
str_3: .asciz "Suma pequeña"
str_4: .asciz "While: "
str_5: .asciz "Repeat termino en: "
str_6: .asciz "Celda "
str_7: .asciz ","

.section .bss
.align 3
var_suma: .space 8
var_i: .space 8
var_contador: .space 8
var_r: .space 8
var_fila: .space 8
var_col: .space 8
arr_numeros: .space 512
rt_str_pool: .space 1024
rt_str_pos: .space 8
