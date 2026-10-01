-- prueba_lectura.lua -- pone a prueba la lectura POR PARTES de archivos
-- grandes (fase 5 de la memoria): SYS_FILE_READ_AT sobre el volumen FAT
-- (en la Pi, la particion de arranque, la que ve el Mac).
--
-- El archivo de prueba es PRUEBA.BIN en la raiz del volumen FAT. Sigue un
-- patron que se puede comprobar en cualquier posicion sin guardar una
-- copia: bloques de 64 KB, cada uno empieza con su numero ("B00000042|")
-- y sigue con un caracter de relleno que depende de ese numero. Esta app
-- sabe crearlo POR PARTES (SYS_FILE_APPEND, fase 5b): bloque a bloque, sin
-- tenerlo entero en memoria y sin parar el sistema, del tamaño que se
-- elija.
local gui = require("nemo_gui")
local archivos = require("nemo_archivos")

local NOMBRE, BLOQUE, CABECERA = "PRUEBA.BIN", 65536, 10
local VOL = archivos.VOL_FAT

gui.crear_ventana("Prueba de lectura", 160, 110, 380, 270)
local et_arch   = gui.crear_etiqueta("", 15, 12, 350, 22)
local et_res    = gui.crear_etiqueta("", 15, 40, 350, 22)
local et_res2   = gui.crear_etiqueta("", 15, 66, 350, 22)
local et_estado = gui.crear_etiqueta("", 15, 92, 350, 22)
local b_crear   = gui.crear_boton("Crear", 15, 130, 110, 34)
local b_seguido = gui.crear_boton("Leer seguido", 135, 130, 110, 34)
local b_azar    = gui.crear_boton("Al azar", 255, 130, 110, 34)
local TAMANOS   = { 16, 64, 256, 400 }
local tam_elegido = 1
local b_tamano  = gui.crear_boton("Tamaño: 16 MB", 15, 176, 170, 34)

-- ---- el patron ----
local function cabecera(b) return string.format("B%08d|", b) end
local function relleno(b) return string.char(33 + (b * 7) % 90) end
local function bloque_entero(b) return cabecera(b) .. string.rep(relleno(b), BLOQUE - CABECERA) end

