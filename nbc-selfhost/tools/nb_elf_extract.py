#!/usr/bin/env python3
"""
nb_elf_extract.py -- combina varios .o ELF64/AArch64 relocatables
(nb_alloc.o, nb_string.o, y mas adelante nb_type.o) en UN SOLO bloque
de runtime resuelto, para que nbc-selfhost lo incruste en cada .pro
que compile.

Es, en esencia, un enlazador en miniatura: coloca el .text de cada
objeto uno detras de otro, el .bss de cada uno detras de TODOS los
.text, construye una tabla de simbolos GLOBAL para resolver llamadas
entre archivos (nb_string.o llama a nb_alloc, que vive en otro .o), y
aplica cada reubicacion contra esa tabla combinada.

Parser ELF64 propio y minimo (sin pyelftools) -- ver la nota de la
version anterior de este archivo: el formato esta fijo y documentado,
un parser directo es tan fiable como cualquier libreria para un script
que se ejecuta una vez por cambio en el runtime.
"""
import struct
import sys

STT_FUNC = 2
STT_OBJECT = 1
STB_GLOBAL = 1
SHN_UNDEF = 0

RELOC_NAMES = {
    0x113: 'R_AARCH64_ADR_PREL_PG_HI21',
    0x115: 'R_AARCH64_ADD_ABS_LO12_NC',
    0x11d: 'R_AARCH64_LDST32_ABS_LO12_NC',
    0x11e: 'R_AARCH64_LDST64_ABS_LO12_NC',
    0x11b: 'R_AARCH64_CALL26',
    0x11a: 'R_AARCH64_JUMP26',
}

def parse_elf(data):
    (ei_mag, ei_class, ei_data, *_rest) = struct.unpack_from('<4sBBBBB7s', data, 0)
    assert ei_mag == b'\x7fELF', "no es un ELF"
    assert ei_class == 2, "se esperaba ELF64"
    assert ei_data == 1, "se esperaba little-endian"
    (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags,
     e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    assert e_machine == 183, f"se esperaba AArch64 (183), se encontro {e_machine}"

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
         sh_link, sh_info, sh_addralign,
         sh_entsize) = struct.unpack_from('<IIQQQQIIQQ', data, off)
        sections.append(dict(name_off=sh_name, type=sh_type, flags=sh_flags,
                              addr=sh_addr, offset=sh_offset, size=sh_size,
                              link=sh_link, info=sh_info,
                              addralign=sh_addralign, entsize=sh_entsize))
    shstrtab = sections[e_shstrndx]
    for sec in sections:
        start = shstrtab['offset'] + sec['name_off']
        end = data.index(b'\x00', start)
        sec['name'] = data[start:end].decode()
    by_name = {s['name']: s for s in sections}
    return sections, by_name

def cstr(strtab, off):
    end = strtab.index(b'\x00', off)
    return strtab[off:end].decode()

def parse_symtab(data, sections, by_name):
    symtab_sec = by_name['.symtab']
    strtab_sec = sections[symtab_sec['link']]
    strtab = data[strtab_sec['offset']:strtab_sec['offset'] + strtab_sec['size']]
    n = symtab_sec['size'] // 24
    syms = []
    for i in range(n):
        off = symtab_sec['offset'] + i * 24
        st_name, st_info, st_other, st_shndx, st_value, st_size = \
            struct.unpack_from('<IBBHQQ', data, off)
        name = cstr(strtab, st_name) if st_name else ''
        syms.append(dict(name=name, bind=st_info >> 4, type=st_info & 0xF,
                          shndx=st_shndx, value=st_value, size=st_size, index=i))
    return syms

def parse_relocs(data, sections, sec, syms):
    n = sec['size'] // 24
    relocs = []
    for i in range(n):
        off = sec['offset'] + i * 24
        r_offset, r_info, r_addend = struct.unpack_from('<QQq', data, off)
        sym_idx = r_info >> 32
        rtype = r_info & 0xffffffff
        relocs.append(dict(offset=r_offset, type_name=RELOC_NAMES.get(rtype, hex(rtype)),
                            sym=syms[sym_idx], addend=r_addend))
    return relocs

