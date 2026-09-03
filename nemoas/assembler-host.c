// assembler.c — ensamblador Nemo-AS
//
// Dos pasadas:
//   1. Recorre todo el archivo calculando el tamaño de cada seccion
//      (.text, .rodata, .bss) y la posicion de cada etiqueta DENTRO
//      de su seccion. Al terminar, sabemos el tamaño total de .text y
//      .rodata, asi que podemos calcular la direccion FINAL de cada
//      etiqueta (las de .text empiezan en 0; las de .rodata justo
//      despues; las de .bss despues de esas -- aunque .bss no ocupa
//      espacio real en el archivo de salida).
//   2. Recorre todo el archivo otra vez, esta vez codificando cada
//      instruccion de verdad -- ahora que conocemos TODAS las
//      direcciones, podemos resolver saltos, adrp y :lo12: sin
//      importar si la etiqueta aparece antes o despues en el archivo.

#include "assembler.h"
#include "asm_lexer.h"
#include "asm_encode.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SEC_TEXT 0
#define SEC_RODATA 1
#define SEC_BSS 2

// ---- tabla de simbolos ----

#define MAX_SYMBOLS 512
typedef struct {
    char name[64];
    int section;
    uint32_t offset;  // dentro de su seccion (pasada 1)
    uint32_t address; // final, absoluta (calculada tras la pasada 1)
} Symbol;

static Symbol symbols[MAX_SYMBOLS];
static int symbol_count = 0;

static int find_symbol(const char *name) {
    for (int i = 0; i < symbol_count; i++) {
        if (strcmp(symbols[i].name, name) == 0) return i;
    }
    return -1;
}

static void define_symbol(const char *name, int section, uint32_t offset) {
    if (find_symbol(name) >= 0) {
        fprintf(stderr, "error: etiqueta '%s' definida mas de una vez\n", name);
        exit(1);
    }
    if (symbol_count >= MAX_SYMBOLS) { fprintf(stderr, "error: demasiadas etiquetas\n"); exit(1); }
    strncpy(symbols[symbol_count].name, name, 63);
    symbols[symbol_count].section = section;
    symbols[symbol_count].offset = offset;
    symbol_count++;
}

static uint32_t symbol_address(const char *name) {
    int idx = find_symbol(name);
    if (idx < 0) { fprintf(stderr, "error: simbolo no definido: %s\n", name); exit(1); }
    return symbols[idx].address;
}

// ---- registros ----

static int reg_number(const char *name) {
    if (strcmp(name, "sp") == 0) return REG_SP;
    if (strcmp(name, "xzr") == 0 || strcmp(name, "wzr") == 0) return REG_ZR;
    if ((name[0] == 'x' || name[0] == 'w' || name[0] == 'd' || name[0] == 's') && name[1] != '\0') {
        return atoi(name + 1);
    }
    fprintf(stderr, "error: registro no reconocido: %s\n", name);
    exit(1);
}

// ---- condiciones ----

static int cond_from_name(const char *name) {
    if (strcmp(name, "eq") == 0) return COND_EQ;
    if (strcmp(name, "ne") == 0) return COND_NE;
    if (strcmp(name, "cs") == 0 || strcmp(name, "hs") == 0) return COND_CS;
    if (strcmp(name, "cc") == 0 || strcmp(name, "lo") == 0) return COND_CC;
    if (strcmp(name, "mi") == 0) return COND_MI;
    if (strcmp(name, "pl") == 0) return COND_PL;
    if (strcmp(name, "vs") == 0) return COND_VS;
    if (strcmp(name, "vc") == 0) return COND_VC;
    if (strcmp(name, "hi") == 0) return COND_HI;
    if (strcmp(name, "ls") == 0) return COND_LS;
    if (strcmp(name, "ge") == 0) return COND_GE;
    if (strcmp(name, "lt") == 0) return COND_LT;
    if (strcmp(name, "gt") == 0) return COND_GT;
    if (strcmp(name, "le") == 0) return COND_LE;
    fprintf(stderr, "error: condicion no reconocida: %s\n", name);
    exit(1);
}

// ---- descomposicion de "mov xd, #inmediato" en movz + hasta 3 movk ----
// Compartida entre las dos pasadas: la primera solo necesita saber
// CUANTAS instrucciones hacen falta; la segunda tambien necesita los
// propios trozos de 16 bits.
static int mov_imm_chunks(uint64_t value, uint16_t chunks[4], bool used[4]) {
    bool any_nonzero = false;
    for (int i = 0; i < 4; i++) {
        chunks[i] = (uint16_t)(value >> (i * 16));
        used[i] = false;
        if (chunks[i] != 0) any_nonzero = true;
    }
    if (!any_nonzero) { used[0] = true; return 1; } // mov xd, #0
    int count = 0;
    for (int i = 0; i < 4; i++) {
        if (chunks[i] != 0) { used[i] = true; count++; }
    }
    return count;
}

