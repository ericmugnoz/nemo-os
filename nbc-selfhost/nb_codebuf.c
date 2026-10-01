// nb_codebuf.c — ver nb_codebuf.h.

#include "nb_codebuf.h"
#include "nb_encode.h"
#include <stddef.h>

extern void *nb_alloc(uint32_t n);
extern void nb_free(void *p);

void nb_codebuf_init(nb_codebuf_t *cb) {
    cb->words = NULL;
    cb->count = 0;
    cb->cap = 0;
}

void nb_codebuf_free(nb_codebuf_t *cb) {
    if (cb->words) nb_free(cb->words);
    cb->words = NULL; cb->count = 0; cb->cap = 0;
}

uint32_t nb_codebuf_here(const nb_codebuf_t *cb) {
    return cb->count;
}

uint32_t nb_codebuf_emit(nb_codebuf_t *cb, uint32_t word) {
    if (cb->count >= cb->cap) {
        uint32_t new_cap = cb->cap == 0 ? 64 : cb->cap * 2;
        uint32_t *new_words = (uint32_t *)nb_alloc(new_cap * (uint32_t)sizeof(uint32_t));
        // nb_alloc no deberia fallar en la practica (el monton de la
        // tarea es de varios MB, y el codigo de un programa razonable
        // no se acerca a eso) -- sin manejo de errores real todavia
        // (esteroide #1 del diseño), un fallo aqui simplemente detiene
        // de escribir mas instrucciones, en vez de escribir fuera de
        // los limites del buffer.
        if (!new_words) return cb->count;
        for (uint32_t i = 0; i < cb->count; i++) new_words[i] = cb->words[i];
        if (cb->words) nb_free(cb->words);
        cb->words = new_words;
        cb->cap = new_cap;
    }
    uint32_t pos = cb->count;
    cb->words[cb->count++] = word;
    return pos;
}

void nb_codebuf_patch(nb_codebuf_t *cb, uint32_t pos, uint32_t word) {
    if (pos >= cb->count) return; // defensivo -- no deberia pasar nunca
    cb->words[pos] = word;
}

uint32_t nb_codebuf_emit_bytes(nb_codebuf_t *cb, const uint8_t *bytes, uint32_t len) {
    uint32_t start = nb_codebuf_here(cb);
    for (uint32_t i = 0; i + 4 <= len; i += 4) {
        uint32_t word = (uint32_t)bytes[i] | ((uint32_t)bytes[i+1] << 8) |
                         ((uint32_t)bytes[i+2] << 16) | ((uint32_t)bytes[i+3] << 24);
        nb_codebuf_emit(cb, word);
    }
    return start;
}

void nb_codebuf_align(nb_codebuf_t *cb, uint32_t byte_align) {
    extern uint32_t nb_enc_nop(void);
    while ((nb_codebuf_here(cb) * 4) % byte_align != 0) {
        nb_codebuf_emit(cb, nb_enc_nop());
    }
}

nb_label_t *nb_label_new(void) {
    nb_label_t *l = (nb_label_t *)nb_alloc((uint32_t)sizeof(nb_label_t));
    if (!l) return NULL;
    l->target = NB_LABEL_UNDEFINED;
    l->pending = NULL;
    return l;
}

void nb_label_free(nb_label_t *label) {
    if (!label) return;
    nb_label_ref_t *r = label->pending;
    while (r) { nb_label_ref_t *next = r->next; nb_free(r); r = next; }
    nb_free(label);
}

// Recodifica UNA referencia pendiente, ahora que se conoce 'target'.
static void nb_patch_one(nb_codebuf_t *cb, nb_label_ref_t *r, uint32_t target) {
    int32_t offset_words = (int32_t)target - (int32_t)r->pos; // en palabras, con signo
    uint32_t word;
    switch (r->kind) {
        case NB_LABEL_KIND_B:     word = nb_enc_b(offset_words); break;
        case NB_LABEL_KIND_BL:    word = nb_enc_bl(offset_words); break;
        case NB_LABEL_KIND_CBZ:   word = nb_enc_cbz(r->extra, offset_words); break;
        case NB_LABEL_KIND_CBNZ:  word = nb_enc_cbnz(r->extra, offset_words); break;
        case NB_LABEL_KIND_BCOND: word = nb_enc_bcond(r->extra, offset_words); break;
        case NB_LABEL_KIND_ADRP: {
            // A diferencia de los saltos (offset EN PALABRAS relativo
            // a la propia instruccion), adrp necesita la diferencia
            // de PAGINA entre posiciones ABSOLUTAS en bytes.
            uint32_t target_byte = target * 4;
            uint32_t adrp_byte = r->pos * 4;
            int32_t page_diff = (int32_t)(target_byte >> 12) - (int32_t)(adrp_byte >> 12);
            word = nb_enc_adrp(r->extra, page_diff);
            break;
        }
        case NB_LABEL_KIND_ADD_LO12: {
            uint32_t target_byte = target * 4;
            word = nb_enc_add_imm(r->extra, r->extra, target_byte & 0xFFF);
            break;
        }
        default: return; // inalcanzable
    }
    nb_codebuf_patch(cb, r->pos, word);
}

