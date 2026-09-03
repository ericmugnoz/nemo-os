// Generado por el compilador Nemo-Blitz -- no editar a mano.
.section .text.start
.global _start
_start:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x8, #2
    svc #0
    adrp x9, rt_rnd_seed
    add x9, x9, :lo12:rt_rnd_seed
    str x0, [x9]
    mov x9, #0xFFFFFF
    adrp x10, rt_current_color
    add x10, x10, :lo12:rt_current_color
    str x9, [x10]
    adrp x9, rt_data_table
    add x9, x9, :lo12:rt_data_table
    adrp x10, rt_data_ptr
    add x10, x10, :lo12:rt_data_ptr
    str x9, [x10]
    mov x0, #400
    str x0, [sp, #-16]!
    mov x0, #300
    mov x1, x0
    ldr x0, [sp], #16
    mov x8, #46
    svc #0
    mov x0, #0
    mov x0, #50
    str x0, [sp, #-16]!
    adrp x0, var_x
    add x0, x0, :lo12:var_x
    ldr x1, [sp], #16
    str x1, [x0]
    mov x0, #50
    str x0, [sp, #-16]!
    adrp x0, var_y
    add x0, x0, :lo12:var_y
    ldr x1, [sp], #16
    str x1, [x0]
    mov x0, #3
    str x0, [sp, #-16]!
    adrp x0, var_speed
    add x0, x0, :lo12:var_speed
    ldr x1, [sp], #16
    str x1, [x0]
.Lrepeat_0:
    mov x0, #0
    str x0, [sp, #-16]!
    mov x0, #0
    str x0, [sp, #-16]!
    mov x0, #60
    and x3, x0, #0xFF
    ldr x2, [sp], #16
    and x2, x2, #0xFF
    lsl x2, x2, #8
    ldr x1, [sp], #16
    and x1, x1, #0xFF
    lsl x1, x1, #16
    orr x0, x1, x2
    orr x0, x0, x3
    adrp x9, rt_current_color
    add x9, x9, :lo12:rt_current_color
    str x0, [x9]
    mov x0, #0
    mov x1, #0
    mov x2, #4096
    mov x3, #4096
    adrp x9, rt_current_color
    add x9, x9, :lo12:rt_current_color
    ldr x4, [x9]
    mov x8, #30
    svc #0
    mov x0, #30
    mov x8, #48
    svc #0
    cmp x0, #0
    beq .Lelse_1
    adrp x0, var_x
    add x0, x0, :lo12:var_x
    ldr x0, [x0]
    str x0, [sp, #-16]!
    adrp x0, var_speed
    add x0, x0, :lo12:var_speed
    ldr x0, [x0]
    ldr x1, [sp], #16
    sub x0, x1, x0
    str x0, [sp, #-16]!
    adrp x0, var_x
    add x0, x0, :lo12:var_x
    ldr x1, [sp], #16
    str x1, [x0]
    b .Lend_1
.Lelse_1:
.Lend_1:
    mov x0, #32
    mov x8, #48
    svc #0
    cmp x0, #0
    beq .Lelse_2
    adrp x0, var_x
    add x0, x0, :lo12:var_x
    ldr x0, [x0]
    str x0, [sp, #-16]!
    adrp x0, var_speed
    add x0, x0, :lo12:var_speed
    ldr x0, [x0]
    ldr x1, [sp], #16
    add x0, x1, x0
    str x0, [sp, #-16]!
    adrp x0, var_x
    add x0, x0, :lo12:var_x
    ldr x1, [sp], #16
    str x1, [x0]
    b .Lend_2
.Lelse_2:
.Lend_2:
    mov x0, #17
    mov x8, #48
    svc #0
    cmp x0, #0
    beq .Lelse_3
    adrp x0, var_y
    add x0, x0, :lo12:var_y
    ldr x0, [x0]
    str x0, [sp, #-16]!
    adrp x0, var_speed
    add x0, x0, :lo12:var_speed
    ldr x0, [x0]
    ldr x1, [sp], #16
    sub x0, x1, x0
    str x0, [sp, #-16]!
    adrp x0, var_y
    add x0, x0, :lo12:var_y
    ldr x1, [sp], #16
    str x1, [x0]
    b .Lend_3
.Lelse_3:
.Lend_3:
    mov x0, #31
    mov x8, #48
    svc #0
    cmp x0, #0
    beq .Lelse_4
    adrp x0, var_y
    add x0, x0, :lo12:var_y
    ldr x0, [x0]
    str x0, [sp, #-16]!
    adrp x0, var_speed
    add x0, x0, :lo12:var_speed
    ldr x0, [x0]
    ldr x1, [sp], #16
    add x0, x1, x0
    str x0, [sp, #-16]!
    adrp x0, var_y
    add x0, x0, :lo12:var_y
    ldr x1, [sp], #16
    str x1, [x0]
    b .Lend_4
.Lelse_4:
.Lend_4:
    mov x0, #255
    str x0, [sp, #-16]!
    mov x0, #200
    str x0, [sp, #-16]!
    mov x0, #0
    and x3, x0, #0xFF
    ldr x2, [sp], #16
    and x2, x2, #0xFF
    lsl x2, x2, #8
    ldr x1, [sp], #16
    and x1, x1, #0xFF
    lsl x1, x1, #16
    orr x0, x1, x2
    orr x0, x0, x3
    adrp x9, rt_current_color
    add x9, x9, :lo12:rt_current_color
    str x0, [x9]
    adrp x0, var_x
    add x0, x0, :lo12:var_x
    ldr x0, [x0]
    str x0, [sp, #-16]!
    adrp x0, var_y
    add x0, x0, :lo12:var_y
    ldr x0, [x0]
    str x0, [sp, #-16]!
    mov x0, #30
    str x0, [sp, #-16]!
    mov x0, #30
    mov x3, x0
    ldr x2, [sp], #16
    ldr x1, [sp], #16
    ldr x0, [sp], #16
    adrp x9, rt_current_color
    add x9, x9, :lo12:rt_current_color
    ldr x4, [x9]
    mov x8, #47
    svc #0
    mov x0, #255
    str x0, [sp, #-16]!
    mov x0, #255
    str x0, [sp, #-16]!
    mov x0, #255
    and x3, x0, #0xFF
    ldr x2, [sp], #16
    and x2, x2, #0xFF
    lsl x2, x2, #8
    ldr x1, [sp], #16
    and x1, x1, #0xFF
    lsl x1, x1, #16
    orr x0, x1, x2
    orr x0, x0, x3
    adrp x9, rt_current_color
    add x9, x9, :lo12:rt_current_color
    str x0, [x9]
    mov x0, #10
    str x0, [sp, #-16]!
    mov x0, #10
    str x0, [sp, #-16]!
    adrp x0, str_0
    add x0, x0, :lo12:str_0
    str x0, [sp, #-16]!
    adrp x0, var_x
    add x0, x0, :lo12:var_x
    ldr x0, [x0]
    bl rt_int_to_str
    ldr x1, [sp], #16
    bl rt_str_concat
    str x0, [sp, #-16]!
    adrp x0, str_1
    add x0, x0, :lo12:str_1
    ldr x1, [sp], #16
    bl rt_str_concat
    str x0, [sp, #-16]!
    adrp x0, var_y
    add x0, x0, :lo12:var_y
    ldr x0, [x0]
    bl rt_int_to_str
    ldr x1, [sp], #16
    bl rt_str_concat
    mov x2, x0
    ldr x1, [sp], #16
    ldr x0, [sp], #16
    adrp x9, rt_current_color
    add x9, x9, :lo12:rt_current_color
    ldr x3, [x9]
    mov x8, #31
    svc #0
    mov x0, #10
    str x0, [sp, #-16]!
    mov x0, #24
    str x0, [sp, #-16]!
    adrp x0, str_2
    add x0, x0, :lo12:str_2
    str x0, [sp, #-16]!
    mov x8, #2
    svc #0
    mov x1, #10
    mul x0, x0, x1
    bl rt_int_to_str
    ldr x1, [sp], #16
    bl rt_str_concat
    mov x2, x0
    ldr x1, [sp], #16
    ldr x0, [sp], #16
    adrp x9, rt_current_color
    add x9, x9, :lo12:rt_current_color
    ldr x3, [x9]
    mov x8, #31
    svc #0
    adrp x0, var_x
    add x0, x0, :lo12:var_x
    ldr x0, [x0]
    str x0, [sp, #-16]!
    adrp x0, var_y
    add x0, x0, :lo12:var_y
    ldr x0, [x0]
    str x0, [sp, #-16]!
    mov x0, #30
    str x0, [sp, #-16]!
    mov x0, #30
    str x0, [sp, #-16]!
    mov x0, #150
    str x0, [sp, #-16]!
    mov x0, #150
    str x0, [sp, #-16]!
    mov x0, #60
    str x0, [sp, #-16]!
    mov x0, #60
    mov x7, x0
    ldr x6, [sp], #16
    ldr x5, [sp], #16
    ldr x4, [sp], #16
    ldr x3, [sp], #16
    ldr x2, [sp], #16
    ldr x1, [sp], #16
    ldr x0, [sp], #16
    add x8, x4, x6
    cmp x0, x8
    bge .Lro_false_6
    add x8, x0, x2
    cmp x4, x8
    bge .Lro_false_6
    add x8, x5, x7
    cmp x1, x8
    bge .Lro_false_6
    add x8, x1, x3
    cmp x5, x8
    bge .Lro_false_6
    mov x0, #1
    b .Lro_done_6
.Lro_false_6:
    mov x0, #0
.Lro_done_6:
    cmp x0, #0
    beq .Lelse_5
    mov x0, #10
    str x0, [sp, #-16]!
    mov x0, #40
    str x0, [sp, #-16]!
    adrp x0, str_3
    add x0, x0, :lo12:str_3
    mov x2, x0
    ldr x1, [sp], #16
    ldr x0, [sp], #16
    adrp x9, rt_current_color
    add x9, x9, :lo12:rt_current_color
    ldr x3, [x9]
    mov x8, #31
    svc #0
    b .Lend_5
.Lelse_5:
.Lend_5:
    mov x8, #14
    svc #0
    mov x0, #1
    mov x8, #48
    svc #0
    cmp x0, #0
    beq .Lrepeat_0
.Lrepeat_end_0:
    b .Lprogram_end
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

// rt_float_to_str: x0=patron de bits de un double -- lo
// convierte a texto decimal con hasta 6 cifras despues del
// punto (recortando ceros sobrantes). Reutiliza rt_int_to_str
// para la parte entera (con su signo), y calcula la parte
// fraccionaria por separado, redondeando al sumar 0.5 antes
// de truncar.
rt_float_to_str:
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    mov x21, x0
    fmov d0, x0
    fcvtzs x0, d0
    bl rt_int_to_str
    mov x19, x0
    fmov d0, x21
    fabs d0, d0
    fcvtzs x9, d0
    scvtf d1, x9
    fsub d0, d0, d1
    adrp x9, rt_half
    add x9, x9, :lo12:rt_half
    ldr x9, [x9]
    fmov d2, x9
    mov x9, #1000000
    scvtf d1, x9
    fmul d0, d0, d1
    fadd d0, d0, d2
    fcvtzs x22, d0
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
67:
    ldrb w13, [x19], #1
    cbz w13, 68f
    strb w13, [x12], #1
    b 67b
68:
    mov w13, #46
    strb w13, [x12], #1
    mov x14, #100000
69:
    udiv x15, x22, x14
    add x16, x15, #48
    strb w16, [x12], #1
    msub x22, x15, x14, x22
    mov x17, #10
    udiv x14, x14, x17
    cbnz x14, 69b
71:
    ldrb w17, [x12, #-1]
    cmp w17, #48
    bne 72f
    sub x12, x12, #1
    b 71b
72:
    ldrb w17, [x12, #-1]
    cmp w17, #46
    bne 73f
    sub x12, x12, #1
73:
    mov w17, #0
    strb w17, [x12]
    mov x0, x11
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x29, x30, [sp], #48
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

// -- funciones de cadenas: Len, Left$, Right$, Mid$, Upper$, Lower$, Chr$, Asc --

// rt_strlen: longitud de la cadena de x0, en x0.
rt_strlen:
    mov x1, x0
    mov x0, #0
20:
    ldrb w2, [x1], #1
    cbz w2, 21f
    add x0, x0, #1
    b 20b
21:
    ret

// rt_left: primeros x1 caracteres de la cadena x0 (o menos, si
// es mas corta). Devuelve el puntero al resultado en x0.
rt_left:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x9, x0
    mov x10, x1
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
22:
    cbz x10, 23f
    ldrb w15, [x9], #1
    cbz w15, 23f
    strb w15, [x14], #1
    sub x10, x10, #1
    b 22b
23:
    mov w15, #0
    strb w15, [x14]
    mov x0, x13
    ldp x29, x30, [sp], #16
    ret

// rt_right: ultimos x1 caracteres de la cadena x0.
rt_right:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    mov x19, x0
    mov x20, x1
    bl rt_strlen
    mov x9, x0
    cmp x20, x9
    bge 24f
    sub x9, x9, x20
    add x19, x19, x9
24:
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
25:
    ldrb w15, [x19], #1
    strb w15, [x14], #1
    cbnz w15, 25b
    mov x0, x13
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

// rt_mid: subcadena de x0 empezando en x1 (base 1), longitud
// x2 (o -1 para 'hasta el final').
rt_mid:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    mov x19, x0
    sub x9, x1, #1
    cmp x9, #0
    bge 26f
    mov x9, #0
26:
27:
    cbz x9, 28f
    ldrb w10, [x19]
    cbz w10, 28f
    add x19, x19, #1
    sub x9, x9, #1
    b 27b
28:
    mov x20, x2
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
29:
    cmp x20, #0
    beq 30f
    ldrb w15, [x19], #1
    cbz w15, 30f
    strb w15, [x14], #1
    sub x20, x20, #1
    b 29b
30:
    mov w15, #0
    strb w15, [x14]
    mov x0, x13
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

// rt_upper / rt_lower: copia de x0 en mayusculas/minusculas.
rt_upper:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x9, x0
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
31:
    ldrb w15, [x9], #1
    cbz w15, 32f
    cmp w15, #97
    blt 33f
    cmp w15, #122
    bgt 33f
    sub w15, w15, #32
33:
    strb w15, [x14], #1
    b 31b
32:
    mov w15, #0
    strb w15, [x14]
    mov x0, x13
    ldp x29, x30, [sp], #16
    ret
rt_lower:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x9, x0
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
34:
    ldrb w15, [x9], #1
    cbz w15, 35f
    cmp w15, #65
    blt 36f
    cmp w15, #90
    bgt 36f
    add w15, w15, #32
36:
    strb w15, [x14], #1
    b 34b
35:
    mov w15, #0
    strb w15, [x14]
    mov x0, x13
    ldp x29, x30, [sp], #16
    ret

// rt_chr: cadena de un solo caracter, codigo x0.
rt_chr:
    adrp x11, rt_str_pos
    add x11, x11, :lo12:rt_str_pos
    ldr x12, [x11]
    adrp x13, rt_str_pool
    add x13, x13, :lo12:rt_str_pool
    add x13, x13, x12
    add x12, x12, #128
    and x12, x12, #1023
    str x12, [x11]
    strb w0, [x13]
    mov w9, #0
    strb w9, [x13, #1]
    mov x0, x13
    ret

// rt_asc: codigo del primer caracter de x0 (0 si esta vacia).
rt_asc:
    ldrb w0, [x0]
    ret

// -- funciones numericas: Abs, Sgn, Min, Max, Rnd --

rt_abs:
    cmp x0, #0
    bge 37f
    neg x0, x0
37:
    ret
rt_sgn:
    cmp x0, #0
    beq 39f
    bgt 38f
    mov x0, #-1
    ret
38:
    mov x0, #1
    ret
39:
    mov x0, #0
    ret
rt_min:
    cmp x0, x1
    ble 40f
    mov x0, x1
40:
    ret
rt_max:
    cmp x0, x1
    bge 41f
    mov x0, x1
41:
    ret

// rt_rnd: entero pseudoaleatorio en [x0, x1). Generador
// congruencial lineal sencillo (mismos coeficientes que la
// libreria C clasica); la semilla se inicializa una vez en
// _start con SYS_GET_TICKS, asi que cada ejecucion arranca
// con una secuencia distinta.
rt_rnd:
    adrp x9, rt_rnd_seed
    add x9, x9, :lo12:rt_rnd_seed
    ldr x10, [x9]
    mov x11, #1103515245
    mul x10, x10, x11
    mov x11, #12345
    add x10, x10, x11
    str x10, [x9]
    lsr x12, x10, #16
    sub x13, x1, x0
    cmp x13, #0
    bgt 42f
    ret
42:
    udiv x14, x12, x13
    msub x15, x14, x13, x12
    add x0, x0, x15
    ret

// -- bucle de eventos: WaitEvent, PollEvent, Notify, ClientWidth/Height --

// rt_wait_event: x0=timeout en milisegundos, o -1 para esperar
// sin limite. Bombea el resto del sistema (SYS_PUMP) y consulta
// el evento pendiente (SYS_POLL_EVENT) en bucle hasta que llegue
// uno, o hasta que expire el timeout (en ese caso devuelve 0).
// El id devuelto se cachea en rt_last_event_id para EventID().
rt_wait_event:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    mov x19, x0
    cmp x19, #0
    blt 43f
    mov x9, #10
    sdiv x20, x19, x9
    mov x8, #2
    svc #0
    add x20, x20, x0
    b 44f
43:
    mov x20, #0
44:
45:
    mov x8, #14
    svc #0
    mov x8, #8
    svc #0
    cbnz x0, 47f
    cmp x19, #0
    blt 45b
    mov x8, #2
    svc #0
    cmp x0, x20
    blt 45b
    mov x0, #0
47:
    adrp x9, rt_last_event_id
    add x9, x9, :lo12:rt_last_event_id
    str x0, [x9]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

// rt_poll_event: igual que rt_wait_event, pero UNA sola consulta
// (sin esperar) -- version 'no bloqueante' para PollEvent().
rt_poll_event:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x8, #14
    svc #0
    mov x8, #8
    svc #0
    adrp x9, rt_last_event_id
    add x9, x9, :lo12:rt_last_event_id
    str x0, [x9]
    ldp x29, x30, [sp], #16
    ret

// rt_client_width / rt_client_height: x0=handle de ventana (el
// centinela 0x7FFFFFFF que devuelve Desktop() pide las medidas
// de TODA la pantalla en vez de las de la ventana propia).
rt_client_width:
    mov x9, #0x7FFFFFFF
    cmp x0, x9
    beq 48f
    mov x8, #33
    svc #0
    lsr x0, x0, #32
    ret
48:
    mov x8, #35
    svc #0
    lsr x0, x0, #32
    ret
rt_client_height:
    mov x9, #0x7FFFFFFF
    cmp x0, x9
    beq 49f
    mov x8, #33
    svc #0
    mov w0, w0
    ret
49:
    mov x8, #35
    svc #0
    mov w0, w0
    ret

// rt_notify: x0=puntero al mensaje. Aproximacion sencilla de
// Notify() -- dibuja una caja centrada con el texto y espera
// 1.5 segundos, sin necesitar ningun dialogo modal nuevo en el
// kernel (solo syscalls que ya existian).
rt_notify:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x9, x0
    mov x8, #33
    svc #0
    lsr x10, x0, #32
    mov w11, w0
    mov x12, #200
    mov x13, #60
    subs x14, x10, x12
    bge 50f
    mov x14, #0
50:
    lsr x14, x14, #1
    subs x15, x11, x13
    bge 51f
    mov x15, #0
51:
    lsr x15, x15, #1
    mov x0, x14
    mov x1, x15
    mov x2, x12
    mov x3, x13
    mov x4, #0xF0F0F0
    mov x8, #30
    svc #0
    add x0, x14, #10
    add x1, x15, #20
    mov x2, x9
    mov x3, #0
    mov x8, #31
    svc #0
    mov x0, #150
    mov x8, #1
    svc #0
    ldp x29, x30, [sp], #16
    ret

// rt_instr: posicion (base 1) de la primera aparicion de x1
// dentro de x0, o 0 si no aparece.
rt_instr:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x9, x0
    mov x10, #1
52:
    ldrb w11, [x9]
    cbz w11, 55f
    mov x12, x9
    mov x13, x1
53:
    ldrb w14, [x13]
    cbz w14, 54f
    ldrb w15, [x12]
    cbz w15, 55f
    cmp w14, w15
    bne 56f
    add x12, x12, #1
    add x13, x13, #1
    b 53b
54:
    mov x0, x10
    ldp x29, x30, [sp], #16
    ret
56:
    add x9, x9, #1
    add x10, x10, #1
    b 52b
55:
    mov x0, #0
    ldp x29, x30, [sp], #16
    ret

// rt_replace: x0=cadena, x1=buscada, x2=sustituta -- devuelve
// una cadena nueva con TODAS las apariciones sustituidas.
rt_replace:
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    mov x19, x0
    mov x20, x1
    mov x21, x2
    adrp x9, rt_str_pos
    add x9, x9, :lo12:rt_str_pos
    ldr x10, [x9]
    adrp x11, rt_str_pool
    add x11, x11, :lo12:rt_str_pool
    add x11, x11, x10
    add x10, x10, #128
    and x10, x10, #1023
    str x10, [x9]
    mov x22, x11
60:
    ldrb w12, [x19]
    cbz w12, 65f
    mov x13, x19
    mov x14, x20
61:
    ldrb w15, [x14]
    cbz w15, 63f
    ldrb w16, [x13]
    cbz w16, 62f
    cmp w15, w16
    bne 62f
    add x13, x13, #1
    add x14, x14, #1
    b 61b
63:
    mov x14, x21
64:
    ldrb w15, [x14], #1
    cbz w15, 66f
    strb w15, [x22], #1
    b 64b
66:
    mov x19, x13
    b 60b
62:
    strb w12, [x22], #1
    add x19, x19, #1
    b 60b
65:
    mov w12, #0
    strb w12, [x22]
    mov x0, x11
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x29, x30, [sp], #48
    ret

// rt_readline: x0=handle de archivo -- pide al kernel la
// siguiente linea directamente sobre un hueco del pool
// rotatorio de cadenas, y devuelve ese mismo puntero.
rt_readline:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x9, x0
    adrp x11, rt_str_pos
    add x11, x11, :lo12:rt_str_pos
    ldr x12, [x11]
    adrp x13, rt_str_pool
    add x13, x13, :lo12:rt_str_pool
    add x13, x13, x12
    add x12, x12, #128
    and x12, x12, #1023
    str x12, [x11]
    mov x0, x9
    mov x1, x13
    mov x2, #128
    mov x8, #42
    svc #0
    mov x0, x13
    ldp x29, x30, [sp], #16
    ret

.section .rodata
rt_half: .quad 0x3FE0000000000000
str_0: .asciz "x="
str_1: .asciz " y="
str_2: .asciz "ms="
str_3: .asciz "COLISION!"

rt_data_table:
rt_data_end:

.section .bss
.align 3
var_x: .space 8
var_y: .space 8
var_speed: .space 8
rt_str_pool: .space 1024
rt_str_pos: .space 8
rt_rnd_seed: .space 8
rt_last_event_id: .space 8
rt_data_ptr: .space 8
rt_current_color: .space 8
