-- Particionador -- el reparto de la tarjeta, de un vistazo.
--
-- De momento SOLO MIRA: enseña lo que ocupa cada cosa y cuanto espacio
-- libre queda sin usar. La particion de arranque se ve, se mide y NO se
-- toca: es la que hace que la Pi encienda.
--
-- Los numeros salen del propio sistema (llamadas 279-282), no de suponer
-- nada: capacidad real de la tarjeta, tabla de particiones, donde vive
-- NemoFS y cuanto tiene ocupado.

local gui = require("nemo_gui")

local SYS_DISK_FISICO, SYS_DISK_PARTICION = 279, 280
local SYS_DISK_NEMOFS, SYS_NEMOFS_USO, SYS_DISK_ESTIRAR = 281, 282, 283
local SECTOR = 512

-- ---------------------------------------------------------------------
-- Cuentas (sin pantalla: se pueden probar solas)
-- ---------------------------------------------------------------------
local M = {}

function M.tamano(sectores)
  local bytes = sectores * SECTOR
  if bytes >= 1024 * 1024 * 1024 then
    return string.format("%.1f GB", bytes / (1024 * 1024 * 1024))
  elseif bytes >= 1024 * 1024 then
    return string.format("%d MB", math.floor(bytes / (1024 * 1024) + 0.5))
  end
  return string.format("%d KB", math.floor(bytes / 1024 + 0.5))
end

function M.es_fat(tipo)
  return tipo == 0x01 or tipo == 0x04 or tipo == 0x06
      or tipo == 0x0B or tipo == 0x0C or tipo == 0x0E
end

-- Los tramos del disco, en orden y sin huecos: cada uno con su inicio,
-- su tamaño, su nombre y si se puede tocar.
function M.tramos(capacidad, particiones, nfs, uso)
  local t = {}
  local fin_anterior = 0
  for _, p in ipairs(particiones) do
    if p.inicio > fin_anterior then
      t[#t + 1] = { clase = "reservado", inicio = fin_anterior,
                    sectores = p.inicio - fin_anterior, nombre = "Reservado", tocable = false }
    end
    if p.inicio == nfs.inicio and nfs.sectores > 0 then
      local usados = math.min(uso.usados, p.sectores)
      t[#t + 1] = { clase = "nemofs_usado", inicio = p.inicio, sectores = usados,
                    nombre = "NemoFS (ocupado)", tocable = false }
      t[#t + 1] = { clase = "nemofs_libre", inicio = p.inicio + usados,
                    sectores = p.sectores - usados, nombre = "NemoFS (libre)", tocable = false }
    else
      t[#t + 1] = { clase = M.es_fat(p.tipo) and "arranque" or "otra", inicio = p.inicio,
                    sectores = p.sectores,
                    nombre = M.es_fat(p.tipo) and "Arranque (FAT)" or "Otra particion",
                    tocable = false }
    end
    fin_anterior = p.inicio + p.sectores
  end
  if capacidad > fin_anterior then
    t[#t + 1] = { clase = "sin_usar", inicio = fin_anterior,
                  sectores = capacidad - fin_anterior, nombre = "Sin usar", tocable = true }
  end
  return t
end

-- Cuanto se ganaria al estirar NemoFS hasta el final de la tarjeta
-- Que ha pasado al intentar estirar, en palabras
M.MOTIVO = {
  [0] = "Hecho: NemoFS ocupa ya todo lo que puede de la tarjeta.",
  [1] = "No hay espacio libre que aprovechar.",
  [2] = "La tabla de particiones no es fiable: no se toca nada.",
  [3] = "Hay otra particion detras de NemoFS: no se toca nada.",
  [4] = "No se pudo leer o escribir en la tarjeta.",
  [5] = "NemoFS ya esta en su tamaño maximo (16 GB). El mapa de bloques libres\n" ..
        "vive en la memoria del kernel, y dejarlo crecer mas se comeria la RAM\n" ..
        "que necesitan los programas. El resto de la tarjeta queda sin usar.",
}

function M.se_puede_ganar(capacidad, particiones, nfs)
  if nfs.sectores == 0 then return 0 end
  local fin_nfs = nfs.inicio + nfs.sectores
  for _, p in ipairs(particiones) do
    if p.inicio >= fin_nfs then return 0 end     -- hay algo detras: no se toca
  end
  return capacidad > fin_nfs and (capacidad - fin_nfs) or 0
end

