// Mini-desensamblador SOLO para depurar -- decodifica lo justo para
// rastrear el bucle For a mano (branches, cmp, ldr/str con
// pre/post-indice, adrp/add).
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static const char *cond_name(int c) {
    static const char *n[] = {"eq","ne","cs","cc","mi","pl","vs","vc","hi","ls","ge","lt","gt","le","al","nv"};
    return n[c & 0xF];
}

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    uint8_t buf[65536];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t w = buf[i] | (buf[i+1]<<8) | (buf[i+2]<<16) | ((uint32_t)buf[i+3]<<24);
        printf("%04zx: %08x  ", i, w);

        if ((w & 0xFF000010) == 0x54000000) {
            int32_t imm19 = (int32_t)((w >> 5) & 0x7FFFF);
            if (imm19 & 0x40000) imm19 |= 0xFFF80000; // signo
            int cond = w & 0xF;
            printf("b.%s #%d (destino=%04zx)\n", cond_name(cond), imm19, i + (size_t)(imm19*4));
        } else if ((w & 0xFC000000) == 0x14000000) {
            int32_t imm26 = (int32_t)(w & 0x3FFFFFF);
            if (imm26 & 0x2000000) imm26 |= 0xFC000000;
            printf("b #%d (destino=%04zx)\n", imm26, i + (size_t)(imm26*4));
        } else if ((w & 0xFC000000) == 0x94000000) {
            int32_t imm26 = (int32_t)(w & 0x3FFFFFF);
            if (imm26 & 0x2000000) imm26 |= 0xFC000000;
            printf("bl #%d (destino=%04zx)\n", imm26, i + (size_t)(imm26*4));
        } else if (w == 0xD65F03C0) {
            printf("ret\n");
        } else if ((w & 0xFF800000) == 0xEB000000) {
            printf("subs/cmp xzr, x%d, x%d\n", (w>>5)&0x1F, (w>>16)&0x1F);
        } else if ((w & 0x7F800000) == 0x11000000) {
            printf("add x%d, x%d, #%d\n", w&0x1F, (w>>5)&0x1F, (w>>10)&0xFFF);
        } else if ((w & 0xFFC00000) == 0xF9400000) {
            printf("ldr x%d, [x%d, #%d]\n", w&0x1F, (w>>5)&0x1F, ((w>>10)&0xFFF)*8);
        } else if ((w & 0xFFC00000) == 0xF9000000) {
            printf("str x%d, [x%d, #%d]\n", w&0x1F, (w>>5)&0x1F, ((w>>10)&0xFFF)*8);
        } else if ((w & 0xFFC00000) == 0xF8400000) {
            int idx = (w>>10)&3;
            int32_t imm9 = (int32_t)((w>>12)&0x1FF);
            if (imm9 & 0x100) imm9 |= 0xFFFFFE00;
            printf("ldr x%d, [x%d] %s#%d\n", w&0x1F, (w>>5)&0x1F, idx==1?"POST,":idx==3?"PRE,":"?,", imm9);
        } else if ((w & 0xFFC00000) == 0xF8000000) {
            int idx = (w>>10)&3;
            int32_t imm9 = (int32_t)((w>>12)&0x1FF);
            if (imm9 & 0x100) imm9 |= 0xFFFFFE00;
            printf("str x%d, [x%d] %s#%d\n", w&0x1F, (w>>5)&0x1F, idx==1?"POST,":idx==3?"PRE,":"?,", imm9);
        } else if ((w & 0x9F000000) == 0x90000000) {
            printf("adrp x%d, #pagina\n", w&0x1F);
        } else if ((w & 0xFFE0FC00) == 0x91000000) {
            printf("mov x%d, x%d\n", w&0x1F, (w>>5)&0x1F);
        } else if ((w & 0x7F800000) == 0x12800000 || (w&0xFF800000)==0xD2800000) {
            printf("movz x%d, #%d, lsl #%d\n", w&0x1F, (w>>5)&0xFFFF, ((w>>21)&3)*16);
        } else if ((w & 0xFF800000) == 0xF2800000) {
            printf("movk x%d, #%d, lsl #%d\n", w&0x1F, (w>>5)&0xFFFF, ((w>>21)&3)*16);
        } else if (w == 0xD4000001) {
            printf("svc #0\n");
        } else {
            printf(".word 0x%08x\n", w);
        }
    }
    return 0;
}