// ---- parser de una linea (compartido, con o sin generar codigo) ----

typedef struct {
    AsmLexer lx;
    AsmToken cur;
} AParser;

static void aadvance(AParser *p) { p->cur = asm_lexer_next(&p->lx); }

static void aexpect(AParser *p, AsmTokenType t, const char *what) {
    if (p->cur.type != t) {
        fprintf(stderr, "error (linea %d): se esperaba %s\n", p->cur.line, what);
        exit(1);
    }
    aadvance(p);
}

static int parse_reg(AParser *p) {
    if (p->cur.type != AT_IDENT) {
        fprintf(stderr, "error (linea %d): se esperaba un registro\n", p->cur.line);
        exit(1);
    }
    int r = reg_number(p->cur.text);
    aadvance(p);
    return r;
}

typedef struct {
    int rn;
    int32_t imm;
    bool lo12;         // true si el desplazamiento era ":lo12:simbolo" en vez de un numero
    char lo12_sym[64];
    bool writeback_pre;  // [rn, #imm]!
    bool writeback_post; // [rn], #imm
} MemOperand;

static MemOperand parse_mem_operand(AParser *p) {
    MemOperand m;
    memset(&m, 0, sizeof(m));
    aexpect(p, AT_LBRACKET, "'['");
    m.rn = parse_reg(p);
    if (p->cur.type == AT_COMMA) {
        aadvance(p);
        if (p->cur.type != AT_NUMBER) {
            fprintf(stderr, "error (linea %d): se esperaba un numero en el desplazamiento\n", p->cur.line);
            exit(1);
        }
        m.imm = (int32_t)p->cur.num_value;
        aadvance(p);
    }
    aexpect(p, AT_RBRACKET, "']'");
    if (p->cur.type == AT_BANG) {
        aadvance(p);
        m.writeback_pre = true;
    } else if (p->cur.type == AT_COMMA) {
        aadvance(p);
        if (p->cur.type != AT_NUMBER) {
            fprintf(stderr, "error (linea %d): se esperaba un numero tras ','\n", p->cur.line);
            exit(1);
        }
        m.imm = (int32_t)p->cur.num_value;
        aadvance(p);
        m.writeback_post = true;
    }
    return m;
}

// Direccion de la instruccion que se esta codificando ahora mismo --
// la necesitan adrp y los saltos para calcular su desplazamiento
// relativo al PC. Se actualiza en cada palabra que se escribe.
static uint32_t g_current_addr;

// Salida de la pasada 2
static uint8_t *g_text_out;
static uint32_t g_text_pos;
static uint8_t *g_rodata_out;
static uint32_t g_rodata_pos;
static uint32_t g_text_base;   // siempre 0
static uint32_t g_rodata_base; // = tamaño final de .text
static uint32_t g_bss_base;    // = rodata_base + tamaño final de .rodata

static void emit_word(uint32_t word) {
    g_text_out[g_text_pos++] = (uint8_t)(word);
    g_text_out[g_text_pos++] = (uint8_t)(word >> 8);
    g_text_out[g_text_pos++] = (uint8_t)(word >> 16);
    g_text_out[g_text_pos++] = (uint8_t)(word >> 24);
    g_current_addr += 4;
}

// ---- codificacion de una instruccion (solo se llama en la pasada 2) ----