class ObjFile:
    """Un .o ya analizado -- todo lo que hace falta de el para combinarlo con otros."""
    def __init__(self, path):
        with open(path, 'rb') as f:
            data = f.read()
        sections, by_name = parse_elf(data)
        self.path = path
        self.syms = parse_symtab(data, sections, by_name)
        text_sec = by_name.get('.text')
        self.text_bytes = data[text_sec['offset']:text_sec['offset'] + text_sec['size']] if text_sec else b''
        bss_sec = by_name.get('.bss')
        self.bss_size = bss_sec['size'] if bss_sec else 0

        # Indice de cada seccion que nos importa, para poder resolver
        # las reubicaciones RELATIVAS A SECCION (las que no llevan
        # nombre de simbolo, solo "seccion N + desplazamiento").
        # Antes se suponia que esas siempre apuntaban al .bss, lo que
        # era cierto mientras ningun archivo tuviera constantes; en
        # cuanto nb_math.c trajo sus tablas de coeficientes en
        # .rodata, esa suposicion empezo a mandar las referencias al
        # .bss (todo ceros) y las constantes se leian como 0.0.
        self.text_index = None
        self.bss_index = None
        self.rodata = []   # [(indice, bytes, alineacion)]
        for i, sec in enumerate(sections):
            nm = sec.get('name', '')
            if nm == '.text':
                self.text_index = i
            elif nm == '.bss':
                self.bss_index = i
            elif nm.startswith('.rodata'):
                blob = data[sec['offset']:sec['offset'] + sec['size']]
                self.rodata.append((i, blob, max(sec.get('addralign', 8), 8)))
        self.relocs = []
        for name, sec in by_name.items():
            if name.startswith('.rela.'):
                self.relocs.extend(parse_relocs(data, sections, sec, self.syms))
        # SHN_UNDEF (0) = definido en OTRO archivo; cualquier otro
        # indice != 0 = definido en .text/.data/.bss de ESTE MISMO
        # archivo (para nuestros .o, siempre shndx=1 -> .text ó
        # shndx=4 -> .bss, pero no hace falta suponerlo: basta con
        # "es 0 o no es 0").

def enc_adrp(rd, imm_pages):
    uimm = imm_pages & 0x1FFFFF
    return 0x90000000 | ((uimm & 3) << 29) | (((uimm >> 2) & 0x7FFFF) << 5) | rd

def enc_add_imm(rd, rn, imm12):
    return 0x91000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd

def enc_ldst_lo12(base_word, imm_scaled):
    return (base_word & ~(0xFFF << 10)) | ((imm_scaled & 0xFFF) << 10)

def enc_bl(imm26):
    return 0x94000000 | (imm26 & 0x3FFFFFF)

def enc_b(imm26):
    return 0x14000000 | (imm26 & 0x3FFFFFF)

def align(n, a):
    return (n + a - 1) & ~(a - 1)

def sec_base(o, shndx, text_base, rodata_base, bss_base):
    """Desplazamiento absoluto, dentro del bloque combinado, donde
    quedo la seccion 'shndx' de este objeto."""
    if shndx == o.text_index:
        return text_base[o.path]
    if (o.path, shndx) in rodata_base:
        return rodata_base[(o.path, shndx)]
    if shndx == o.bss_index:
        return bss_base[o.path]
    # Seccion no reconocida (.data con contenido, por ejemplo): mejor
    # parar que resolverla a un sitio equivocado en silencio.
    raise ValueError(f"seccion {shndx} de {o.path} no colocada en el bloque "
                     f"(¿.data con contenido? habria que anadirla igual que .rodata)")

