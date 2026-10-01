#!/usr/bin/env python3
# arm64.py -- un emulador minimo de ARM64 para ejecutar los .pro de Nemo Basic
#. Carga el programa como el kernel (en 0x80c00000), lo ejecuta
# instruccion a instruccion y atiende unas pocas llamadas al sistema. Una
# instruccion que no conoce DETIENE la emulacion y lo dice: nunca da un
# resultado falso en silencio.
#
#   python3 arm64.py programa.pro            -> lo que el programa escribe
import struct, sys, math

BASE = 0x80c00000
PILA_TOP = 0x90100000
PILA_TAM = 0x100000
FIN_LIMPIO = 0xF1F1F1F0        # x30 al empezar: si el programa vuelve aqui, ha terminado
MASK = (1 << 64) - 1

class Parada(Exception):
    pass

def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v

def ror(v, r, w):
    r %= w
    return ((v >> r) | (v << (w - r))) & ((1 << w) - 1)

def decode_bitmask(n, imms, immr, w):
    ln = (n << 6) | (~imms & 0x3F)
    length = ln.bit_length() - 1
    if length < 1:
        raise Parada("mascara logica no valida")
    esize = 1 << length
    levels = esize - 1
    s = imms & levels
    r = immr & levels
    welem = (1 << (s + 1)) - 1
    welem = ror(welem, r, esize)
    v = 0
    for i in range(w // esize):
        v |= welem << (i * esize)
    return v

class Cpu:
    def __init__(self, pro, salida=None, max_pasos=5_000_000):
        if pro[:4] != b"NEXE":
            raise Parada("no es un .pro")
        _, _, entrada, code_size, mem_size, flags = struct.unpack_from("<4sIIIII", pro, 0)
        self.mem = bytearray(max(mem_size, code_size) + 4096)
        cod = pro[24:24 + code_size]
        self.mem[0:len(cod)] = cod
        self.pila = bytearray(PILA_TAM)
        self.x = [0] * 31
        self.x[30] = FIN_LIMPIO      # volver aqui = el programa termino (un programa sin End)
        self.d = [0] * 32          # coma flotante, en bits (doble precision)
        self.sp = PILA_TOP
        self.pc = BASE + entrada
        self.n = self.z = self.c = self.v = 0
        self.salida = salida if salida is not None else []
        self.max_pasos = max_pasos
        self.pasos = 0
        self.fin = False
        self.registro = []

    # ---- memoria ----
    def _zona(self, dir, n):
        if BASE <= dir and dir + n <= BASE + len(self.mem):
            return self.mem, dir - BASE
        if PILA_TOP - PILA_TAM <= dir and dir + n <= PILA_TOP:
            return self.pila, dir - (PILA_TOP - PILA_TAM)
        raise Parada("acceso a memoria fuera del programa: 0x%x (pc 0x%x, desplazamiento 0x%x)" % (dir, self.pc, self.pc - BASE))
    def leer(self, dir, n):
        m, o = self._zona(dir, n)
        return int.from_bytes(m[o:o + n], "little")
    def escribir(self, dir, n, v):
        m, o = self._zona(dir, n)
        m[o:o + n] = (v & ((1 << (8 * n)) - 1)).to_bytes(n, "little")
    def cadena(self, dir, maximo=4096):
        r = bytearray()
        while len(r) < maximo:
            b = self.leer(dir + len(r), 1)
            if b == 0:
                break
            r.append(b)
        return r.decode("utf-8", "replace")

    # ---- registros: 31 = xzr o sp, segun la instruccion ----
    def r(self, i, sp=False):
        if i == 31:
            return self.sp if sp else 0
        return self.x[i]
    def w(self, i, v, sp=False, bits=64):
        v &= (1 << bits) - 1
        if i == 31:
            if sp:
                self.sp = v
            return
        self.x[i] = v

    def flags_suma(self, a, b, carry, bits):
        m = (1 << bits) - 1
        us = (a & m) + (b & m) + carry
        ss = sx(a, bits) + sx(b, bits) + carry
        res = us & m
        self.n = res >> (bits - 1)
        self.z = int(res == 0)
        self.c = int(us > m)
        self.v = int(sx(res, bits) != ss)
        return res

    def cond(self, c):
        base = c >> 1
        r = [self.z == 1, self.c == 1, self.n == 1, self.v == 1,
             self.c == 1 and self.z == 0, self.n == self.v,
             self.n == self.v and self.z == 0, True][base]
        if (c & 1) and c != 15:
            r = not r
        return r

    def shift(self, v, t, amt, bits):
        m = (1 << bits) - 1
        v &= m
        if t == 0: return (v << amt) & m
        if t == 1: return v >> amt
        if t == 2: return (sx(v, bits) >> amt) & m
        return ror(v, amt, bits)

    # ---- un sistema de archivos en memoria, para las pruebas ----
    # Llamadas de Nemo OS: 41-44 (leer por lineas), 70-77 (general), 78-80
    # (recorrer una carpeta), 81-84 (por nombre) y 24 (crear carpeta).
    def fs_init(self):
        self.archivos = {}        # nombre -> bytearray
        self.carpetas = {}        # nombre -> inodo (2, 3, ...)
        self.h_lectura = {}
        self.h_general = {}
        self.h_carpeta = {}
        self.sig_h = 1
    def fs_syscall(self, num, a0, a1, a2):
        if not hasattr(self, "archivos"): self.fs_init()
        if num == 41:                                   # abrir para leer lineas
            n = self.cadena(a0)
            if n not in self.archivos: return -1
            h = self.sig_h; self.sig_h += 1
            self.h_lectura[h] = [self.archivos[n], 0]
            return h
        if num == 42:                                   # una linea
            e = self.h_lectura.get(a0)
            if not e: return 0
            datos, pos = e
            fin = datos.find(b"\n", pos)
            if fin < 0: fin = len(datos)
            linea = datos[pos:fin]
            e[1] = fin + 1
            n = min(len(linea), a2 - 1)
            for i in range(n): self.escribir(a1 + i, 1, linea[i])
            self.escribir(a1 + n, 1, 0)
            return n
        if num == 43:
            e = self.h_lectura.get(a0)
            return 1 if (not e or e[1] >= len(e[0])) else 0
        if num == 44:
            self.h_lectura.pop(a0, None); return 0
        if num == 70:                                   # abrir general (modo 1: crea vacio)
            n = self.cadena(a0)
            if a1 == 1 or n not in self.archivos: self.archivos[n] = bytearray()
            h = self.sig_h; self.sig_h += 1
            self.h_general[h] = [n, 0]
            return h
        if num in (71, 72, 73, 74, 75, 76, 77):
            e = self.h_general.get(a0)
            if not e: return -1
            nombre, pos = e
            datos = self.archivos[nombre]
            if num == 71:
                n = min(a2, len(datos) - pos)
                for i in range(n): self.escribir(a1 + i, 1, datos[pos + i])
                e[1] = pos + n; return n
            if num == 72:
                b = bytearray(self.leer(a1 + i, 1) for i in range(a2))
                datos[pos:pos + a2] = b
                e[1] = pos + a2; return 0
            if num == 73: return pos
            if num == 74:
                if a1 > len(datos): return -1
                e[1] = a1; return 0
            if num == 75: return len(datos)
            if num == 76: return 1 if pos >= len(datos) else 0
            if num == 77: self.h_general.pop(a0, None); return 0
        if num == 78:                                   # abrir una carpeta
            h = self.sig_h; self.sig_h += 1
            if a0 == 0: nombres = sorted(self.archivos) + sorted(self.carpetas)
            else: nombres = []
            self.h_carpeta[h] = nombres
            return h
        if num == 79:
            l = self.h_carpeta.get(a0)
            if not l: return 0
            n = l.pop(0).encode()
            k = min(len(n), a2 - 1)
            for i in range(k): self.escribir(a1 + i, 1, n[i])
            self.escribir(a1 + k, 1, 0)
            return k
        if num == 80:
            self.h_carpeta.pop(a0, None); return 0
        if num == 81:
            n = self.cadena(a0); return len(self.archivos[n]) if n in self.archivos else -1
        if num == 82:
            n = self.cadena(a0)
            return 1 if n in self.archivos else (2 if n in self.carpetas else 0)
        if num == 83:
            n = self.cadena(a0); return self.carpetas.get(n, -1)
        if num == 84:
            n = self.cadena(a0)
            if n in self.archivos: del self.archivos[n]; return 0
            if n in self.carpetas: del self.carpetas[n]; return 0
            return -1
        if num == 24:
            n = self.cadena(a0)
            if n in self.carpetas: return -1
            self.carpetas[n] = 2 + len(self.carpetas)
            return self.carpetas[n]
        return None

    # ---- llamadas al sistema ----
    def svc(self):
        num = self.x[8]
        a0 = self.x[0]
        if num == 0:                      # SYS_EXIT
            self.fin = True
            raise Parada("fin")
        if num == 11:                     # SYS_WRITE_STRING
            self.salida.append(self.cadena(a0))
            self.x[0] = 0
            return
        if num == 28:                     # SYS_DEBUG_LOG
            self.registro.append(self.cadena(a0))
            self.x[0] = 0
            return
        # ventana y controles (para probar los anclajes)
        if num == 33: self.x[0] = (640 << 32) | 480; return
        if num in (107, 108, 109):
            if not hasattr(self, "gadgets"): self.gadgets = {}
            g = self.gadgets.setdefault(a0, [0, 0, 0, 0])
            if num == 108: g[0], g[1] = sx(self.x[1], 64), sx(self.x[2], 64); self.x[0] = 0; return
            if num == 109: g[2], g[3] = self.x[1], self.x[2]; self.x[0] = 0; return
            self.x[0] = ((g[0] & 0xFFFF) << 48) | ((g[1] & 0xFFFF) << 32) | ((g[2] & 0xFFFF) << 16) | (g[3] & 0xFFFF)
            return
        if 150 <= num <= 200 or num in (100, 101, 102, 103, 104, 105, 188):   # crear un control: un numero distinto a cada uno
            if not hasattr(self, "sig_gadget"): self.sig_gadget = 1
            self.x[0] = self.sig_gadget; self.sig_gadget += 1
            return
        # ---- imagenes de verdad, y filas de pixeles ----
        #
        # Hasta ahora el emulador no tenia imagenes, asi que CreateImage
        # devolvia 0 y todo lo que dibujara o midiera pixeles se ejecutaba
        # "bien" sin hacer nada: una prueba de ejecucion de FillRow o
        # RowRun habria pasado en verde midiendo el aire.
        #
        # Cada imagen es [ancho, alto, bytearray] con los pixeles en el
        # MISMO orden que src/syscall.c: R, G, B, 255.
        if num in (51, 52, 88, 289, 290, 291, 292):
            if not hasattr(self, "imgs"):
                self.imgs = {}
                self.sig_img = 0
            if num == 52:                                  # CreateImage(ancho, alto)
                w, h = a0 & 0xFFFFFFFF, self.x[1] & 0xFFFFFFFF
                if w == 0 or h == 0 or w > 1024 or h > 1024:
                    self.x[0] = MASK; return               # -1
                hnd = self.sig_img; self.sig_img += 1
                self.imgs[hnd] = [w, h, bytearray(w * h * 4)]
                self.x[0] = hnd; return
            if num == 88:                                  # FreeImage
                self.imgs.pop(a0, None); self.x[0] = 0; return
            if num == 51:                                  # ImageSize -> (ancho << 32) | alto
                im = self.imgs.get(a0)
                self.x[0] = 0 if im is None else ((im[0] << 32) | im[1]); return

            # Las cuatro de fila. El 'buffer' es imagen+1 (convencion de
            # ImageBuffer); el 0 seria la ventana, que no se admite.
            b = sx(a0, 64)
            im = self.imgs.get(b - 1) if b > 0 else None
            if im is None:
                self.x[0] = MASK; return                   # -1
            W, H, px = im
            x = sx(self.x[1], 64)
            y = sx(self.x[2], 64)

            if num == 292:                                 # SYS_FILA_TIRA
                mx = sx(self.x[3], 64)
                color = self.x[4] & 0xFFFFFF
                distinto = (self.x[5] != 0)
                if y < 0 or y >= H or x < 0 or x >= W:
                    self.x[0] = 0; return
                cuantos = -mx if mx < 0 else mx
                paso = -1 if mx < 0 else 1
                n, cx = 0, x
                while n < cuantos and 0 <= cx < W:
                    o = (y * W + cx) * 4
                    c = (px[o] << 16) | (px[o + 1] << 8) | px[o + 2]
                    if (c == color) == distinto:
                        break
                    n += 1; cx += paso
                self.x[0] = n; return

            # LEER / ESCRIBIR / RELLENAR: el mismo recorte que fila_recortar()
            nn = sx(self.x[3], 64)
            if nn <= 0 or y < 0 or y >= H:
                self.x[0] = 0; return
            ini, fin, salto = x, x + nn, 0
            if ini < 0:
                salto = -ini; ini = 0
            if fin > W:
                fin = W
            if ini >= fin:
                self.x[0] = 0; return
            cuantos = fin - ini

            if num == 291:                                 # SYS_FILA_RELLENAR
                rgb = self.x[4] & 0xFFFFFF
                r, g, bb = (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF
                for i in range(cuantos):
                    o = (y * W + ini + i) * 4
                    px[o], px[o + 1], px[o + 2], px[o + 3] = r, g, bb, 255
                self.x[0] = cuantos; return

            # bytes por pixel en el buffer DEL PROGRAMA: 4 (o 0) para Lua,
            # 8 para un array de Nemo Basic. Ver fila_paso() en syscall.c.
            pb = self.x[5]
            pb = 4 if pb in (0, 4) else (8 if pb == 8 else 0)
            if pb == 0:
                self.x[0] = MASK; return
            dir0 = (self.x[4] + salto * pb) & MASK

            if num == 289:                                 # SYS_FILA_LEER
                for i in range(cuantos):
                    o = (y * W + ini + i) * 4
                    v = (px[o] << 16) | (px[o + 1] << 8) | px[o + 2]
                    self.escribir(dir0 + i * pb, pb, v)
                self.x[0] = cuantos; return

            if num == 290:                                 # SYS_FILA_ESCRIBIR
                for i in range(cuantos):
                    v = self.leer(dir0 + i * pb, pb) & 0xFFFFFF
                    o = (y * W + ini + i) * 4
                    px[o] = (v >> 16) & 0xFF
                    px[o + 1] = (v >> 8) & 0xFF
                    px[o + 2] = v & 0xFF
                    px[o + 3] = 255
                self.x[0] = cuantos; return

        r = self.fs_syscall(num, a0, self.x[1], self.x[2])
        if r is not None:
            self.x[0] = r & MASK
            return
        self.x[0] = 0                     # el resto: sin efecto, devuelven 0

    # ---- ejecutar ----
    def paso(self):
        if self.pc == FIN_LIMPIO:
            self.fin = True
            raise Parada("fin")
        ins = self.leer(self.pc, 4)
        pc = self.pc
        self.pc += 4
        self.pasos += 1
        op0 = (ins >> 25) & 0xF
        if ins == 0xD503201F:                                   # nop
            return
        if (ins & 0xFFE0001F) == 0xD4000001:                    # svc
            self.svc(); return
        # --- saltos ---
        if (ins & 0x7C000000) == 0x14000000:                    # b / bl
            off = sx(ins & 0x3FFFFFF, 26) * 4
            if ins & 0x80000000:
                self.x[30] = self.pc
            self.pc = (pc + off) & MASK; return
        if (ins & 0xFF000010) == 0x54000000:                    # b.cond
            if self.cond(ins & 0xF):
                self.pc = (pc + sx((ins >> 5) & 0x7FFFF, 19) * 4) & MASK
            return
        if (ins & 0x7E000000) == 0x34000000:                    # cbz / cbnz
            bits = 64 if ins >> 31 else 32
            v = self.r(ins & 31) & ((1 << bits) - 1)
            nz = (ins >> 24) & 1
            if (v == 0) != bool(nz):
                self.pc = (pc + sx((ins >> 5) & 0x7FFFF, 19) * 4) & MASK
            return
        if (ins & 0x7E000000) == 0x36000000:                    # tbz / tbnz
            b = ((ins >> 31) << 5) | ((ins >> 19) & 31)
            bit = (self.r(ins & 31) >> b) & 1
            if bit == ((ins >> 24) & 1):
                self.pc = (pc + sx((ins >> 5) & 0x3FFF, 14) * 4) & MASK
            return
        if (ins & 0xFFFFFC1F) == 0xD65F0000:                    # ret
            self.pc = self.r((ins >> 5) & 31); return
        if (ins & 0xFFFFFC1F) == 0xD61F0000:                    # br
            self.pc = self.r((ins >> 5) & 31); return
        if (ins & 0xFFFFFC1F) == 0xD63F0000:                    # blr
            dest = self.r((ins >> 5) & 31); self.x[30] = self.pc; self.pc = dest; return
        # --- adr / adrp ---
        if (ins & 0x1F000000) == 0x10000000:
            imm = sx(((ins >> 5) & 0x7FFFF) << 2 | ((ins >> 29) & 3), 21)
            if ins >> 31:
                self.w(ins & 31, (pc & ~0xFFF) + (imm << 12))
            else:
                self.w(ins & 31, pc + imm)
            return
        sf = ins >> 31
        bits = 64 if sf else 32
        rd, rn = ins & 31, (ins >> 5) & 31
        # --- suma / resta con inmediato ---
        if (ins & 0x1F000000) == 0x11000000:
            op, s = (ins >> 30) & 1, (ins >> 29) & 1
            imm = (ins >> 10) & 0xFFF
            if (ins >> 22) & 1: imm <<= 12
            a = self.r(rn, sp=True)
            if op: res = self.flags_suma(a, ~imm, 1, bits) if s else (a - imm)
            else: res = self.flags_suma(a, imm, 0, bits) if s else (a + imm)
            self.w(rd, res, sp=not s, bits=bits); return
        # --- logicas con inmediato ---
        if (ins & 0x1F800000) == 0x12000000:
            opc = (ins >> 29) & 3
            imm = decode_bitmask((ins >> 22) & 1, (ins >> 10) & 0x3F, (ins >> 16) & 0x3F, bits)
            a = self.r(rn)
            res = [a & imm, a | imm, a ^ imm, a & imm][opc] & ((1 << bits) - 1)
            if opc == 3:
                self.n = res >> (bits - 1); self.z = int(res == 0); self.c = self.v = 0
            self.w(rd, res, sp=(opc != 3), bits=bits); return
        # --- movz / movn / movk ---
        if (ins & 0x1F800000) == 0x12800000:
            opc = (ins >> 29) & 3
            hw = (ins >> 21) & 3
            imm = ((ins >> 5) & 0xFFFF) << (16 * hw)
            if opc == 0: self.w(rd, ~imm, bits=bits)
            elif opc == 2: self.w(rd, imm, bits=bits)
            elif opc == 3:
                v = self.r(rd) & ~(0xFFFF << (16 * hw))
                self.w(rd, v | imm, bits=bits)
            else: raise Parada("mov desconocido 0x%08x" % ins)
            return
        # --- campos de bits (lsl/lsr/asr/ubfx/sbfx/uxtb...) ---
        if (ins & 0x1F800000) == 0x13000000:
            opc = (ins >> 29) & 3
            immr, imms = (ins >> 16) & 0x3F, (ins >> 10) & 0x3F
            src = self.r(rn) & ((1 << bits) - 1)
            wmask = decode_bitmask((ins >> 22) & 1, imms, immr, bits) if True else 0
            # implementacion directa de UBFM/SBFM/BFM
            if imms >= immr:
                ancho = imms - immr + 1
                campo = (src >> immr) & ((1 << ancho) - 1)
                if opc == 0: res = sx(campo, ancho) & ((1 << bits) - 1)
                elif opc == 2: res = campo
                else:
                    dst = self.r(rd); res = (dst & ~((1 << ancho) - 1)) | campo
            else:
                ancho = imms + 1
                campo = src & ((1 << ancho) - 1)
                sh = bits - immr
                if opc == 0: res = (sx(campo, ancho) << sh) & ((1 << bits) - 1)
                elif opc == 2: res = (campo << sh) & ((1 << bits) - 1)
                else:
                    dst = self.r(rd); m = ((1 << ancho) - 1) << sh; res = (dst & ~m) | (campo << sh)
            self.w(rd, res, bits=bits); return
        # --- logicas con registro ---
        if (ins & 0x1F000000) == 0x0A000000:
            opc = (ins >> 29) & 3
            nbit = (ins >> 21) & 1
            b = self.shift(self.r((ins >> 16) & 31), (ins >> 22) & 3, (ins >> 10) & 0x3F, bits)
            if nbit: b = ~b & ((1 << bits) - 1)
            a = self.r(rn)
            res = [a & b, a | b, a ^ b, a & b][opc] & ((1 << bits) - 1)
            if opc == 3:
                self.n = res >> (bits - 1); self.z = int(res == 0); self.c = self.v = 0
            self.w(rd, res, bits=bits); return
        # --- suma / resta con registro desplazado ---
        if (ins & 0x1F200000) == 0x0B000000:
            op, s = (ins >> 30) & 1, (ins >> 29) & 1
            b = self.shift(self.r((ins >> 16) & 31), (ins >> 22) & 3, (ins >> 10) & 0x3F, bits)
            a = self.r(rn)
            if op: res = self.flags_suma(a, ~b, 1, bits) if s else (a - b)
            else: res = self.flags_suma(a, b, 0, bits) if s else (a + b)
            self.w(rd, res, bits=bits); return
        # --- suma / resta con registro extendido (uxtw, sxtw... y con sp) ---
        if (ins & 0x1F200000) == 0x0B200000:
            op, s = (ins >> 30) & 1, (ins >> 29) & 1
            opt, amt = (ins >> 13) & 7, (ins >> 10) & 7
            v = self.r((ins >> 16) & 31)
            anchos = [8, 16, 32, 64, 8, 16, 32, 64]
            v &= (1 << anchos[opt]) - 1
            if opt >= 4: v = sx(v, anchos[opt])
            b = (v << amt) & MASK
            a = self.r(rn, sp=True)
            if op: res = self.flags_suma(a, ~b, 1, bits) if s else (a - b)
            else: res = self.flags_suma(a, b, 0, bits) if s else (a + b)
            self.w(rd, res, sp=not s, bits=bits); return
        # --- ccmp / ccmn: comparar solo si se cumple la condicion ---
        if (ins & 0x3FE00010) == 0x3A400000:
            op = (ins >> 30) & 1
            c = (ins >> 12) & 0xF
            if self.cond(c):
                a = self.r(rn)
                b = ((ins >> 16) & 31) if (ins >> 11) & 1 else self.r((ins >> 16) & 31)
                if op: self.flags_suma(a, ~b, 1, bits)
                else: self.flags_suma(a, b, 0, bits)
            else:
                f = ins & 0xF
                self.n, self.z, self.c, self.v = (f >> 3) & 1, (f >> 2) & 1, (f >> 1) & 1, f & 1
            return
                # --- seleccion condicional ---
        if (ins & 0x1FE00000) == 0x1A800000:
            op, o2 = (ins >> 30) & 1, (ins >> 10) & 1
            c = (ins >> 12) & 0xF
            a, b = self.r(rn), self.r((ins >> 16) & 31)
            if self.cond(c): res = a
            else:
                if not op and not o2: res = b
                elif not op and o2: res = b + 1
                elif op and not o2: res = ~b
                else: res = -b
            self.w(rd, res, bits=bits); return
        # --- dos fuentes: division y desplazamientos variables ---
        if (ins & 0x5FE00000) == 0x1AC00000:
            opc = (ins >> 10) & 0x3F
            a, b = self.r(rn) & ((1 << bits) - 1), self.r((ins >> 16) & 31) & ((1 << bits) - 1)
            if opc == 2: res = 0 if b == 0 else a // b
            elif opc == 3:
                sa, sb = sx(a, bits), sx(b, bits)
                res = 0 if sb == 0 else int(abs(sa) // abs(sb)) * (1 if (sa < 0) == (sb < 0) else -1)
            elif opc in (8, 9, 10, 11): res = self.shift(a, opc - 8, b % bits, bits)
            else: raise Parada("dos fuentes desconocida 0x%08x" % ins)
            self.w(rd, res, bits=bits); return
        # --- umulh / smulh: la mitad alta del producto de 128 bits ---
        if (ins & 0xFF800000) == 0x9B800000 or (ins & 0xFF800000) == 0x9B000000 and ((ins >> 21) & 7) == 2:
            o = (ins >> 21) & 7
            if o in (6, 2):
                a, b = self.r(rn), self.r((ins >> 16) & 31)
                if o == 2: a, b = sx(a, 64), sx(b, 64)
                self.w(rd, (a * b) >> 64); return
                # --- tres fuentes: madd / msub ---
        if (ins & 0x1F000000) == 0x1B000000 and ((ins >> 21) & 7) == 0:
            ra = (ins >> 10) & 31
            prod = self.r(rn) * self.r((ins >> 16) & 31)
            res = self.r(ra) - prod if (ins >> 15) & 1 else self.r(ra) + prod
            self.w(rd, res, bits=bits); return
        # --- ushr dd, dn, #n: desplazar a la derecha los BITS de un
        # registro escalar de 64 (no es coma flotante, por eso no entra en
        # fp()). La usa nb_rnd_float para quedarse con la mantisa de un
        # numero al azar: sin esto, Rnd() sin argumento no se podia ejecutar
        # aqui, y era lo unico que le faltaba.
        #
        # 011111110 immh immb 00000 1 Rn Rd, con el desplazamiento guardado
        # como 128 - (immh:immb).
        if (ins & 0xFF80FC00) == 0x7F000400 and ((ins >> 16) & 0x40):
            self.d[rd] = (self.d[rn] & MASK) >> (128 - ((ins >> 16) & 0x7F))
            return
        # --- movi dd, #imm: poner un valor fijo en un registro escalar.
        # En la practica, poner un doble a cero, que es como empieza
        # nb_str_to_float. op=1 y cmode=1110: cada BIT del inmediato de ocho
        # se convierte en un byte entero, 0x00 o 0xFF.
        # 0 Q 1 0111100000 abc 1110 0 1 defgh Rd
        if (ins & 0xBFF8FC00) == 0x2F00E400:
            imm8 = (((ins >> 16) & 7) << 5) | ((ins >> 5) & 0x1F)
            v = 0
            for k in range(8):
                if imm8 & (1 << k): v |= 0xFF << (k * 8)
            self.d[ins & 31] = v
            return

        # --- scvtf/ucvtf dd, dn: los BITS de dn como entero, a doble.
        # Van juntas con la ushr de arriba en nb_rnd_float (bits al azar ->
        # entero -> doble). 010 11110 011 00001 11011 0 Rn Rd, y con el bit
        # 29 a uno es la version sin signo.
        if (ins & 0xFFFFFC00) in (0x5E61D800, 0x7E61D800):
            crudo = self.d[rn] & MASK
            if ins & (1 << 29):
                self.d[rd] = self.f_a_bits(float(crudo))          # ucvtf: sin signo
            else:
                self.d[rd] = self.f_a_bits(float(sx(crudo, 64)))  # scvtf: con signo
            return

        # --- coma flotante (doble precision) ---
        if (ins & 0x5E000000) == 0x1E000000:                     # coma flotante
            if self.fp(ins): return
            raise Parada("instruccion de coma flotante desconocida 0x%08x en el desplazamiento 0x%x" % (ins, pc - BASE))
        # --- cargas y almacenamientos ---
        if (ins & 0x0A000000) == 0x08000000:
            return self.carga(ins, pc)
        raise Parada("instruccion desconocida 0x%08x en el desplazamiento 0x%x" % (ins, pc - BASE))

    def bits_a_f(self, b):
        return struct.unpack("<d", (b & MASK).to_bytes(8, "little"))[0]
    def f_a_bits(self, v):
        return int.from_bytes(struct.pack("<d", v), "little")
    def fp(self, ins):
        rd, rn, rm = ins & 31, (ins >> 5) & 31, (ins >> 16) & 31
        op2 = ins & 0xFFE0FC00        # dos operandos (rm en los bits 16-20)
        op = ins & 0xFFFFFC00         # uno solo
        a = self.bits_a_f(self.d[rn]); b = self.bits_a_f(self.d[rm])
        if (ins & 0xFFE01FE0) == 0x1E601000:      # fmov dd, #valor (el numero va dentro de la instruccion)
            i8 = (ins >> 13) & 0xFF
            signo, b6 = i8 >> 7, (i8 >> 6) & 1
            exp = ((1 - b6) << 10) | ((0x3FF if b6 else 0) & 0x3FC) | (((0xFF if b6 else 0) & 0xFF) << 2 & 0x3FC) | ((i8 >> 4) & 3)
            exp = ((1 - b6) << 10) | ((0xFF if b6 else 0) << 2) | ((i8 >> 4) & 3)
            frac = (i8 & 0xF) << 48
            self.d[rd] = (signo << 63) | (exp << 52) | frac
            return True
        if op == 0x9E660000: self.w(rd, self.d[rn]); return True                  # fmov xd, dn
        if op == 0x9E670000: self.d[rd] = self.r(rn); return True                 # fmov dd, xn
        if op == 0x9E620000: self.d[rd] = self.f_a_bits(float(sx(self.r(rn), 64))); return True    # scvtf dd, xn
        # scvtf dd, wn -- la misma, pero leyendo solo los 32 bits bajos. La
        # emite nb_str_to_float al convertir el signo y cada cifra.
        if op == 0x1E620000: self.d[rd] = self.f_a_bits(float(sx(self.r(rn) & 0xFFFFFFFF, 32))); return True
        if op == 0x9E780000:                                                       # fcvtzs xd, dn
            self.w(rd, int(a) & MASK); return True
        if op2 == 0x1E601800: self.d[rd] = self.f_a_bits(a / b if b != 0 else math.inf if a > 0 else (-math.inf if a < 0 else math.nan)); return True
        if op2 == 0x1E600800: self.d[rd] = self.f_a_bits(a * b); return True
        if op2 == 0x1E602800: self.d[rd] = self.f_a_bits(a + b); return True
        if op2 == 0x1E603800: self.d[rd] = self.f_a_bits(a - b); return True
        if op == 0x1E614000: self.d[rd] = self.f_a_bits(-a); return True           # fneg
        if op == 0x1E60C000: self.d[rd] = self.f_a_bits(abs(a)); return True       # fabs
        if op == 0x1E61C000: self.d[rd] = self.f_a_bits(math.sqrt(a) if a >= 0 else math.nan); return True
        if op == 0x1E654000: self.d[rd] = self.f_a_bits(math.floor(a)); return True
        if op == 0x1E64C000: self.d[rd] = self.f_a_bits(math.ceil(a)); return True
        if (ins & 0xFFC00000) == 0x1F400000:   # fmadd / fmsub / fnmadd / fnmsub (doble)
            ra = self.bits_a_f(self.d[(ins >> 10) & 31])
            prod = a * b
            o1, o0 = (ins >> 21) & 1, (ins >> 15) & 1
            if not o1 and not o0: r = ra + prod
            elif not o1 and o0: r = ra - prod
            elif o1 and not o0: r = -ra - prod
            else: r = -ra + prod
            self.d[rd] = self.f_a_bits(r)
            return True
        if (ins & 0xFFE00C00) == 0x1E600C00:   # fcsel: uno u otro, segun la condicion
            self.d[rd] = self.d[rn] if self.cond((ins >> 12) & 0xF) else self.d[rm]
            return True
        if (ins & 0xFFE00C10) == 0x1E600400:   # fccmp / fccmpe: comparar solo si se cumple la condicion
            if self.cond((ins >> 12) & 0xF):
                if a != a or b != b: self.n, self.z, self.c, self.v = 0, 0, 1, 1
                elif a == b: self.n, self.z, self.c, self.v = 0, 1, 1, 0
                elif a < b: self.n, self.z, self.c, self.v = 1, 0, 0, 0
                else: self.n, self.z, self.c, self.v = 0, 0, 1, 0
            else:
                fl = ins & 0xF
                self.n, self.z, self.c, self.v = (fl >> 3) & 1, (fl >> 2) & 1, (fl >> 1) & 1, fl & 1
            return True
        if (ins & 0xFFE0FC07) == 0x1E602000:   # fcmp / fcmpe, con registro o contra cero
            if ins & 8: b = 0.0
            if a != a or b != b: self.n, self.z, self.c, self.v = 0, 0, 1, 1
            elif a == b: self.n, self.z, self.c, self.v = 0, 1, 1, 0
            elif a < b: self.n, self.z, self.c, self.v = 1, 0, 0, 0
            else: self.n, self.z, self.c, self.v = 0, 0, 1, 0
            return True
        if (ins & 0xFFFFFC00) == 0x1E22C000: self.d[rd] = self.f_a_bits(struct.unpack("<f", struct.pack("<I", self.d[rn] & 0xFFFFFFFF))[0]); return True  # fcvt d, s
        if (ins & 0xFFFFFC00) == 0x1E624000:                                        # fcvt s, d
            self.d[rd] = int.from_bytes(struct.pack("<f", a), "little"); return True
        return False

    def carga(self, ins, pc):
        # pares: ldp / stp
        if (ins & 0x3A000000) == 0x28000000:
            opc = ins >> 30
            l = (ins >> 22) & 1
            modo = (ins >> 23) & 3
            tam = 8 if opc == 2 else 4
            imm = sx((ins >> 15) & 0x7F, 7) * tam
            rt, rt2, rn = ins & 31, (ins >> 10) & 31, (ins >> 5) & 31
            base = self.r(rn, sp=True)
            dir = base + imm if modo in (2, 3) else base
            if l:
                self.w(rt, self.leer(dir, tam)); self.w(rt2, self.leer(dir + tam, tam))
            else:
                self.escribir(dir, tam, self.r(rt)); self.escribir(dir + tam, tam, self.r(rt2))
            if modo in (1, 3): self.w(rn, base + imm, sp=True)
            return
        # un registro: ldr/str (b, h, w, x), con sus modos
        if (ins & 0x3B000000) == 0x39000000 or (ins & 0x3B200000) == 0x38000000 or (ins & 0x3B200C00) == 0x38200800:
            size = ins >> 30
            opc = (ins >> 22) & 3
            v = (ins >> 26) & 1
            if v:                                        # ldr/str de un registro de coma flotante
                tam = 8 if size == 3 else 4
                rt, rn = ins & 31, (ins >> 5) & 31
                base = self.r(rn, sp=True)
                if (ins & 0x3B000000) == 0x39000000: dir = base + ((ins >> 10) & 0xFFF) * tam
                else:
                    imm = sx((ins >> 12) & 0x1FF, 9); modo = (ins >> 10) & 3
                    dir = base + imm if modo in (0, 3) else base
                    if modo in (1, 3): self.w(rn, base + (imm if modo == 1 else 0) + (imm if modo == 3 else 0) - (imm if modo == 3 else 0), sp=True)
                if opc & 1: self.d[rt] = self.leer(dir, tam)
                else: self.escribir(dir, tam, self.d[rt])
                return
            tam = 1 << size
            rt, rn = ins & 31, (ins >> 5) & 31
            base = self.r(rn, sp=True)
            escribe_base = None
            if (ins & 0x3B000000) == 0x39000000:              # desplazamiento sin signo
                dir = base + ((ins >> 10) & 0xFFF) * tam
            elif (ins & 0x3B200C00) == 0x38200800:            # registro
                opt, s = (ins >> 13) & 7, (ins >> 12) & 1
                m = self.r((ins >> 16) & 31)
                if opt == 2: m &= 0xFFFFFFFF
                elif opt == 6: m = sx(m, 32)
                dir = base + (m << (size if s else 0))
            else:
                imm = sx((ins >> 12) & 0x1FF, 9)
                modo = (ins >> 10) & 3
                if modo == 0: dir = base + imm                  # sin escalar (ldur/stur)
                elif modo == 1: dir = base; escribe_base = base + imm        # post
                elif modo == 3: dir = base + imm; escribe_base = dir         # pre
                else: raise Parada("carga desconocida 0x%08x" % ins)
            dir &= MASK
            if opc == 0: self.escribir(dir, tam, self.r(rt))
            elif opc == 1: self.w(rt, self.leer(dir, tam))
            else:                                              # con signo
                val = sx(self.leer(dir, tam), 8 * tam)
                self.w(rt, val, bits=(64 if opc == 2 else 32))
            if escribe_base is not None: self.w(rn, escribe_base, sp=True)
            return
        raise Parada("carga o almacenamiento desconocido 0x%08x en el desplazamiento 0x%x" % (ins, pc - BASE))

    def ejecutar(self):
        try:
            while self.pasos < self.max_pasos:
                self.paso()
            raise Parada("demasiados pasos (%d): ¿un bucle sin fin?" % self.pasos)
        except Parada as p:
            return str(p)

def ejecutar(ruta, max_pasos=5_000_000):
    cpu = Cpu(open(ruta, "rb").read(), max_pasos=max_pasos)
    motivo = cpu.ejecutar()
    return cpu, motivo

if __name__ == "__main__":
    cpu, motivo = ejecutar(sys.argv[1])
    sys.stdout.write("".join(cpu.salida))
    for r in cpu.registro: print("[registro] " + r)
    if motivo != "fin": print("[emulador] " + motivo)