-- Reparte el ancho de la barra entre los tramos, dando a cada uno al
-- menos 3 pixeles para que los pequeños no desaparezcan
function M.anchos(tramos, capacidad, ancho_total)
  local w, resto = {}, ancho_total
  for i, tr in ipairs(tramos) do
    w[i] = math.max(3, math.floor(ancho_total * tr.sectores / capacidad))
    resto = resto - w[i]
  end
  local i = 1                                    -- lo que sobre o falte, al tramo mas grande
  for k = 2, #w do if w[k] > w[i] then i = k end end
  w[i] = math.max(3, w[i] + resto)
  return w
end

if not nemo then return M end                    -- si se carga para probar, solo las cuentas

-- ---------------------------------------------------------------------
-- La ventana
-- ---------------------------------------------------------------------
local COLOR = {
  fondo        = gui.rgb(214, 212, 206),
  arranque     = gui.rgb(150, 154, 166),
  nemofs_usado = gui.rgb(70, 130, 210),
  nemofs_libre = gui.rgb(150, 195, 245),
  sin_usar     = gui.rgb(230, 228, 222),
  otra         = gui.rgb(190, 150, 90),
  reservado    = gui.rgb(120, 124, 134),
  borde        = gui.rgb(60, 62, 70),
  texto        = gui.rgb(30, 32, 40),
  suave        = gui.rgb(90, 94, 104),
}

local function leer_sistema()
  local capacidad = nemo.syscall(SYS_DISK_FISICO)
  local particiones = {}
  for i = 0, 3 do
    local v = nemo.syscall(SYS_DISK_PARTICION, i)
    if v ~= 0 then
      particiones[#particiones + 1] = {
        tipo = (v >> 56) & 0xFF,
        inicio = (v >> 28) & 0xFFFFFFF,
        sectores = v & 0xFFFFFFF,
      }
    end
  end
  table.sort(particiones, function(a, b) return a.inicio < b.inicio end)
  local v = nemo.syscall(SYS_DISK_NEMOFS)
  local nfs = { de_tabla = (v >> 63) & 1 == 1, inicio = (v >> 28) & 0xFFFFFFF, sectores = v & 0xFFFFFFF }
  local buf = nemo.buffer(8)
  local uso = { total = 0, usados = 0 }
  -- OJO: a syscall se le pasa la DIRECCION del buffer, no el buffer
  if nemo.syscall(SYS_NEMOFS_USO, nemo.direccion(buf)) == 1 then
    uso.total = nemo.leer_i32(buf, 0)
    uso.usados = nemo.leer_i32(buf, 4)
  end
  return capacidad, particiones, nfs, uso
end

local capacidad, particiones, nfs, uso = leer_sistema()
-- Cuantos sectores ocupa un bloque de NemoFS. NO se supone: se deduce de lo
-- que el propio sistema dice que tiene la particion. (estaba
-- puesto a 8 a ojo, y como en realidad es 1, lo ocupado salia ocho veces
-- mayor: en QEMU la tarjeta entera aparecia llena.)
local por_bloque = 1
if uso.total > 0 and nfs.sectores > 0 then
  por_bloque = math.max(1, math.floor(nfs.sectores / uso.total))
end
local uso_sectores = { usados = uso.usados * por_bloque, total = uso.total * por_bloque }
local hay_datos = uso.total > 0
local tramos = M.tramos(capacidad, particiones, nfs, uso_sectores)
local ganable = M.se_puede_ganar(capacidad, particiones, nfs)

gui.crear_ventana("Particionador", 90, 70, 560, 380)
gui.botones_ventana(false, true, true)

local BARRA_X, BARRA_Y, BARRA_W, BARRA_H = 20, 60, 520, 56

local boton, boton_si, boton_no
local preguntando = false
local mensaje = nil

local function refrescar_datos()
  capacidad, particiones, nfs, uso = leer_sistema()
  local pb = 1
  if uso.total > 0 and nfs.sectores > 0 then pb = math.max(1, math.floor(nfs.sectores / uso.total)) end
  uso_sectores = { usados = uso.usados * pb, total = uso.total * pb }
  tramos = M.tramos(capacidad, particiones, nfs, uso_sectores)
  ganable = M.se_puede_ganar(capacidad, particiones, nfs)
end

