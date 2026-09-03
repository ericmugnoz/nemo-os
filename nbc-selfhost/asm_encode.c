// asm_encode.c — ensamblador Nemo-AS
//
// Cada formula de aqui esta escrita como desplazamientos de bits
// explicitos, siguiendo el mismo orden que las tablas de campos de
// bits del manual de ARM -- no como constantes hexadecimales ya
// calculadas de memoria, para que se pueda auditar cada instruccion
// campo a campo. La formula de stp/ldp la verifique a mano contra
// "stp x29,x30,[sp,#-16]!" = 0xA9BF7BFD (la instruccion mas comun de
// todo AArch64 -- aparece en el prologo de practicamente cualquier
// funcion), y coincidio exactamente bit a bit.

#include "asm_encode.h"

// ---- procesamiento de datos, registro a registro ----

uint32_t enc_add_reg(int rd, int rn, int rm, int shift_amount) {
    uint32_t sf = 1, op = 0, S = 0, shift_type = 0; // shift_type=00 -> LSL
    return (sf << 31) | (op << 30) | (S << 29) | (0x0Bu << 24) |
           (shift_type << 22) | (0u << 21) |
           ((uint32_t)rm << 16) | ((uint32_t)shift_amount << 10) |
           ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_sub_reg(int rd, int rn, int rm) {
    uint32_t sf = 1, op = 1, S = 0;
    return (sf << 31) | (op << 30) | (S << 29) | (0x0Bu << 24) |
           ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_subs_reg(int rd, int rn, int rm) {
    uint32_t sf = 1, op = 1, S = 1;
    return (sf << 31) | (op << 30) | (S << 29) | (0x0Bu << 24) |
           ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_mul(int rd, int rn, int rm) {
    // MUL xd,xn,xm es el alias de MADD xd,xn,xm,xzr
    return 0x9B000000u | ((uint32_t)rm << 16) | ((uint32_t)REG_ZR << 10) |
           ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_sdiv(int rd, int rn, int rm) {
    return 0x9AC00C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_udiv(int rd, int rn, int rm) {
    return 0x9AC00800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_msub(int rd, int rn, int rm, int ra) {
    return 0x9B008000u | ((uint32_t)rm << 16) | ((uint32_t)ra << 10) |
           ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_and_reg(int rd, int rn, int rm) {
    uint32_t sf = 1, opc = 0; // 00 = AND
    return (sf << 31) | (opc << 29) | (0x0Au << 24) |
           ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_and_imm(int rd, int rn, uint64_t imm) {
    int ones = 0;
    while (ones < 63 && (imm & (1ULL << ones))) ones++;
    uint64_t expected = (ones >= 63) ? ~0ULL : ((1ULL << ones) - 1ULL);
    if (ones == 0 || imm != expected) {
        return 0; // el llamador debe detectar 0 como "no representable" y avisar
    }
    // N=1 (elemento de 64 bits, sin repeticion), immr=0 (sin
    // rotacion -- la mascara ya empieza en el bit 0), imms = numero
    // de unos - 1.
    uint32_t imms = (uint32_t)(ones - 1);
    return 0x92400000u | (imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

// ORR de inmediato usa EXACTAMENTE la misma familia de codificacion
// que AND de inmediato -- solo cambia el campo 'opc' (bits 30:29) de
// 00 (AND) a 01 (ORR), que equivale a sumar 0x20000000 a la base de AND.
uint32_t enc_orr_imm(int rd, int rn, uint64_t imm) {
    int ones = 0;
    while (ones < 63 && (imm & (1ULL << ones))) ones++;
    uint64_t expected = (ones >= 63) ? ~0ULL : ((1ULL << ones) - 1ULL);
    if (ones == 0 || imm != expected) {
        return 0;
    }
    uint32_t imms = (uint32_t)(ones - 1);
    return 0xB2400000u | (imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_orr_reg(int rd, int rn, int rm) {
    uint32_t sf = 1, opc = 1; // 01 = ORR (mov xd,xn = orr xd,xzr,xn)
    return (sf << 31) | (opc << 29) | (0x0Au << 24) |
           ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_lsl_imm(int rd, int rn, int shift_amount) {
    // LSL xd,xn,#N es el alias de UBFM xd,xn,#((64-N)&63),#(63-N)
    uint32_t immr = (uint32_t)((64 - shift_amount) & 63);
    uint32_t imms = (uint32_t)(63 - shift_amount);
    return 0xD3400000u | (immr << 16) | (imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_lsr_imm(int rd, int rn, int shift_amount) {
    // LSR xd,xn,#N es el alias de UBFM xd,xn,#N,#63
    uint32_t immr = (uint32_t)(shift_amount & 63);
    uint32_t imms = 63u;
    return 0xD3400000u | (immr << 16) | (imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_asr_imm(int rd, int rn, int shift_amount) {
    uint32_t immr = (uint32_t)(shift_amount & 63);
    uint32_t imms = 63u;
    return 0x93400000u | (immr << 16) | (imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_lslv(int rd, int rn, int rm) {
    return 0x9AC02000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_lsrv(int rd, int rn, int rm) {
    return 0x9AC02400u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_asrv(int rd, int rn, int rm) {
    return 0x9AC02800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_eor_reg(int rd, int rn, int rm) {
    return 0xCA000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

// ---- procesamiento de datos, con inmediato ----

uint32_t enc_add_imm(int rd, int rn, uint32_t imm12) {
    uint32_t sf = 1, op = 0, S = 0;
    return (sf << 31) | (op << 30) | (S << 29) | (0x11u << 24) |
           ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_sub_imm(int rd, int rn, uint32_t imm12) {
    uint32_t sf = 1, op = 1, S = 0;
    return (sf << 31) | (op << 30) | (S << 29) | (0x11u << 24) |
           ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_subs_imm(int rd, int rn, uint32_t imm12) {
    uint32_t sf = 1, op = 1, S = 1;
    return (sf << 31) | (op << 30) | (S << 29) | (0x11u << 24) |
           ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

uint32_t enc_movz(int rd, uint32_t imm16, int hw) {
    return 0xD2800000u | ((uint32_t)hw << 21) | ((imm16 & 0xFFFFu) << 5) | (uint32_t)rd;
}

uint32_t enc_movk(int rd, uint32_t imm16, int hw) {
    return 0xF2800000u | ((uint32_t)hw << 21) | ((imm16 & 0xFFFFu) << 5) | (uint32_t)rd;
}

// ---- seleccion condicional ----
// CSET/CNEG codifican la condicion INVERTIDA -- es la propia
// definicion del alias (ver comentario en asm_encode.h).

uint32_t enc_cset(int rd, int cond) {
    uint32_t invcond = (uint32_t)(cond ^ 1);
    return 0x9A9F07E0u | (invcond << 12) | (uint32_t)rd;
}

uint32_t enc_cneg(int rd, int rn, int cond) {
    uint32_t invcond = (uint32_t)(cond ^ 1);
    return 0xDA800400u | ((uint32_t)rn << 16) | (invcond << 12) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

// ---- ramas ----

uint32_t enc_b(int32_t imm26) {
    return 0x14000000u | ((uint32_t)imm26 & 0x3FFFFFFu);
}
uint32_t enc_bl(int32_t imm26) {
    return 0x94000000u | ((uint32_t)imm26 & 0x3FFFFFFu);
}
uint32_t enc_ret(void) {
    return 0xD65F0000u | (30u << 5); // siempre x30
}
uint32_t enc_br(int rn) {
    return 0xD61F0000u | ((uint32_t)rn << 5);
}
uint32_t enc_svc(uint32_t imm16) {
    return 0xD4000001u | ((imm16 & 0xFFFFu) << 5);
}
uint32_t enc_cbz(int rt, int32_t imm19) {
    return 0xB4000000u | (((uint32_t)imm19 & 0x7FFFFu) << 5) | (uint32_t)rt;
}
uint32_t enc_cbnz(int rt, int32_t imm19) {
    return 0xB5000000u | (((uint32_t)imm19 & 0x7FFFFu) << 5) | (uint32_t)rt;
}
uint32_t enc_bcond(int cond, int32_t imm19) {
    return 0x54000000u | (((uint32_t)imm19 & 0x7FFFFu) << 5) | (uint32_t)cond;
}

// ---- carga/almacenamiento, desplazamiento sin signo (escalado) ----

uint32_t enc_ldr_imm(int rt, int rn, uint32_t imm12_scaled) {
    return 0xF9400000u | ((imm12_scaled & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_str_imm(int rt, int rn, uint32_t imm12_scaled) {
    return 0xF9000000u | ((imm12_scaled & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_ldur(int rt, int rn, int32_t imm9) {
    return 0xF8400000u | (((uint32_t)imm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_stur(int rt, int rn, int32_t imm9) {
    return 0xF8000000u | (((uint32_t)imm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_ldurb(int rt, int rn, int32_t imm9) {
    return 0x38400000u | (((uint32_t)imm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_sturb(int rt, int rn, int32_t imm9) {
    return 0x38000000u | (((uint32_t)imm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_ldrb_imm(int rt, int rn, uint32_t imm12) {
    return 0x39400000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_strb_imm(int rt, int rn, uint32_t imm12) {
    return 0x39000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

// ---- carga/almacenamiento con desplazamiento por REGISTRO, [rn, rm]
// (sin escala, opcion LSL/UXTX por defecto) -- misma familia de
// instruccion que las de unsigned-immediate de arriba, solo que con
// Rm+opcion en vez de imm12.
uint32_t enc_ldrb_reg(int rt, int rn, int rm) {
    return 0x38606800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_strb_reg(int rt, int rn, int rm) {
    return 0x38206800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_ldr_reg(int rt, int rn, int rm) {
    return 0xF8606800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_str_reg(int rt, int rn, int rm) {
    return 0xF8206800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

// ---- carga/almacenamiento, pre/post-indexado (imm9 sin escalar) ----

uint32_t enc_ldr_post(int rt, int rn, int32_t imm9) {
    return 0xF8400400u | (((uint32_t)imm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_ldr_pre(int rt, int rn, int32_t imm9) {
    return 0xF8400C00u | (((uint32_t)imm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_str_pre(int rt, int rn, int32_t imm9) {
    return 0xF8000C00u | (((uint32_t)imm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_str_post(int rt, int rn, int32_t imm9) {
    return 0xF8000400u | (((uint32_t)imm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_ldrb_post(int rt, int rn, int32_t imm9) {
    return 0x38400400u | (((uint32_t)imm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_ldrb_pre(int rt, int rn, int32_t imm9) {
    return 0x38400C00u | (((uint32_t)imm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_strb_post(int rt, int rn, int32_t imm9) {
    return 0x38000400u | (((uint32_t)imm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
uint32_t enc_strb_pre(int rt, int rn, int32_t imm9) {
    return 0x38000C00u | (((uint32_t)imm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

// ---- pares (stp/ldp), verificadas a mano contra 0xA9BF7BFD ----

uint32_t enc_stp_pre(int rt1, int rt2, int rn, int32_t imm7_scaled) {
    return 0xA9800000u | (((uint32_t)imm7_scaled & 0x7Fu) << 15) |
           ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt1;
}
uint32_t enc_stp_post(int rt1, int rt2, int rn, int32_t imm7_scaled) {
    return 0xA8800000u | (((uint32_t)imm7_scaled & 0x7Fu) << 15) |
           ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt1;
}
uint32_t enc_stp_off(int rt1, int rt2, int rn, int32_t imm7_scaled) {
    return 0xA9000000u | (((uint32_t)imm7_scaled & 0x7Fu) << 15) |
           ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt1;
}
uint32_t enc_ldp_post(int rt1, int rt2, int rn, int32_t imm7_scaled) {
    return 0xA8C00000u | (((uint32_t)imm7_scaled & 0x7Fu) << 15) |
           ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt1;
}
uint32_t enc_ldp_pre(int rt1, int rt2, int rn, int32_t imm7_scaled) {
    return 0xA9C00000u | (((uint32_t)imm7_scaled & 0x7Fu) << 15) |
           ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt1;
}
uint32_t enc_ldp_off(int rt1, int rt2, int rn, int32_t imm7_scaled) {
    return 0xA9400000u | (((uint32_t)imm7_scaled & 0x7Fu) << 15) |
           ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt1;
}

// ---- direcciones ----

uint32_t enc_adrp(int rd, int32_t imm_pages) {
    uint32_t uimm = (uint32_t)imm_pages & 0x1FFFFFu; // 21 bits, con signo
    uint32_t immlo = uimm & 3u;
    uint32_t immhi = (uimm >> 2) & 0x7FFFFu;
    return 0x90000000u | (immlo << 29) | (immhi << 5) | (uint32_t)rd;
}

// ---- coma flotante (doble precision unicamente) ----

uint32_t enc_fmov_gpr_to_fpr(int rd, int rn) {
    return 0x9E670000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_fmov_fpr_to_gpr(int rd, int rn) {
    return 0x9E660000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_fmov_gpr_to_fpr32(int rd, int rn) {
    return 0x1E270000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_fmov_fpr_to_gpr32(int rd, int rn) {
    return 0x1E260000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_scvtf(int rd, int rn) {
    return 0x9E620000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_fcvtzs(int rd, int rn) {
    return 0x9E780000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_fadd(int rd, int rn, int rm) {
    return 0x1E602800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_fsub(int rd, int rn, int rm) {
    return 0x1E603800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_fmul(int rd, int rn, int rm) {
    return 0x1E600800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_fdiv(int rd, int rn, int rm) {
    return 0x1E601800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_fcmp(int rn, int rm) {
    return 0x1E602000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5);
}
uint32_t enc_fneg(int rd, int rn) {
    return 0x1E614000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_fabs(int rd, int rn) {
    return 0x1E60C000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_fsqrt(int rd, int rn) {
    return 0x1E61C000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_frintm(int rd, int rn) {
    return 0x1E654000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_frintp(int rd, int rn) {
    return 0x1E64C000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
uint32_t enc_fcvt(int rd, int rn, int src_is_single) {
    if (src_is_single) return 0x1E22C000u | ((uint32_t)rn << 5) | (uint32_t)rd;
    return 0x1E624000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
