// nb_encode.h — codificador de instrucciones ARM64 de Nemo-Blitz 2.0
//
// Adaptado de asm_encode.c del compilador viejo (Nemo-AS) -- esa parte
// nunca fue el problema (el ensamblador PROPIO que interpretaba texto
// alrededor de esto, si -- ver H_MOV_SAMEREG/H11/H12 de la auditoria).
// Aqui el generador de codigo llama a estas funciones DIRECTAMENTE, sin
// ningun texto intermedio que otro modulo tenga que volver a interpretar
// -- decision de diseño 1 del documento de rediseño.
//
// Cada formula verificada a mano contra el manual de ARM64 antes de
// adaptarla (ret=0xD65F03C0, svc#0=0xD4000001, movz x0,#5=0xD28000A0,
// stp x29,x30,[sp,#-16]!=0xA9BF7BFD -- las cuatro coinciden exactas).
//
// Codificadores de instrucciones AArch64: cada funcion produce los 4
// bytes exactos (como un uint32_t) de UNA instruccion. Solo cubre el
// subconjunto que nuestro propio generador de codigo usa de verdad --
// no es un ensamblador de proposito general.

#ifndef NB_ENCODE_H
#define NB_ENCODE_H

#include <stdint.h>

// Registros: 0-30 son x0-x30 (o w0-w30 en su forma de 32 bits),
// 31 significa XZR/WZR *o* SP segun la instruccion (igual que en el
// propio ARM -- ambiguedad historica, cada codificador la resuelve
// como corresponda).
#define NB_REG_SP  31
#define NB_REG_ZR  31

// Codigos de condicion (los mismos 4 bits que usa el propio hardware)
#define NB_COND_EQ 0
#define NB_COND_NE 1
#define NB_COND_CS 2
#define NB_COND_CC 3
#define NB_COND_MI 4
#define NB_COND_PL 5
#define NB_COND_VS 6
#define NB_COND_VC 7
#define NB_COND_HI 8
#define NB_COND_LS 9
#define NB_COND_GE 10
#define NB_COND_LT 11
#define NB_COND_GT 12
#define NB_COND_LE 13
#define NB_COND_AL 14

// -- Procesamiento de datos, registro a registro --
uint32_t nb_enc_add_reg(int rd, int rn, int rm, int shift_amount);   // add xd, xn, xm [, lsl #N]
uint32_t nb_enc_sub_reg(int rd, int rn, int rm);                     // sub xd, xn, xm
uint32_t nb_enc_subs_reg(int rd, int rn, int rm);                    // subs xd, xn, xm (cmp = subs xzr,...)
uint32_t nb_enc_mul(int rd, int rn, int rm);                         // mul xd, xn, xm
uint32_t nb_enc_sdiv(int rd, int rn, int rm);                        // sdiv xd, xn, xm
uint32_t nb_enc_udiv(int rd, int rn, int rm);                        // udiv xd, xn, xm
uint32_t nb_enc_msub(int rd, int rn, int rm, int ra);                // msub xd, xn, xm, xa
uint32_t nb_enc_and_reg(int rd, int rn, int rm);                     // and xd, xn, xm
// and xd, xn, #imm -- SOLO soporta mascaras de bits bajos contiguos
// (imm = 2^K - 1). Es la parte de todo el ensamblador que MENOS se
// puede verificar sin el toolchain real (la codificacion de
// "inmediato de patron de bits" de ARM64 es notoriamente compleja) --
// falla alto y claro si el valor no encaja en ese patron, en vez de
// arriesgarse a generar algo silenciosamente incorrecto.
uint32_t nb_enc_and_imm(int rd, int rn, uint64_t imm);
uint32_t nb_enc_orr_imm(int rd, int rn, uint64_t imm);
uint32_t nb_enc_orr_reg(int rd, int rn, int rm);                     // orr xd, xn, xm (mov xd,xn = orr xd,xzr,xn)
uint32_t nb_enc_lsl_imm(int rd, int rn, int shift_amount);           // lsl xd, xn, #N
uint32_t nb_enc_lsr_imm(int rd, int rn, int shift_amount);           // lsr xd, xn, #N
uint32_t nb_enc_asr_imm(int rd, int rn, int shift_amount);
uint32_t nb_enc_lslv(int rd, int rn, int rm);
uint32_t nb_enc_lsrv(int rd, int rn, int rm);
uint32_t nb_enc_asrv(int rd, int rn, int rm);
uint32_t nb_enc_eor_reg(int rd, int rn, int rm);