static void encode_mnemonic(AParser *p, const char *mn) {
    if (strcmp(mn, "mov") == 0) {
        bool dst_is_sp = (p->cur.type == AT_IDENT && strcmp(p->cur.text, "sp") == 0);
        int rd = parse_reg(p);
        aexpect(p, AT_COMMA, "','");
        if (p->cur.type == AT_NUMBER) {
            uint64_t value = (uint64_t)p->cur.num_value;
            aadvance(p);
            uint16_t chunks[4]; bool used[4];
            mov_imm_chunks(value, chunks, used);
            bool first = true;
            for (int hw = 0; hw < 4; hw++) {
                if (!used[hw]) continue;
                if (first) { emit_word(enc_movz(rd, chunks[hw], hw)); first = false; }
                else { emit_word(enc_movk(rd, chunks[hw], hw)); }
            }
        } else {
            // Caso especial: "sp" y "xzr" comparten el mismo numero
            // de registro (31) en la codificacion, pero NO son
            // intercambiables -- orr/mov-registro normal SIEMPRE
            // interpreta el 31 como xzr, nunca como sp. Si el origen
            // o el destino es de verdad "sp", hay que usar
            // "add rd, rn, #0" en su lugar, que si sabe distinguirlos.
            bool src_is_sp = (p->cur.type == AT_IDENT && strcmp(p->cur.text, "sp") == 0);
            int rn = parse_reg(p);
            if (src_is_sp || dst_is_sp) emit_word(enc_add_imm(rd, rn, 0));
            else emit_word(enc_orr_reg(rd, REG_ZR, rn));
        }
        return;
    }
    if (strcmp(mn, "add") == 0 || strcmp(mn, "sub") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p); aexpect(p, AT_COMMA, "','");
        bool is_add = (mn[0] == 'a');
        if (p->cur.type == AT_LO12) {
            aadvance(p);
            if (p->cur.type != AT_IDENT) { fprintf(stderr, "error (linea %d): se esperaba un simbolo tras :lo12:\n", p->cur.line); exit(1); }
            uint32_t addr = symbol_address(p->cur.text);
            aadvance(p);
            emit_word(is_add ? enc_add_imm(rd, rn, addr & 0xFFF) : enc_sub_imm(rd, rn, addr & 0xFFF));
        } else if (p->cur.type == AT_NUMBER) {
            int64_t raw = p->cur.num_value;
            bool negative = raw < 0;
            uint32_t imm = negative ? (uint32_t)(-raw) : (uint32_t)raw;
            aadvance(p);
            // "add" con un inmediato negativo es, en realidad, un
            // "sub" con el valor positivo -- y viceversa. La propia
            // instruccion ADD/SUB-inmediato de ARM64 solo admite
            // valores SIN signo (0-4095); no hay forma de meter un
            // negativo ahi directamente.
            bool effective_is_add = negative ? !is_add : is_add;
            emit_word(effective_is_add ? enc_add_imm(rd, rn, imm) : enc_sub_imm(rd, rn, imm));
        } else {
            int rm = parse_reg(p);
            int shift_amt = 0;
            if (p->cur.type == AT_COMMA) {
                aadvance(p);
                if (p->cur.type != AT_IDENT || strcmp(p->cur.text, "lsl") != 0) {
                    fprintf(stderr, "error (linea %d): solo se soporta 'lsl' como desplazamiento\n", p->cur.line);
                    exit(1);
                }
                aadvance(p);
                if (p->cur.type != AT_NUMBER) { fprintf(stderr, "error (linea %d): se esperaba #cantidad\n", p->cur.line); exit(1); }
                shift_amt = (int)p->cur.num_value;
                aadvance(p);
            }
            emit_word(is_add ? enc_add_reg(rd, rn, rm, shift_amt) : enc_sub_reg(rd, rn, rm));
        }
        return;
    }
    if (strcmp(mn, "subs") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rm = parse_reg(p);
        emit_word(enc_subs_reg(rd, rn, rm));
        return;
    }
    if (strcmp(mn, "cmp") == 0) {
        int rn = parse_reg(p); aexpect(p, AT_COMMA, "','");
        if (p->cur.type == AT_NUMBER) {
            if (p->cur.num_value < 0) {
                fprintf(stderr, "error (linea %d): 'cmp' con inmediato negativo no soportado\n", p->cur.line);
                exit(1);
            }
            uint32_t imm = (uint32_t)p->cur.num_value;
            aadvance(p);
            emit_word(enc_subs_imm(REG_ZR, rn, imm));
        } else {
            int rm = parse_reg(p);
            emit_word(enc_subs_reg(REG_ZR, rn, rm));
        }
        return;
    }
    if (strcmp(mn, "mul") == 0 || strcmp(mn, "sdiv") == 0 || strcmp(mn, "udiv") == 0 ||
        strcmp(mn, "and") == 0 || strcmp(mn, "orr") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p); aexpect(p, AT_COMMA, "','");
        if (strcmp(mn, "and") == 0 && p->cur.type == AT_NUMBER) {
            uint64_t imm = (uint64_t)p->cur.num_value;
            aadvance(p);
            uint32_t word = enc_and_imm(rd, rn, imm);
            if (word == 0) {
                fprintf(stderr, "error (linea %d): 'and' con #%llu no soportado "
                                "(solo mascaras de bits bajos contiguos)\n", p->cur.line, (unsigned long long)imm);
                exit(1);
            }
            emit_word(word);
            return;
        }
        int rm = parse_reg(p);
        if (strcmp(mn, "mul") == 0) emit_word(enc_mul(rd, rn, rm));
        else if (strcmp(mn, "sdiv") == 0) emit_word(enc_sdiv(rd, rn, rm));
        else if (strcmp(mn, "udiv") == 0) emit_word(enc_udiv(rd, rn, rm));
        else if (strcmp(mn, "and") == 0) emit_word(enc_and_reg(rd, rn, rm));
        else emit_word(enc_orr_reg(rd, rn, rm));
        return;
    }
    if (strcmp(mn, "msub") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rm = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int ra = parse_reg(p);
        emit_word(enc_msub(rd, rn, rm, ra));
        return;
    }
    if (strcmp(mn, "neg") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p);
        emit_word(enc_sub_reg(rd, REG_ZR, rn));
        return;
    }
    if (strcmp(mn, "lsl") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p); aexpect(p, AT_COMMA, "','");
        if (p->cur.type == AT_NUMBER) {
            int amt = (int)p->cur.num_value;
            aadvance(p);
            emit_word(enc_lsl_imm(rd, rn, amt));
        } else {
            int rm = parse_reg(p); // cantidad en un registro, no inmediato
            emit_word(enc_lslv(rd, rn, rm));
        }
        return;
    }
    if (strcmp(mn, "lsr") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p); aexpect(p, AT_COMMA, "','");
        if (p->cur.type == AT_NUMBER) {
            int amt = (int)p->cur.num_value;
            aadvance(p);
            emit_word(enc_lsr_imm(rd, rn, amt));
        } else {
            int rm = parse_reg(p);
            emit_word(enc_lsrv(rd, rn, rm));
        }
        return;
    }
    if (strcmp(mn, "asr") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p); aexpect(p, AT_COMMA, "','");
        if (p->cur.type == AT_NUMBER) {
            int amt = (int)p->cur.num_value;
            aadvance(p);
            emit_word(enc_asr_imm(rd, rn, amt));
        } else {
            int rm = parse_reg(p);
            emit_word(enc_asrv(rd, rn, rm));
        }
        return;
    }
    if (strcmp(mn, "eor") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rm = parse_reg(p);
        emit_word(enc_eor_reg(rd, rn, rm));
        return;
    }
    if (strcmp(mn, "fmov") == 0) {
        // fmov dd,xn / fmov sd,wn (bits, sin convertir) o al reves --
        // se distingue mirando si el registro DESTINO empieza por 'd'
        // o 's' (FP), y si es 's' (o el GPR asociado es 'w'), usamos
        // la variante de 32 bits en vez de la de 64.
        if (p->cur.type != AT_IDENT) { fprintf(stderr, "error (linea %d): se esperaba un registro\n", p->cur.line); exit(1); }
        char dst_prefix = p->cur.text[0];
        bool dst_is_fp = (dst_prefix == 'd' || dst_prefix == 's');
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        if (p->cur.type != AT_IDENT) { fprintf(stderr, "error (linea %d): se esperaba un registro\n", p->cur.line); exit(1); }
        char src_prefix = p->cur.text[0];
        bool src_is_fp = (src_prefix == 'd' || src_prefix == 's');
        int rn = parse_reg(p);
        if (dst_is_fp && !src_is_fp) {
            bool is32 = (dst_prefix == 's');
            emit_word(is32 ? enc_fmov_gpr_to_fpr32(rd, rn) : enc_fmov_gpr_to_fpr(rd, rn));
        } else if (!dst_is_fp && src_is_fp) {
            bool is32 = (src_prefix == 's');
            emit_word(is32 ? enc_fmov_fpr_to_gpr32(rd, rn) : enc_fmov_fpr_to_gpr(rd, rn));
        } else {
            fprintf(stderr, "error (linea %d): fmov necesita mezclar un registro FP (d/s) con uno general (x/w)\n", p->cur.line);
            exit(1);
        }
        return;
    }
    if (strcmp(mn, "scvtf") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p);
        emit_word(enc_scvtf(rd, rn));
        return;
    }
    if (strcmp(mn, "fcvtzs") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p);
        emit_word(enc_fcvtzs(rd, rn));
        return;
    }
    if (strcmp(mn, "fadd") == 0 || strcmp(mn, "fsub") == 0 || strcmp(mn, "fmul") == 0 || strcmp(mn, "fdiv") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rm = parse_reg(p);
        if (strcmp(mn, "fadd") == 0) emit_word(enc_fadd(rd, rn, rm));
        else if (strcmp(mn, "fsub") == 0) emit_word(enc_fsub(rd, rn, rm));
        else if (strcmp(mn, "fmul") == 0) emit_word(enc_fmul(rd, rn, rm));
        else emit_word(enc_fdiv(rd, rn, rm));
        return;
    }
    if (strcmp(mn, "fcmp") == 0) {
        int rn = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rm = parse_reg(p);
        emit_word(enc_fcmp(rn, rm));
        return;
    }
    if (strcmp(mn, "fneg") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p);
        emit_word(enc_fneg(rd, rn));
        return;
    }
    if (strcmp(mn, "fabs") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p);
        emit_word(enc_fabs(rd, rn));
        return;
    }
    if (strcmp(mn, "fsqrt") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p);
        emit_word(enc_fsqrt(rd, rn));
        return;
    }
    if (strcmp(mn, "frintm") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p);
        emit_word(enc_frintm(rd, rn));
        return;
    }
    if (strcmp(mn, "frintp") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p);
        emit_word(enc_frintp(rd, rn));
        return;
    }
    if (strcmp(mn, "fcvt") == 0) {
        if (p->cur.type != AT_IDENT) { fprintf(stderr, "error (linea %d): se esperaba un registro\n", p->cur.line); exit(1); }
        int rd = parse_reg(p);
        aexpect(p, AT_COMMA, "','");
        if (p->cur.type != AT_IDENT) { fprintf(stderr, "error (linea %d): se esperaba un registro\n", p->cur.line); exit(1); }
        int src_is_single = (p->cur.text[0] == 's') ? 1 : 0;
        int rn = parse_reg(p);
        emit_word(enc_fcvt(rd, rn, src_is_single));
        return;
    }
    if (strcmp(mn, "cset") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        if (p->cur.type != AT_IDENT) { fprintf(stderr, "error (linea %d): se esperaba una condicion\n", p->cur.line); exit(1); }
        int cond = cond_from_name(p->cur.text);
        aadvance(p);
        emit_word(enc_cset(rd, cond));
        return;
    }
    if (strcmp(mn, "cneg") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rn = parse_reg(p); aexpect(p, AT_COMMA, "','");
        if (p->cur.type != AT_IDENT) { fprintf(stderr, "error (linea %d): se esperaba una condicion\n", p->cur.line); exit(1); }
        int cond = cond_from_name(p->cur.text);
        aadvance(p);
        emit_word(enc_cneg(rd, rn, cond));
        return;
    }
    if (strcmp(mn, "ret") == 0) { emit_word(enc_ret()); return; }
    if (strcmp(mn, "br") == 0) { int rn = parse_reg(p); emit_word(enc_br(rn)); return; }
    if (strcmp(mn, "svc") == 0) {
        if (p->cur.type != AT_NUMBER) { fprintf(stderr, "error (linea %d): se esperaba #inmediato\n", p->cur.line); exit(1); }
        uint32_t imm = (uint32_t)p->cur.num_value;
        aadvance(p);
        emit_word(enc_svc(imm));
        return;
    }
    if (strcmp(mn, "b") == 0 || strcmp(mn, "bl") == 0) {
        if (p->cur.type != AT_IDENT) { fprintf(stderr, "error (linea %d): se esperaba una etiqueta\n", p->cur.line); exit(1); }
        uint32_t target = symbol_address(p->cur.text);
        aadvance(p);
        int32_t imm26 = (int32_t)(((int64_t)target - (int64_t)g_current_addr) / 4);
        emit_word(mn[1] == '\0' ? enc_b(imm26) : enc_bl(imm26));
        return;
    }
    if (mn[0] == 'b' && mn[1] != '\0') { // beq, bne, bgt, bge, blt, ble, bmi, bpl...
        int cond = cond_from_name(mn + 1);
        if (p->cur.type != AT_IDENT) { fprintf(stderr, "error (linea %d): se esperaba una etiqueta\n", p->cur.line); exit(1); }
        uint32_t target = symbol_address(p->cur.text);
        aadvance(p);
        int32_t imm19 = (int32_t)(((int64_t)target - (int64_t)g_current_addr) / 4);
        emit_word(enc_bcond(cond, imm19));
        return;
    }
    if (strcmp(mn, "cbz") == 0 || strcmp(mn, "cbnz") == 0) {
        int rt = parse_reg(p); aexpect(p, AT_COMMA, "','");
        if (p->cur.type != AT_IDENT) { fprintf(stderr, "error (linea %d): se esperaba una etiqueta\n", p->cur.line); exit(1); }
        uint32_t target = symbol_address(p->cur.text);
        aadvance(p);
        int32_t imm19 = (int32_t)(((int64_t)target - (int64_t)g_current_addr) / 4);
        emit_word(mn[2] == 'z' ? enc_cbz(rt, imm19) : enc_cbnz(rt, imm19));
        return;
    }
    if (strcmp(mn, "adrp") == 0) {
        int rd = parse_reg(p); aexpect(p, AT_COMMA, "','");
        if (p->cur.type != AT_IDENT) { fprintf(stderr, "error (linea %d): se esperaba un simbolo\n", p->cur.line); exit(1); }
        uint32_t target = symbol_address(p->cur.text);
        aadvance(p);
        int32_t pages = (int32_t)(((int64_t)(target >> 12)) - ((int64_t)(g_current_addr >> 12)));
        emit_word(enc_adrp(rd, pages));
        return;
    }
    if (strcmp(mn, "ldr") == 0 || strcmp(mn, "str") == 0 || strcmp(mn, "ldrb") == 0 || strcmp(mn, "strb") == 0) {
        int rt = parse_reg(p); aexpect(p, AT_COMMA, "','");
        MemOperand m = parse_mem_operand(p);
        bool is_byte = (mn[3] == 'b');
        bool is_load = (mn[0] == 'l');
        if (m.writeback_pre) {
            if (is_byte) emit_word(is_load ? enc_ldrb_pre(rt, m.rn, m.imm) : enc_strb_pre(rt, m.rn, m.imm));
            else emit_word(is_load ? enc_ldr_pre(rt, m.rn, m.imm) : enc_str_pre(rt, m.rn, m.imm));
        } else if (m.writeback_post) {
            if (is_byte) emit_word(is_load ? enc_ldrb_post(rt, m.rn, m.imm) : enc_strb_post(rt, m.rn, m.imm));
            else emit_word(is_load ? enc_ldr_post(rt, m.rn, m.imm) : enc_str_post(rt, m.rn, m.imm));
        } else {
            bool negative = m.imm < 0;
            if (is_byte) {
                uint32_t scaled = (uint32_t)m.imm;
                if (negative) emit_word(is_load ? enc_ldurb(rt, m.rn, m.imm) : enc_sturb(rt, m.rn, m.imm));
                else emit_word(is_load ? enc_ldrb_imm(rt, m.rn, scaled) : enc_strb_imm(rt, m.rn, scaled));
            } else {
                if (negative) emit_word(is_load ? enc_ldur(rt, m.rn, m.imm) : enc_stur(rt, m.rn, m.imm));
                else emit_word(is_load ? enc_ldr_imm(rt, m.rn, (uint32_t)m.imm / 8) : enc_str_imm(rt, m.rn, (uint32_t)m.imm / 8));
            }
        }
        return;
    }
    if (strcmp(mn, "stp") == 0 || strcmp(mn, "ldp") == 0) {
        int rt1 = parse_reg(p); aexpect(p, AT_COMMA, "','");
        int rt2 = parse_reg(p); aexpect(p, AT_COMMA, "','");
        MemOperand m = parse_mem_operand(p);
        int32_t imm7 = m.imm / 8;
        bool is_load = (mn[0] == 'l');
        if (m.writeback_pre) emit_word(is_load ? enc_ldp_pre(rt1, rt2, m.rn, imm7) : enc_stp_pre(rt1, rt2, m.rn, imm7));
        else if (m.writeback_post) emit_word(is_load ? enc_ldp_post(rt1, rt2, m.rn, imm7) : enc_stp_post(rt1, rt2, m.rn, imm7));
        else emit_word(is_load ? enc_ldp_off(rt1, rt2, m.rn, imm7) : enc_stp_off(rt1, rt2, m.rn, imm7));
        return;
    }

    fprintf(stderr, "error (linea %d): instruccion no soportada: %s\n", p->cur.line, mn);
    exit(1);
}