-- Lo que DEBE haber en [pos, pos + n): se arma trozo a trozo, bloque a bloque.
local function esperado(pos, n)
  local partes, fin = {}, pos + n
  while pos < fin do
    local b = pos // BLOQUE
    local dentro = pos % BLOQUE
    local hasta = math.min(BLOQUE, dentro + (fin - pos))
    local s = ""
    if dentro < CABECERA then s = cabecera(b):sub(dentro + 1, math.min(CABECERA, hasta)) end
    local r0 = math.max(dentro, CABECERA)
    if hasta > r0 then s = s .. string.rep(relleno(b), hasta - r0) end
    partes[#partes + 1] = s
    pos = pos + (hasta - dentro)
  end
  return table.concat(partes)
end

local function info()
  local e = archivos.buscar(NOMBRE, archivos.RAIZ, VOL)
  if not e then gui.poner_texto(et_arch, NOMBRE .. ": no existe (pulsa Crear)"); return nil end
  gui.poner_texto(et_arch, string.format("%s: %.1f MB en el volumen FAT", NOMBRE, e.tamano / 1048576))
  return e.tamano
end

local function crear()
  local mb = TAMANOS[tam_elegido]
  local bloques = mb * 1048576 // BLOQUE
  local t0, ultimo = nemo.ticks(), -1
  gui.poner_texto(et_estado, string.format("Creando %d MB...", mb)); nemo.pump()
  local total = archivos.crear_por_partes(NOMBRE, archivos.RAIZ,
    function(b) if b < bloques then return bloque_entero(b) end end,     -- un bloque de 64 KB cada vez
    function(escrito)
      local ya = escrito // (4 * 1048576)
      if ya ~= ultimo then            -- cada 4 MB: progreso y dejar respirar a la interfaz
        ultimo = ya
        gui.poner_texto(et_estado, string.format("Creando... %d de %d MB", escrito // 1048576, mb))
        nemo.pump()
      end
    end)
  local seg = math.max(nemo.ticks() - t0, 1) / 100
  if total == mb * 1048576 then
    gui.poner_texto(et_res, string.format("Escritura: %d MB en %.1f s = %.1f MB/s", mb, seg, mb / seg))
    gui.poner_texto(et_estado, "Creado")
  else
    gui.poner_texto(et_estado, total and string.format("Solo %d MB: disco lleno?", total // 1048576) or "No se pudo crear")
  end
  info()
end

-- Lee el archivo entero a trozos de 256 KB, comparando CADA trozo con lo
-- esperado (la comparacion de cadenas la hace C, es rapida).
local function leer_seguido()
  local tam = info(); if not tam then return end
  local t0, malos, ultimo_mb = nemo.ticks(), 0, -1
  local total = archivos.leer_por_partes(NOMBRE, archivos.RAIZ, VOL, function(datos, pos)
    if datos ~= esperado(pos, #datos) then malos = malos + 1 end
    local mb = (pos + #datos) // (4 * 1048576)
    if mb ~= ultimo_mb then      -- cada 4 MB: progreso y dejar respirar a la interfaz
      ultimo_mb = mb
      gui.poner_texto(et_estado, string.format("Leyendo... %d MB", (pos + #datos) // 1048576))
      nemo.pump()
    end
  end)
  local seg = math.max(nemo.ticks() - t0, 1) / 100
  if not total then gui.poner_texto(et_res, "Leer seguido: FALLO de lectura"); return end
  gui.poner_texto(et_res, string.format("Seguido: %.1f MB en %.1f s = %.1f MB/s", total / 1048576, seg, total / 1048576 / seg))
  gui.poner_texto(et_estado, malos == 0 and (total == tam and "Todo correcto" or "Tamaño leido distinto!")
                                        or string.format("%d TROZOS CON DATOS MALOS", malos))
end

-- 300 lecturas en posiciones y tamaños al azar, hacia delante y atras.
local function al_azar()
  local tam = info(); if not tam then return end
  local id = archivos.abrir(NOMBRE, archivos.RAIZ, VOL)
  if not id then gui.poner_texto(et_res2, "No se pudo abrir"); return end
  local buf = nemo.buffer(archivos.TROZO_MAX)
  local malos, t0 = 0, nemo.ticks()
  for k = 1, 300 do
    local pos = math.random(0, tam - 1)
    local n = math.random(1, math.random(2) == 1 and 3000 or archivos.TROZO_MAX)
    local leidos = archivos.leer_desde(id, pos, n, VOL, buf)
    local debido = math.min(n, tam - pos)
    if leidos ~= debido or nemo.bytes(buf, 0, leidos) ~= esperado(pos, leidos) then malos = malos + 1 end
    if k % 50 == 0 then gui.poner_texto(et_estado, "Al azar... " .. k); nemo.pump() end
  end
  local seg = math.max(nemo.ticks() - t0, 1) / 100
  gui.poner_texto(et_res2, string.format("Al azar: 300 lecturas en %.1f s", seg))
  gui.poner_texto(et_estado, malos == 0 and "Todo correcto" or string.format("%d LECTURAS MALAS", malos))
end

info()
-- La ventana ha cambiado de tamaño: las etiquetas de los
-- resultados, a todo lo ancho (los textos largos se veian cortados).
local function colocar(w, h)
  gui.limpiar(gui.COLOR_VENTANA)
  gui.mover_gadget(et_arch, 15, 12, w - 30, 22)
  gui.mover_gadget(et_res, 15, 40, w - 30, 22)
  gui.mover_gadget(et_res2, 15, 66, w - 30, 22)
  gui.mover_gadget(et_estado, 15, 92, w - 30, 22)
  gui.mover_gadget(b_crear, 15, 130, 110, 34)
  gui.mover_gadget(b_seguido, 135, 130, 110, 34)
  gui.mover_gadget(b_azar, 255, 130, 110, 34)
  gui.mover_gadget(b_tamano, 15, 176, 170, 34)
end

gui.bucle(function(ev, fuente)
  local cambia, w, h = gui.tamano_cambiado()
  if cambia and w and w >= 100 then colocar(w, h) end
  if ev == gui.EVENT_GADGETACTION then
    if fuente == b_crear then crear()
    elseif fuente == b_tamano then
      tam_elegido = tam_elegido % #TAMANOS + 1
      gui.poner_texto(b_tamano, string.format("Tamaño: %d MB", TAMANOS[tam_elegido]))
    elseif fuente == b_seguido then leer_seguido()
    elseif fuente == b_azar then al_azar() end
  end
end)
