// nb_symtab.c — ver nb_symtab.h.

#include "nb_symtab.h"
#include "nb_encode.h"
#include <stddef.h>

extern void *nb_alloc(uint32_t n);
extern void nb_free(void *p);
extern bool nb_string_eq(nb_string_t *a, nb_string_t *b);

// ---- Globales ----

void nb_symtab_init(nb_symtab_t *st) {
    st->entries = NULL; st->count = 0; st->cap = 0;
    st->patches = NULL; st->patch_count = 0; st->patch_cap = 0;
    st->literals = NULL; st->literal_count = 0; st->literal_cap = 0;
    st->data_patches = NULL; st->data_patch_count = 0; st->data_patch_cap = 0;
    st->next_offset = 0;
    st->data_region_pos = 0;
}

void nb_symtab_free(nb_symtab_t *st) {
    extern void nb_string_release(nb_string_t *s);
    for (uint32_t i = 0; i < st->count; i++) nb_string_release(st->entries[i].name);
    if (st->entries) nb_free(st->entries);
    if (st->patches) nb_free(st->patches);
    for (uint32_t i = 0; i < st->literal_count; i++) if (st->literals[i].bytes) nb_free(st->literals[i].bytes);
    if (st->literals) nb_free(st->literals);
    if (st->data_patches) nb_free(st->data_patches);
    st->entries = NULL; st->count = 0; st->cap = 0;
    st->patches = NULL; st->patch_count = 0; st->patch_cap = 0;
    st->literals = NULL; st->literal_count = 0; st->literal_cap = 0;
    st->data_patches = NULL; st->data_patch_count = 0; st->data_patch_cap = 0;
}

nb_global_entry_t *nb_symtab_find_global(nb_symtab_t *st, nb_string_t *name) {
    for (uint32_t i = 0; i < st->count; i++) {
        if (nb_string_eq(st->entries[i].name, name)) return &st->entries[i];
    }
    return NULL;
}

nb_global_entry_t *nb_symtab_add_global(nb_symtab_t *st, nb_string_t *name) {
    if (st->count >= st->cap) {
        uint32_t new_cap = st->cap == 0 ? 8 : st->cap * 2;
        nb_global_entry_t *new_entries = (nb_global_entry_t *)nb_alloc(new_cap * (uint32_t)sizeof(nb_global_entry_t));
        if (!new_entries) return NULL; // memoria agotada -- ver la nota de siempre en este proyecto
        for (uint32_t i = 0; i < st->count; i++) new_entries[i] = st->entries[i];
        if (st->entries) nb_free(st->entries);
        st->entries = new_entries;
        st->cap = new_cap;
    }
    nb_global_entry_t *e = &st->entries[st->count++];
    e->name = name;
    e->offset = st->next_offset;
    st->next_offset += 8; // cada variable escalar ocupa 8 bytes -- arrays de tamaño mayor son una pieza aparte
    return e;
}

void nb_emit_global_addr(nb_symtab_t *st, nb_codebuf_t *cb, uint32_t reg, uint32_t var_offset) {
    uint32_t adrp_pos = nb_codebuf_emit(cb, nb_enc_adrp((int32_t)reg, 0)); // relleno, se corrige despues
    uint32_t add_pos = nb_codebuf_emit(cb, nb_enc_add_imm((int32_t)reg, (int32_t)reg, 0)); // idem

    if (st->patch_count >= st->patch_cap) {
        uint32_t new_cap = st->patch_cap == 0 ? 8 : st->patch_cap * 2;
        nb_global_patch_t *new_patches = (nb_global_patch_t *)nb_alloc(new_cap * (uint32_t)sizeof(nb_global_patch_t));
        if (!new_patches) return; // memoria agotada -- el par adrp/add se queda relleno de ceros,
                                    // apuntaria a un sitio incorrecto, pero no se sale de los limites de nada
        for (uint32_t i = 0; i < st->patch_count; i++) new_patches[i] = st->patches[i];
        if (st->patches) nb_free(st->patches);
        st->patches = new_patches;
        st->patch_cap = new_cap;
    }
    nb_global_patch_t *p = &st->patches[st->patch_count++];
    p->adrp_pos = adrp_pos; p->add_pos = add_pos; p->reg = reg; p->var_offset = var_offset;
}

void nb_symtab_resolve_globals(nb_symtab_t *st, nb_codebuf_t *cb, uint32_t code_size_words) {
    uint32_t data_region_start_bytes = st->data_region_pos != 0 ? st->data_region_pos : code_size_words * 4;
    for (uint32_t i = 0; i < st->patch_count; i++) {
        nb_global_patch_t *p = &st->patches[i];
        uint32_t target_byte = data_region_start_bytes + p->var_offset;
        uint32_t adrp_byte = p->adrp_pos * 4;
        // adrp trabaja en PAGINAS de 4096 bytes -- el numero de
        // paginas de diferencia entre la propia instruccion adrp y el
        // destino, con signo (puede ser negativo si el destino cae
        // "antes", aunque en este diseño -- datos siempre despues del
        // codigo -- en la practica siempre es positivo o cero).
        int32_t page_diff = (int32_t)(target_byte >> 12) - (int32_t)(adrp_byte >> 12);
        uint32_t low12 = target_byte & 0xFFFu;
        nb_codebuf_patch(cb, p->adrp_pos, nb_enc_adrp((int32_t)p->reg, page_diff));
        nb_codebuf_patch(cb, p->add_pos, nb_enc_add_imm((int32_t)p->reg, (int32_t)p->reg, low12));
    }
}