// ---- pasada 1: calcula tamaños de seccion y direcciones de etiquetas ----

static uint32_t g_out_text_size;
static uint32_t g_out_rodata_size;

static void pass1_scan(const char *source) {
    AParser p;
    asm_lexer_init(&p.lx, source);
    aadvance(&p);

    int section = SEC_TEXT; // .text.start es lo primero que aparece siempre
    uint32_t sec_size[3] = {0, 0, 0};

    while (p.cur.type != AT_EOF) {
        if (p.cur.type == AT_NEWLINE) { aadvance(&p); continue; }

        // Una o mas etiquetas al principio de la linea.
        for (;;) {
            if (p.cur.type != AT_IDENT) break;
            AsmLexer save_lx = p.lx;
            AsmToken save_cur = p.cur;
            char name[64]; strncpy(name, p.cur.text, 63); name[63] = '\0';
            aadvance(&p);
            if (p.cur.type == AT_COLON) {
                aadvance(&p);
                define_symbol(name, section, sec_size[section]);
            } else {
                p.lx = save_lx; p.cur = save_cur; // no era una etiqueta, la devolvemos
                break;
            }
        }

        if (p.cur.type == AT_NEWLINE) { aadvance(&p); continue; }
        if (p.cur.type == AT_EOF) break;

        if (p.cur.type == AT_DIRECTIVE) {
            char dir[64]; strncpy(dir, p.cur.text, 63); dir[63] = '\0';
            aadvance(&p);
            if (strcmp(dir, "section") == 0) {
                if (p.cur.type != AT_DIRECTIVE) { fprintf(stderr, "error (linea %d): se esperaba el nombre de la seccion\n", p.cur.line); exit(1); }
                if (strcmp(p.cur.text, "text.start") == 0 || strcmp(p.cur.text, "text") == 0) section = SEC_TEXT;
                else if (strcmp(p.cur.text, "rodata") == 0) section = SEC_RODATA;
                else if (strcmp(p.cur.text, "bss") == 0) section = SEC_BSS;
                else { fprintf(stderr, "error (linea %d): seccion no reconocida: .%s\n", p.cur.line, p.cur.text); exit(1); }
                aadvance(&p);
            } else if (strcmp(dir, "global") == 0) {
                if (p.cur.type == AT_IDENT) aadvance(&p);
            } else if (strcmp(dir, "asciz") == 0) {
                if (p.cur.type != AT_STRING) { fprintf(stderr, "error (linea %d): se esperaba una cadena\n", p.cur.line); exit(1); }
                sec_size[section] += (uint32_t)strlen(p.cur.text) + 1;
                aadvance(&p);
            } else if (strcmp(dir, "space") == 0) {
                if (p.cur.type != AT_NUMBER) { fprintf(stderr, "error (linea %d): se esperaba un numero\n", p.cur.line); exit(1); }
                sec_size[section] += (uint32_t)p.cur.num_value;
                aadvance(&p);
            } else if (strcmp(dir, "align") == 0) {
                if (p.cur.type != AT_NUMBER) { fprintf(stderr, "error (linea %d): se esperaba un numero\n", p.cur.line); exit(1); }
                uint32_t bytes = 1u << (uint32_t)p.cur.num_value;
                sec_size[section] = (sec_size[section] + bytes - 1) & ~(bytes - 1);
                aadvance(&p);
            } else if (strcmp(dir, "quad") == 0) {
                // 8 bytes -- un numero inmediato, o la direccion de una
                // etiqueta (para punteros a cadena dentro de la tabla
                // de Data). Cualquiera de los dos ocupa lo mismo.
                if (p.cur.type != AT_NUMBER && p.cur.type != AT_IDENT) {
                    fprintf(stderr, "error (linea %d): se esperaba un numero o una etiqueta tras .quad\n", p.cur.line);
                    exit(1);
                }
                sec_size[section] += 8;
                aadvance(&p);
            } else {
                fprintf(stderr, "error (linea %d): directiva no soportada: .%s\n", p.cur.line, dir);
                exit(1);
            }
        } else if (p.cur.type == AT_IDENT) {
            char mn[64]; strncpy(mn, p.cur.text, 63); mn[63] = '\0';
            aadvance(&p);
            if (strcmp(mn, "mov") == 0) {
                if (p.cur.type == AT_IDENT) aadvance(&p); // registro destino
                if (p.cur.type == AT_COMMA) aadvance(&p);
                if (p.cur.type == AT_NUMBER) {
                    uint16_t chunks[4]; bool used[4];
                    int count = mov_imm_chunks((uint64_t)p.cur.num_value, chunks, used);
                    sec_size[section] += (uint32_t)(count * 4);
                } else {
                    sec_size[section] += 4; // mov reg,reg -- una sola instruccion
                }
            } else {
                sec_size[section] += 4; // cualquier otra instruccion: siempre 4 bytes
            }
            while (p.cur.type != AT_NEWLINE && p.cur.type != AT_EOF) aadvance(&p);
        } else {
            fprintf(stderr, "error (linea %d): token inesperado al principio de linea\n", p.cur.line);
            exit(1);
        }
    }

    // .text siempre acaba siendo multiplo de 4 (cada instruccion son
    // 4 bytes exactos, incluida la expansion de 'mov'), asi que no
    // hace falta alinear entre secciones -- basta con concatenar los
    // tamaños tal cual, y asi la pasada 2 coincide exactamente.
    g_text_base = 0;
    g_rodata_base = sec_size[SEC_TEXT];
    g_bss_base = g_rodata_base + sec_size[SEC_RODATA];

    for (int i = 0; i < symbol_count; i++) {
        uint32_t base = symbols[i].section == SEC_TEXT ? g_text_base
                       : symbols[i].section == SEC_RODATA ? g_rodata_base
                       : g_bss_base;
        symbols[i].address = base + symbols[i].offset;
    }

    g_out_text_size = sec_size[SEC_TEXT];
    g_out_rodata_size = sec_size[SEC_RODATA];
}