// -- Procesamiento de datos, con inmediato --
uint32_t nb_enc_add_imm(int rd, int rn, uint32_t imm12);             // add xd, xn, #imm (0-4095)
uint32_t nb_enc_sub_imm(int rd, int rn, uint32_t imm12);             // sub xd, xn, #imm
uint32_t nb_enc_subs_imm(int rd, int rn, uint32_t imm12);            // subs (cmp xn,#imm = subs xzr,xn,#imm)
uint32_t nb_enc_movz(int rd, uint32_t imm16, int hw);                // movz xd, #imm16, lsl #(hw*16)
uint32_t nb_enc_movk(int rd, uint32_t imm16, int hw);                // movk xd, #imm16, lsl #(hw*16)

// -- Seleccion condicional --
uint32_t nb_enc_cset(int rd, int cond);
uint32_t nb_enc_csetm(int rd, int cond);                             // csetm xd, cond  (cond ? -1 : 0)                              // cset xd, cond
uint32_t nb_enc_cneg(int rd, int rn, int cond);                      // cneg xd, xn, cond
uint32_t nb_enc_csel(int rd, int rn, int rm, int cond);              // csel xd, xn, xm, cond (xd = cond ? xn : xm)

// -- Ramas --
uint32_t nb_enc_b(int32_t imm26);        // b <offset en INSTRUCCIONES, no bytes>
uint32_t nb_enc_bl(int32_t imm26);       // bl <offset en instrucciones>
uint32_t nb_enc_ret(void);               // ret (siempre usa x30)
uint32_t nb_enc_nop(void);               // nop -- no hace nada, para rellenar hasta una alineacion
uint32_t nb_enc_br(int rn);
uint32_t nb_enc_svc(uint32_t imm16);     // svc #imm16 (llamada al sistema)
uint32_t nb_enc_cbz(int rt, int32_t imm19);   // cbz xt, <offset en instrucciones>
uint32_t nb_enc_cbnz(int rt, int32_t imm19);  // cbnz xt, <offset en instrucciones>
uint32_t nb_enc_bcond(int cond, int32_t imm19); // b.cond <offset en instrucciones>