uint32_t nb_symtab_add_literal_bytes(nb_symtab_t *st, const uint8_t *bytes, uint32_t len) {
    if (st->literal_count >= st->literal_cap) {
        uint32_t new_cap = st->literal_cap == 0 ? 8 : st->literal_cap * 2;
        nb_literal_entry_t *ne = (nb_literal_entry_t *)nb_alloc(new_cap * (uint32_t)sizeof(nb_literal_entry_t));
        if (!ne) return st->next_offset; // memoria agotada -- ver la nota de siempre
        for (uint32_t i = 0; i < st->literal_count; i++) ne[i] = st->literals[i];
        if (st->literals) nb_free(st->literals);
        st->literals = ne; st->literal_cap = new_cap;
    }
    uint8_t *copy = (uint8_t *)nb_alloc(len);
    if (copy) for (uint32_t i = 0; i < len; i++) copy[i] = bytes[i];
    nb_literal_entry_t *e = &st->literals[st->literal_count++];
    e->offset = st->next_offset;
    e->bytes = copy;
    e->len = len;
    st->next_offset += len;
    return e->offset;
}

uint8_t *nb_symtab_build_data_image(nb_symtab_t *st, uint32_t code_size_words, uint32_t *out_len) {
    uint32_t len = st->next_offset;
    uint8_t *img = (uint8_t *)nb_alloc(len > 0 ? len : 1);
    *out_len = len;
    if (!img) return NULL;
    for (uint32_t i = 0; i < len; i++) img[i] = 0; // huecos de variables globales -- cero, como cualquier .bss
    for (uint32_t i = 0; i < st->literal_count; i++) {
        nb_literal_entry_t *e = &st->literals[i];
        if (!e->bytes) continue; // reserva fallida en su momento -- se deja a cero, ver la nota de siempre
        for (uint32_t j = 0; j < e->len; j++) img[e->offset + j] = e->bytes[j];
    }
    // Parcheos dato-sobre-dato: en el hueco se escribe el desplazamiento
    // del destino RELATIVO AL INICIO DE LA REGION DE DATOS (8 bytes,
    // little-endian). Quien lo lee (Read de una cadena de Data) le suma
    // al ejecutarse la direccion REAL de la region, que obtiene con
    // adrp/add.
    //
    // Antes se escribia "inicio de la region en el archivo + destino" y se
    // llamaba direccion ABSOLUTA, pero solo lo habria sido si el programa
    // se cargara en la direccion 0. En Nemo OS cada programa se carga donde
    // hay sitio: leer una cadena de Data apuntaba a memoria del kernel y el
    // programa caia (hallazgo H13 de la auditoria del compilador viejo, que
    // seguia vivo en el nuevo; ningun ejemplo usaba Data con cadenas).
    (void)code_size_words;
    for (uint32_t i = 0; i < st->data_patch_count; i++) {
        nb_data_patch_t *p = &st->data_patches[i];
        uint64_t rel = (uint64_t)p->target_offset;
        for (int b = 0; b < 8; b++) img[p->offset_to_write + b] = (uint8_t)(rel >> (b * 8));
    }
    return img;
}

void nb_symtab_add_data_patch(nb_symtab_t *st, uint32_t offset_to_write, uint32_t target_offset) {
    if (st->data_patch_count >= st->data_patch_cap) {
        uint32_t new_cap = st->data_patch_cap == 0 ? 8 : st->data_patch_cap * 2;
        nb_data_patch_t *ne = (nb_data_patch_t *)nb_alloc(new_cap * (uint32_t)sizeof(nb_data_patch_t));
        if (!ne) return;
        for (uint32_t i = 0; i < st->data_patch_count; i++) ne[i] = st->data_patches[i];
        if (st->data_patches) nb_free(st->data_patches);
        st->data_patches = ne; st->data_patch_cap = new_cap;
    }
    st->data_patches[st->data_patch_count].offset_to_write = offset_to_write;
    st->data_patches[st->data_patch_count].target_offset = target_offset;
    st->data_patch_count++;
}

// ---- Locales ----

void nb_local_scope_init(nb_local_scope_t *sc) {
    sc->entries = NULL; sc->count = 0; sc->cap = 0;
    sc->next_offset = 16; // +16: justo despues del par x29/x30 guardado
                           // por el prologo (stp x29,x30,[sp,#-N]!;
                           // add x29,sp,#0 -- con ESE prologo, x29
                           // apunta a la BASE del marco, donde vive el
                           // par guardado en [x29+0]/[x29+8], y las
                           // locales ocupan el espacio de encima).
}

void nb_local_scope_free(nb_local_scope_t *sc) {
    if (sc->entries) nb_free(sc->entries);
    sc->entries = NULL; sc->count = 0; sc->cap = 0;
}

nb_local_entry_t *nb_local_scope_find(nb_local_scope_t *sc, nb_string_t *name) {
    for (uint32_t i = 0; i < sc->count; i++) {
        if (nb_string_eq(sc->entries[i].name, name)) return &sc->entries[i];
    }
    return NULL;
}

nb_local_entry_t *nb_local_scope_add(nb_local_scope_t *sc, nb_string_t *name) {
    if (sc->count >= sc->cap) {
        uint32_t new_cap = sc->cap == 0 ? 8 : sc->cap * 2;
        nb_local_entry_t *new_entries = (nb_local_entry_t *)nb_alloc(new_cap * (uint32_t)sizeof(nb_local_entry_t));
        if (!new_entries) return NULL;
        for (uint32_t i = 0; i < sc->count; i++) new_entries[i] = sc->entries[i];
        if (sc->entries) nb_free(sc->entries);
        sc->entries = new_entries;
        sc->cap = new_cap;
    }
    nb_local_entry_t *e = &sc->entries[sc->count++];
    e->name = name;
    e->offset = sc->next_offset;
    sc->next_offset += 8;
    return e;
}