local function dibujar()
  gui.rect(0, 0, 560, 380, COLOR.fondo)
  gui.texto(20, 20, "Tarjeta de " .. M.tamano(capacidad), COLOR.texto)
  gui.texto(20, 38, nfs.de_tabla and "Reparto leido de la tabla de particiones"
                                  or "Reparto de siempre (sin tabla utilizable)", COLOR.suave)

  local anchos = M.anchos(tramos, capacidad, BARRA_W)
  local x = BARRA_X
  for i, tr in ipairs(tramos) do
    gui.rect(x, BARRA_Y, anchos[i], BARRA_H, COLOR[tr.clase] or COLOR.otra)
    gui.rect(x, BARRA_Y, anchos[i], 1, COLOR.borde)
    gui.rect(x, BARRA_Y + BARRA_H - 1, anchos[i], 1, COLOR.borde)
    gui.rect(x, BARRA_Y, 1, BARRA_H, COLOR.borde)
    if tr.clase == "arranque" and anchos[i] > 22 then         -- el candado
      local cx, cy = x + anchos[i] // 2 - 5, BARRA_Y + BARRA_H // 2 - 7
      gui.rect(cx, cy + 5, 10, 8, COLOR.texto)
      gui.rect(cx + 2, cy, 6, 2, COLOR.texto)
      gui.rect(cx + 2, cy, 2, 6, COLOR.texto)
      gui.rect(cx + 6, cy, 2, 6, COLOR.texto)
    end
    x = x + anchos[i]
  end
  gui.rect(BARRA_X + BARRA_W - 1, BARRA_Y, 1, BARRA_H, COLOR.borde)

  if not hay_datos then
    gui.texto(300, 38, "(sin datos de uso de NemoFS)", COLOR.suave)
  end

  local y = BARRA_Y + BARRA_H + 22
  for i, tr in ipairs(tramos) do
    gui.rect(20, y + 3, 12, 12, COLOR[tr.clase] or COLOR.otra)
    gui.rect(20, y + 3, 12, 1, COLOR.borde)
    gui.texto(40, y, tr.nombre, COLOR.texto)
    gui.texto(230, y, M.tamano(tr.sectores), COLOR.texto)
    if tr.clase == "arranque" then
      gui.texto(320, y, "fija: no se toca", COLOR.suave)
    elseif tr.clase == "sin_usar" then
      gui.texto(320, y, "se puede aprovechar", COLOR.suave)
    end
    y = y + 20
  end

  y = y + 10
  if mensaje then
    gui.texto(20, y, mensaje, COLOR.texto)
  elseif preguntando then
    gui.texto(20, y, "Se ampliara NemoFS " .. M.tamano(ganable) .. " (hasta el final de la tarjeta).", COLOR.texto)
    gui.texto(20, y + 20, "La particion de arranque no se toca. No quites la tarjeta.", COLOR.suave)
  elseif ganable > 0 then
    gui.texto(20, y, "NemoFS puede crecer " .. M.tamano(ganable) .. " mas.", COLOR.texto)
  else
    gui.texto(20, y, "La tarjeta esta aprovechada del todo.", COLOR.texto)
  end
end

-- El boton solo existe si hay algo que ganar; y al pulsarlo hay que
-- confirmar, con las cifras delante.
local function poner_botones()
  if boton then gui.liberar_gadget(boton); boton = nil end
  if boton_si then gui.liberar_gadget(boton_si); boton_si = nil end
  if boton_no then gui.liberar_gadget(boton_no); boton_no = nil end
  if mensaje then return end
  if preguntando then
    boton_si = gui.crear_boton("Si, ampliar", 20, 330, 130, 28)
    boton_no = gui.crear_boton("Cancelar", 160, 330, 110, 28)
  elseif ganable > 0 then
    boton = gui.crear_boton("Usar todo el espacio libre", 20, 330, 240, 28)
  end
end

dibujar()
poner_botones()

gui.bucle(function(ev, fuente)
  if ev == gui.EVENT_GADGETACTION then
    if boton and fuente == boton then
      preguntando = true
    elseif boton_no and fuente == boton_no then
      preguntando = false
    elseif boton_si and fuente == boton_si then
      local r = nemo.syscall(SYS_DISK_ESTIRAR)
      preguntando = false
      mensaje = M.MOTIVO[r] or ("No se pudo ampliar (codigo " .. tostring(r) .. ")")
      if r == 0 then refrescar_datos() end
    else
      return
    end
    poner_botones()
    dibujar()
  elseif gui.tamano_cambiado() or ev == gui.EVENT_WINDOWSIZE then
    dibujar()
  end
end)