// -- Carga/almacenamiento --
uint32_t nb_enc_ldr_imm(int rt, int rn, uint32_t imm12_scaled);   // ldr xt, [xn, #imm] (imm multiplo de 8, NO negativo)
uint32_t nb_enc_str_imm(int rt, int rn, uint32_t imm12_scaled);   // str xt, [xn, #imm]
// ldur/stur: la misma idea que ldr/str pero con un desplazamiento de
// 9 bits CON SIGNO, sin escalar (-256..255) -- la variante que usa
// un ensamblador de verdad automaticamente en cuanto el desplazamiento
// no cabe en el formato "sin signo" de ldr/str (por ejemplo, para
// variables locales por debajo del puntero de marco, [x29, #-8]).
uint32_t nb_enc_ldur(int rt, int rn, int32_t imm9);
uint32_t nb_enc_stur(int rt, int rn, int32_t imm9);
uint32_t nb_enc_ldurb(int rt, int rn, int32_t imm9);
uint32_t nb_enc_sturb(int rt, int rn, int32_t imm9);
uint32_t nb_enc_ldr_post(int rt, int rn, int32_t imm9);            // ldr xt, [xn], #imm
uint32_t nb_enc_ldr_pre(int rt, int rn, int32_t imm9);              // ldr xt, [xn, #imm]!
uint32_t nb_enc_str_pre(int rt, int rn, int32_t imm9);              // str xt, [xn, #imm]!
uint32_t nb_enc_str_post(int rt, int rn, int32_t imm9);              // str xt, [xn], #imm
uint32_t nb_enc_ldrb_post(int rt, int rn, int32_t imm9);            // ldrb wt, [xn], #imm
uint32_t nb_enc_ldrb_pre(int rt, int rn, int32_t imm9);              // ldrb wt, [xn, #imm]!
uint32_t nb_enc_strb_post(int rt, int rn, int32_t imm9);            // strb wt, [xn], #imm
uint32_t nb_enc_strb_pre(int rt, int rn, int32_t imm9);              // strb wt, [xn, #imm]!
uint32_t nb_enc_ldrb_imm(int rt, int rn, uint32_t imm12);           // ldrb wt, [xn, #imm]
uint32_t nb_enc_strb_imm(int rt, int rn, uint32_t imm12);           // strb wt, [xn, #imm]
uint32_t nb_enc_ldrb_reg(int rt, int rn, int rm);                    // ldrb wt, [xn, xm]
uint32_t nb_enc_strb_reg(int rt, int rn, int rm);                    // strb wt, [xn, xm]
uint32_t nb_enc_ldr_reg(int rt, int rn, int rm);                     // ldr xt, [xn, xm]
uint32_t nb_enc_str_reg(int rt, int rn, int rm);                     // str xt, [xn, xm]

uint32_t nb_enc_stp_pre(int rt1, int rt2, int rn, int32_t imm7_scaled);  // stp xt1, xt2, [xn, #imm]!
uint32_t nb_enc_stp_post(int rt1, int rt2, int rn, int32_t imm7_scaled); // stp xt1, xt2, [xn], #imm
uint32_t nb_enc_stp_off(int rt1, int rt2, int rn, int32_t imm7_scaled);  // stp xt1, xt2, [xn, #imm]
uint32_t nb_enc_ldp_post(int rt1, int rt2, int rn, int32_t imm7_scaled); // ldp xt1, xt2, [xn], #imm
uint32_t nb_enc_ldp_pre(int rt1, int rt2, int rn, int32_t imm7_scaled);  // ldp xt1, xt2, [xn, #imm]!
uint32_t nb_enc_ldp_off(int rt1, int rt2, int rn, int32_t imm7_scaled);  // ldp xt1, xt2, [xn, #imm]

// -- Direcciones --
uint32_t nb_enc_adrp(int rd, int32_t imm_pages); // adrp xd, <paginas de diferencia, con signo>

// -- Coma flotante (solo double, 64 bits) --
uint32_t nb_enc_fmov_gpr_to_fpr(int rd, int rn);
uint32_t nb_enc_fmov_fpr_to_gpr(int rd, int rn);
uint32_t nb_enc_fmov_gpr_to_fpr32(int rd, int rn);
uint32_t nb_enc_fmov_fpr_to_gpr32(int rd, int rn);
uint32_t nb_enc_scvtf(int rd, int rn);
uint32_t nb_enc_fcvtzs(int rd, int rn);
uint32_t nb_enc_fadd(int rd, int rn, int rm);
uint32_t nb_enc_fsub(int rd, int rn, int rm);
uint32_t nb_enc_fmul(int rd, int rn, int rm);
uint32_t nb_enc_fdiv(int rd, int rn, int rm);
uint32_t nb_enc_fcmp(int rn, int rm);
uint32_t nb_enc_fneg(int rd, int rn);
uint32_t nb_enc_fabs(int rd, int rn);
uint32_t nb_enc_fsqrt(int rd, int rn);
uint32_t nb_enc_frintm(int rd, int rn);                              // frintm dd, dn  (redondea hacia -infinito = floor)
uint32_t nb_enc_frintp(int rd, int rn);                              // frintp dd, dn  (redondea hacia +infinito = ceil)
uint32_t nb_enc_fcvt(int rd, int rn, int src_is_single);

#endif