def combine(paths):
    objs = [ObjFile(p) for p in paths]

    # Paso 1: colocar el .text de cada objeto uno detras de otro
    # (alineado a 16 bytes -- el tamaño que ya trae cada .o de gcc es
    # multiplo de 16 en la practica, pero alinear explicitamente no
    # cuesta nada y evita depender de esa casualidad).
    text_base = {}
    running = 0
    for o in objs:
        text_base[o.path] = running
        running += len(o.text_bytes)
        running = align(running, 16)
    total_text = running

    # Paso 1b: el .rodata de cada objeto (constantes: pi, factores de
    # conversion, coeficientes de las series de nb_math.c...) va
    # DESPUES de todo el .text y ANTES del .bss. Son datos
    # INICIALIZADOS: tienen que viajar dentro del bloque, no basta con
    # reservarles hueco como al .bss.
    rodata_base = {}   # (path, indice de seccion) -> desplazamiento absoluto
    for o in objs:
        for (idx, blob_bytes, algn) in o.rodata:
            running = align(running, algn)
            rodata_base[(o.path, idx)] = running
            running += len(blob_bytes)
    running = align(running, 16)
    total_initialized = running   # .text + .rodata: TODO lo que se guarda de verdad

    # Paso 2: el .bss de cada objeto, TODOS colocados despues.
    bss_base = {}
    running_bss = total_initialized
    for o in objs:
        bss_base[o.path] = running_bss
        running_bss += o.bss_size
        running_bss = align(running_bss, 16)
    total_size = running_bss  # tamaño total del bloque, incluyendo el hueco de .bss

    # Paso 3: tabla de simbolos GLOBAL -- nombre -> desplazamiento
    # absoluto dentro del bloque combinado. Solo simbolos DEFINIDOS
    # (shndx != SHN_UNDEF) entran aqui; una llamada externa se resuelve
    # buscando el NOMBRE en esta tabla, sin importar de que archivo
    # venga la definicion.
    global_syms = {}
    for o in objs:
        for s in o.syms:
            if not s['name'] or s['shndx'] == SHN_UNDEF:
                continue
            if s['type'] == STT_FUNC:
                global_syms[s['name']] = text_base[o.path] + s['value']
            elif s['type'] == STT_OBJECT:
                global_syms[s['name']] = sec_base(o, s['shndx'], text_base, rodata_base, bss_base) + s['value']

    # Paso 4: aplicar las reubicaciones de cada objeto contra el
    # bloque combinado.
    blob = bytearray(total_initialized)  # .text + .rodata, ya con su alineacion
    for o in objs:
        base = text_base[o.path]
        blob[base:base + len(o.text_bytes)] = o.text_bytes
    for o in objs:
        for (idx, blob_bytes, _algn) in o.rodata:
            b = rodata_base[(o.path, idx)]
            blob[b:b + len(blob_bytes)] = blob_bytes

    for o in objs:
        base = text_base[o.path]
        for r in o.relocs:
            instr_abs = base + r['offset']
            sym = r['sym']
            if sym['name']:
                # simbolo con nombre: funcion (de este archivo o de
                # otro -- da igual, global_syms ya tiene la absoluta)
                target_abs = global_syms[sym['name']] + r['addend']
            else:
                # Reubicacion relativa a SECCION ("seccion N + M"):
                # siempre del PROPIO archivo que contiene la
                # instruccion, pero hay que mirar DE QUE seccion se
                # trata -- .text, .rodata o .bss -- en vez de suponer
                # que es el .bss.
                target_abs = sec_base(o, sym['shndx'], text_base, rodata_base, bss_base) + r['addend']

            word = struct.unpack_from('<I', blob, instr_abs)[0]
            t = r['type_name']
            if t == 'R_AARCH64_ADR_PREL_PG_HI21':
                rd = word & 0x1F
                page_diff = (target_abs >> 12) - (instr_abs >> 12)
                new_word = enc_adrp(rd, page_diff)
            elif t == 'R_AARCH64_ADD_ABS_LO12_NC':
                rd = word & 0x1F
                rn = (word >> 5) & 0x1F
                new_word = enc_add_imm(rd, rn, target_abs & 0xFFF)
            elif t == 'R_AARCH64_LDST32_ABS_LO12_NC':
                new_word = enc_ldst_lo12(word, (target_abs & 0xFFF) >> 2)
            elif t == 'R_AARCH64_LDST64_ABS_LO12_NC':
                new_word = enc_ldst_lo12(word, (target_abs & 0xFFF) >> 3)
            elif t == 'R_AARCH64_CALL26':
                new_word = enc_bl((target_abs - instr_abs) // 4)
            elif t == 'R_AARCH64_JUMP26':
                new_word = enc_b((target_abs - instr_abs) // 4)
            else:
                raise ValueError(f"tipo de reubicacion no manejado: {t} en {o.path}")
            struct.pack_into('<I', blob, instr_abs, new_word)

    exported = {name: off for name, off in global_syms.items()
                if off < total_text and any(
                    s['name'] == name and s['bind'] == STB_GLOBAL
                    for o in objs for s in o.syms)}

    return dict(blob=bytes(blob), total_text=total_initialized, total_size=total_size,
                bss_total=total_size - total_initialized, exported=exported,
                text_base=text_base, bss_base=bss_base, rodata_base=rodata_base,
                code_only=total_text, objs=objs)

def write_c_header(result, path, guard='NB_RUNTIME_BLOB_H'):
    blob = result['blob']
    lines = []
    lines.append(f"// {path} -- GENERADO por nb_elf_extract.py. No editar a mano.")
    lines.append(f"// Bloque de runtime resuelto (nb_alloc.c + nb_string.c), listo para")
    lines.append(f"// incrustar en cada .pro que nbc-selfhost compile. Ver la nota de")
    lines.append(f"// alineacion: este bloque DEBE colocarse en un")
    lines.append(f"// desplazamiento multiplo de 4096 dentro del .pro final -- todas las")
    lines.append(f"// referencias internas (adrp, llamadas entre funciones) ya estan")
    lines.append(f"// resueltas asumiendo eso, no antes.")
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}")
    lines.append(f"#include <stdint.h>")
    lines.append(f"")
    lines.append(f"#define NB_RUNTIME_BLOB_TEXT_SIZE {result['total_text']}u")
    lines.append(f"#define NB_RUNTIME_BLOB_BSS_SIZE {result['bss_total']}u")
    lines.append(f"#define NB_RUNTIME_BLOB_TOTAL_SIZE {result['total_size']}u")
    lines.append(f"")
    lines.append(f"static const uint8_t NB_RUNTIME_BLOB_TEXT[{len(blob)}] = {{")
    for i in range(0, len(blob), 16):
        chunk = blob[i:i+16]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("};")
    lines.append("")
    # El nombre va como ARRAY DE CARACTERES, no como puntero. Con un
    # puntero, el enlazador escribe en la tabla una direccion ABSOLUTA
    # calculada como si el programa se cargara en la direccion 0 --
    # pero Nemo OS lo carga donde le toca (0x4c600000, por ejemplo), y
    # esos punteros quedan apuntando a la nada. Un array vive DENTRO
    # de la propia tabla, asi que no hay direccion que corregir.
    # (Fallo real: nbc.pro reventaba con Data Abort al buscar "nb_sin";
    # los programas que no tocaban esta tabla funcionaban, lo que lo
    # hacia parecer un problema del programa compilado y no del
    # compilador.)
    lines.append("#define NB_RUNTIME_SYM_NAME_MAX 32")
    lines.append("typedef struct { char name[NB_RUNTIME_SYM_NAME_MAX]; uint32_t offset; } nb_runtime_sym_t;")
    lines.append("")
    exported_sorted = sorted(result['exported'].items(), key=lambda kv: kv[1])
    lines.append(f"static const nb_runtime_sym_t NB_RUNTIME_SYMS[{len(exported_sorted)}] = {{")
    for name, off in exported_sorted:
        lines.append(f'    {{ "{name}", 0x{off:x}u }},')
    lines.append("};")
    lines.append(f"#define NB_RUNTIME_SYMS_COUNT {len(exported_sorted)}u")
    lines.append("")
    lines.append(f"#endif")
    with open(path, 'w') as f:
        f.write("\n".join(lines) + "\n")

def main():
    # Con "-o <ruta>" la cabecera se escribe DIRECTAMENTE donde toca,
    # sin pasar por /tmp ni depender de que alguien se acuerde de
    # copiarla despues. Ese 'cp' olvidado ya dejo una vez una cabecera
    # vieja compilada dentro del kernel, con un fallo dificil de
    # rastrear: el Makefile lo llama asi para que no pueda repetirse.
    args = sys.argv[1:]
    out_header = None
    if '-o' in args:
        i = args.index('-o')
        out_header = args[i + 1]
        args = args[:i] + args[i + 2:]
    paths = args or ['/mnt/user-data/uploads/nb_alloc.o']
    result = combine(paths)
    print(f"Archivos combinados: {paths}")
    print(f".text combinado: {result['total_text']} bytes")
    print(f".bss combinado: {result['bss_total']} bytes")
    print(f"Tamaño total del bloque: {result['total_size']} bytes")
    print(f"\nFunciones exportadas (llamables desde codigo .nb generado):")
    for name, off in sorted(result['exported'].items(), key=lambda kv: kv[1]):
        print(f"  {name:20s} desplazamiento=0x{off:x}")

    with open('/tmp/nb_runtime_blob.bin', 'wb') as f:
        f.write(result['blob'])
    print(f"\nBloque de .text ya resuelto escrito en /tmp/nb_runtime_blob.bin ({len(result['blob'])} bytes)")

    header_path = out_header if out_header else '/tmp/nb_runtime_blob.h'
    write_c_header(result, header_path)
    print(f"Cabecera C generada en {header_path} -- esta es la que nbc-selfhost.c incluye directamente.")

if __name__ == '__main__':
    main()
