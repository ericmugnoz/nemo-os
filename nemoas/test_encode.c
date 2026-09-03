#include <stdio.h>
#include "asm_encode.h"

static int failures = 0;

static void check(const char *desc, uint32_t got, uint32_t expected) {
    if (got == expected) {
        printf("OK   %-40s 0x%08X\n", desc, got);
    } else {
        printf("MAL  %-40s obtenido=0x%08X esperado=0x%08X\n", desc, got, expected);
        failures++;
    }
}

int main(void) {
    // Instrucciones cuyo valor exacto conozco de memoria con total
    // seguridad -- son de las mas comunes en cualquier binario ARM64
    // real, asi que sirven como ancla fiable.
    check("stp x29,x30,[sp,#-16]!", enc_stp_pre(29,30,31,-2), 0xA9BF7BFDu);
    check("ret", enc_ret(), 0xD65F03C0u);
    check("mov x0,x1 (orr x0,xzr,x1)", enc_orr_reg(0,31,1), 0xAA0103E0u);
    check("add x0,x0,#1", enc_add_imm(0,0,1), 0x91000400u);
    check("ldr x0,[x0]", enc_ldr_imm(0,0,0), 0xF9400000u);
    check("cmp x1,x0 (subs xzr,x1,x0)", enc_subs_reg(31,1,0), 0xEB00003Fu);
    check("b.eq #0", enc_bcond(COND_EQ, 0), 0x54000000u);
    check("mul x0,x1,x2", enc_mul(0,1,2), 0x9B027C20u);

    printf("\n%s\n", failures == 0 ? "TODAS LAS COMPROBACIONES PASARON" : "HAY FALLOS -- revisar arriba");
    return failures;
}