void nb_label_define(nb_codebuf_t *cb, nb_label_t *label) {
    nb_label_define_at(cb, label, nb_codebuf_here(cb));
}

void nb_label_define_at(nb_codebuf_t *cb, nb_label_t *label, uint32_t target_word) {
    label->target = target_word;
    nb_label_ref_t *r = label->pending;
    while (r) {
        nb_patch_one(cb, r, label->target);
        nb_label_ref_t *next = r->next;
        nb_free(r);
        r = next;
    }
    label->pending = NULL;
}

// Añade una referencia pendiente a la etiqueta (se resolvera cuando
// se llame a nb_label_define).
static void nb_label_add_pending(nb_label_t *label, uint32_t pos, int32_t kind, int32_t extra) {
    nb_label_ref_t *r = (nb_label_ref_t *)nb_alloc((uint32_t)sizeof(nb_label_ref_t));
    if (!r) return; // memoria agotada -- ver la nota en nb_codebuf_emit
    r->pos = pos; r->kind = kind; r->extra = extra;
    r->next = label->pending;
    label->pending = r;
}

void nb_emit_b(nb_codebuf_t *cb, nb_label_t *label) {
    if (label->target != NB_LABEL_UNDEFINED) {
        uint32_t pos = nb_codebuf_here(cb);
        nb_codebuf_emit(cb, nb_enc_b((int32_t)label->target - (int32_t)pos));
        return;
    }
    uint32_t pos = nb_codebuf_emit(cb, 0); // huevo vacio, se rellena al definir la etiqueta
    nb_label_add_pending(label, pos, NB_LABEL_KIND_B, 0);
}

void nb_emit_bl(nb_codebuf_t *cb, nb_label_t *label) {
    if (label->target != NB_LABEL_UNDEFINED) {
        uint32_t pos = nb_codebuf_here(cb);
        nb_codebuf_emit(cb, nb_enc_bl((int32_t)label->target - (int32_t)pos));
        return;
    }
    uint32_t pos = nb_codebuf_emit(cb, 0);
    nb_label_add_pending(label, pos, NB_LABEL_KIND_BL, 0);
}

void nb_emit_cbz(nb_codebuf_t *cb, int32_t rt, nb_label_t *label) {
    if (label->target != NB_LABEL_UNDEFINED) {
        uint32_t pos = nb_codebuf_here(cb);
        nb_codebuf_emit(cb, nb_enc_cbz(rt, (int32_t)label->target - (int32_t)pos));
        return;
    }
    uint32_t pos = nb_codebuf_emit(cb, 0);
    nb_label_add_pending(label, pos, NB_LABEL_KIND_CBZ, rt);
}

void nb_emit_cbnz(nb_codebuf_t *cb, int32_t rt, nb_label_t *label) {
    if (label->target != NB_LABEL_UNDEFINED) {
        uint32_t pos = nb_codebuf_here(cb);
        nb_codebuf_emit(cb, nb_enc_cbnz(rt, (int32_t)label->target - (int32_t)pos));
        return;
    }
    uint32_t pos = nb_codebuf_emit(cb, 0);
    nb_label_add_pending(label, pos, NB_LABEL_KIND_CBNZ, rt);
}

void nb_emit_bcond(nb_codebuf_t *cb, int32_t cond, nb_label_t *label) {
    if (label->target != NB_LABEL_UNDEFINED) {
        uint32_t pos = nb_codebuf_here(cb);
        nb_codebuf_emit(cb, nb_enc_bcond(cond, (int32_t)label->target - (int32_t)pos));
        return;
    }
    uint32_t pos = nb_codebuf_emit(cb, 0);
    nb_label_add_pending(label, pos, NB_LABEL_KIND_BCOND, cond);
}

void nb_emit_code_addr(nb_codebuf_t *cb, int32_t reg, nb_label_t *label) {
    if (label->target != NB_LABEL_UNDEFINED) {
        uint32_t target_byte = label->target * 4;
        uint32_t adrp_byte = nb_codebuf_here(cb) * 4;
        int32_t page_diff = (int32_t)(target_byte >> 12) - (int32_t)(adrp_byte >> 12);
        nb_codebuf_emit(cb, nb_enc_adrp(reg, page_diff));
        nb_codebuf_emit(cb, nb_enc_add_imm(reg, reg, target_byte & 0xFFF));
        return;
    }
    uint32_t pos1 = nb_codebuf_emit(cb, 0);
    nb_label_add_pending(label, pos1, NB_LABEL_KIND_ADRP, reg);
    uint32_t pos2 = nb_codebuf_emit(cb, 0);
    nb_label_add_pending(label, pos2, NB_LABEL_KIND_ADD_LO12, reg);
}