// ---- pasada 2: genera los bytes de verdad ----

static void pass2_encode(const char *source) {
    AParser p;
    asm_lexer_init(&p.lx, source);
    aadvance(&p);

    int section = SEC_TEXT;
    g_current_addr = 0; // se recalcula segun la seccion mas abajo

    while (p.cur.type != AT_EOF) {
        if (p.cur.type == AT_NEWLINE) { aadvance(&p); continue; }

        for (;;) {
            if (p.cur.type != AT_IDENT) break;
            AsmLexer save_lx = p.lx;
            AsmToken save_cur = p.cur;
            aadvance(&p);
            if (p.cur.type == AT_COLON) {
                aadvance(&p); // ya sabemos su direccion, solo consumimos el token
            } else {
                p.lx = save_lx; p.cur = save_cur;
                break;
            }
        }

        if (p.cur.type == AT_NEWLINE) { aadvance(&p); continue; }
        if (p.cur.type == AT_EOF) break;

        if (p.cur.type == AT_DIRECTIVE) {
            char dir[64]; strncpy(dir, p.cur.text, 63); dir[63] = '\0';
            aadvance(&p);
            if (strcmp(dir, "section") == 0) {
                if (strcmp(p.cur.text, "text.start") == 0 || strcmp(p.cur.text, "text") == 0) {
                    section = SEC_TEXT; g_current_addr = g_text_base + g_text_pos;
                } else if (strcmp(p.cur.text, "rodata") == 0) {
                    section = SEC_RODATA; g_current_addr = g_rodata_base + g_rodata_pos;
                } else {
                    section = SEC_BSS; // sin buffer real, no hace falta direccion
                }
                aadvance(&p);
            } else if (strcmp(dir, "global") == 0) {
                if (p.cur.type == AT_IDENT) aadvance(&p);
            } else if (strcmp(dir, "asciz") == 0) {
                uint32_t len = (uint32_t)strlen(p.cur.text) + 1;
                memcpy(g_rodata_out + g_rodata_pos, p.cur.text, len);
                g_rodata_pos += len;
                aadvance(&p);
            } else if (strcmp(dir, "space") == 0) {
                // En .bss no hace falta escribir nada (no ocupa bytes
                // reales en el archivo, solo memoria reservada en
                // tiempo de carga) -- pero en .text o .rodata SI hay
                // que escribir los ceros de verdad, o la pasada 2 se
                // queda corta respecto al tamaño que calculo la
                // pasada 1 para esa seccion (el bug original: antes
                // esto se saltaba SIEMPRE, asumiendo que .space solo
                // se usaba en .bss).
                uint32_t n = (uint32_t)p.cur.num_value;
                if (section == SEC_TEXT) {
                    for (uint32_t i = 0; i < n; i++) g_text_out[g_text_pos++] = 0;
                } else if (section == SEC_RODATA) {
                    for (uint32_t i = 0; i < n; i++) g_rodata_out[g_rodata_pos++] = 0;
                }
                aadvance(&p);
            } else if (strcmp(dir, "align") == 0) {
                uint32_t bytes = 1u << (uint32_t)p.cur.num_value;
                if (section == SEC_TEXT) {
                    uint32_t target = (g_text_pos + bytes - 1) & ~(bytes - 1);
                    while (g_text_pos < target) g_text_out[g_text_pos++] = 0;
                } else if (section == SEC_RODATA) {
                    uint32_t target = (g_rodata_pos + bytes - 1) & ~(bytes - 1);
                    while (g_rodata_pos < target) g_rodata_out[g_rodata_pos++] = 0;
                }
                aadvance(&p);
            } else if (strcmp(dir, "quad") == 0) {
                uint64_t val;
                if (p.cur.type == AT_IDENT) {
                    val = symbol_address(p.cur.text);
                } else {
                    val = (uint64_t)(int64_t)p.cur.num_value;
                }
                if (section == SEC_RODATA) {
                    for (int b = 0; b < 8; b++) g_rodata_out[g_rodata_pos++] = (uint8_t)(val >> (b * 8));
                } else if (section == SEC_TEXT) {
                    for (int b = 0; b < 8; b++) g_text_out[g_text_pos++] = (uint8_t)(val >> (b * 8));
                }
                aadvance(&p);
            }
        } else if (p.cur.type == AT_IDENT) {
            char mn[64]; strncpy(mn, p.cur.text, 63); mn[63] = '\0';
            aadvance(&p);
            encode_mnemonic(&p, mn);
        }

        // Cualquier resto de la linea que no se haya consumido (no
        // deberia quedar nada tras un directivo/instruccion bien
        // formado) lo saltamos para no atascarnos.
        while (p.cur.type != AT_NEWLINE && p.cur.type != AT_EOF) aadvance(&p);
    }
}

// ---- punto de entrada ----

int64_t assemble(const char *source, uint8_t *out_buf, uint32_t out_buf_size) {
    symbol_count = 0;

    pass1_scan(source);

    if (g_out_text_size + g_out_rodata_size > out_buf_size) {
        fprintf(stderr, "error: el binario resultante (%u bytes) no cabe en el buffer (%u bytes)\n",
                g_out_text_size + g_out_rodata_size, out_buf_size);
        return -1;
    }

    g_text_out = out_buf;
    g_text_pos = 0;
    g_rodata_out = out_buf + g_out_text_size;
    g_rodata_pos = 0;

    pass2_encode(source);

    if (g_text_pos != g_out_text_size || g_rodata_pos != g_out_rodata_size) {
        fprintf(stderr, "error interno: la pasada 2 no coincide con el tamaño calculado en la pasada 1 "
                        "(texto: %u vs %u, rodata: %u vs %u)\n",
                g_text_pos, g_out_text_size, g_rodata_pos, g_out_rodata_size);
        return -1;
    }

    return (int64_t)(g_out_text_size + g_out_rodata_size);
}
