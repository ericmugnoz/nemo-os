-- visor_imagenes.lua -- solo entiende el formato propio NIMG (no hay
-- decodificador de PNG/JPEG real). Para convertir una foto normal a
-- NIMG hace falta nimg_convert.py, en el host (fuera de Nemo OS) --
-- ver la nota junto a SYS_LOAD_IMAGE en syscall.c para el formato
-- exacto si algun dia se escribe un conversor dentro del propio
-- sistema.
local gui = require("nemo_gui")
local fs = require("nemo_archivos")

local ANCHO, ALTO = 520, 420
gui.crear_ventana("Visor de imagenes", 100, 60, ANCHO, ALTO)
-- La zona de dibujo mide lo que mida la ventana AHORA; ver
-- colocar(). (Antes se le restaba la barra de titulo, que se pinta por
-- encima de la zona de dibujo, y todo era fijo: al ampliar, negro.)
local ANCHO_C, ALTO_C = ANCHO, ALTO

local boton_anterior = gui.crear_boton("< Anterior", 10, 10, 100, 28)
local boton_siguiente = gui.crear_boton("Siguiente >", 120, 10, 100, 28)
local etiqueta = gui.crear_etiqueta("Buscando archivos .nimg...", 240, 14, 270, 20)

local ZONA_Y = 46
local ZONA_ALTO = ALTO_C - ZONA_Y

local archivos = {}
local indice = 1
local handle_actual = nil
local carpeta_imagenes = fs.RAIZ

local function terminacion_nimg(nombre) return nombre:lower():match("%.nimg$") ~= nil end

-- DOCUMENTOS/IMAGENES -- se crea IMAGENES si no existe (DOCUMENTOS se
-- asume que ya existe, es una carpeta del sistema; si tampoco
-- existiera, se cae a la raiz antes que fallar sin mas).
local function resolver_carpeta_imagenes()
  local documentos = fs.buscar("DOCUMENTOS", fs.RAIZ, fs.VOL_NEMOFS)
  if not documentos or not fs.es_carpeta_entrada(documentos) then return fs.RAIZ end
  local imagenes = fs.buscar("IMAGENES", documentos.inodo, fs.VOL_NEMOFS)
  if imagenes and fs.es_carpeta_entrada(imagenes) then return imagenes.inodo end
  fs.crear_carpeta("IMAGENES", documentos.inodo, fs.VOL_NEMOFS)
  imagenes = fs.buscar("IMAGENES", documentos.inodo, fs.VOL_NEMOFS)
  return imagenes and imagenes.inodo or documentos.inodo
end

local function buscar_imagenes()
  local entradas = fs.listar(carpeta_imagenes, fs.VOL_NEMOFS)
  table.sort(entradas, function(a, b) return a.nombre < b.nombre end)
  archivos = {}
  for _, e in ipairs(entradas) do
    if not fs.es_carpeta_entrada(e) and terminacion_nimg(e.nombre) then
      archivos[#archivos + 1] = e.nombre
    end
  end
end

local S = { DIBUJAR_ESCALADO = 277 }

-- Ancho y alto de un .nimg leyendo SOLO su cabecera: 12 bytes, sin
-- cargar la imagen. "NIMG" + ancho + alto, enteros de 32 bits en orden
-- little-endian.
local cab_buf = nemo.buffer(12)
local function tamano_nimg(nombre)
  local id = fs.abrir(nombre, carpeta_imagenes)
  if not id then return nil end
  local n = fs.leer_desde(id, 0, 12, nil, cab_buf)
  fs.cerrar(id)                      -- siempre: en FAT son ocho huecos en todo el sistema
  if not n or n < 12 then return nil end
  local cab = nemo.bytes(cab_buf, 0, 12)
  if cab:sub(1, 4) ~= "NIMG" then return nil end
  local function u32(i)
    return cab:byte(i) | (cab:byte(i+1) << 8) | (cab:byte(i+2) << 16) | (cab:byte(i+3) << 24)
  end
  return u32(5), u32(9)
end

local function mostrar()
  gui.rect(0, ZONA_Y, ANCHO_C, ZONA_ALTO, gui.rgb(40, 40, 40))
  if handle_actual then gui.liberar_imagen(handle_actual); handle_actual = nil end

  if #archivos == 0 then
    gui.poner_texto(etiqueta, "No hay archivos .nimg en la raiz")
    return
  end

  local nombre = archivos[indice]

  -- El archivo NO se carga en la tabla de imagenes del sistema: se le
  -- pide al kernel que lo dibuje escalado dentro de un lienzo del
  -- tamaño de la ventana (SYS_DRAW_FILE_SCALED, modo 0 = encajar).
  --
  -- Dos motivos. Uno, la tabla no admite imagenes de mas de 1024x1024,
  -- asi que un fondo de pantalla de 1920x1080 no se podia ni abrir.
  -- Dos, aunque cupiera, se dibujaba a tamaño real y se salia de la
  -- ventana; ahora se ve entera, siempre.
  local lw = math.min(ANCHO_C, 1024)
  local lh = math.min(ZONA_ALTO, 1024)
  if lw < 1 or lh < 1 then return end

  local lienzo = gui.crear_imagen(lw, lh)
  if lienzo < 0 then
    gui.poner_texto(etiqueta, nombre .. " -- sin memoria para mostrarlo")
    return
  end
  handle_actual = lienzo

  if nemo.syscall(S.DIBUJAR_ESCALADO, nombre, lienzo, 0, (lw << 16) | lh, 0) ~= 0 then
    gui.liberar_imagen(lienzo)
    handle_actual = nil
    gui.poner_texto(etiqueta, nombre .. " -- no se pudo cargar")
    return
  end

  gui.dibujar_imagen(lienzo, (ANCHO_C - lw) // 2, ZONA_Y + (ZONA_ALTO - lh) // 2)

  -- El tamaño REAL del archivo, que el lienzo ya no lo dice: se lee su
  -- cabecera, 12 bytes.
  local w, h = tamano_nimg(nombre)
  if w then
    gui.poner_texto(etiqueta, string.format("%s (%d/%d) -- %dx%d", nombre, indice, #archivos, w, h))
  else
    gui.poner_texto(etiqueta, string.format("%s (%d/%d)", nombre, indice, #archivos))
  end
end

local function anterior()
  if #archivos == 0 then return end
  indice = (indice - 2) % #archivos + 1
  mostrar()
end
local function siguiente()
  if #archivos == 0 then return end
  indice = indice % #archivos + 1
  mostrar()
end

-- La ventana ha cambiado de tamaño: la zona de la imagen, entera, y la
-- imagen, centrada en ella; la etiqueta, hasta el borde.
local function colocar(w, h)
  ANCHO_C, ALTO_C = w, h
  ZONA_ALTO = ALTO_C - ZONA_Y
  gui.limpiar(gui.COLOR_VENTANA)
  gui.mover_gadget(etiqueta, 240, 14, math.max(100, w - 250), 20)
  mostrar()
end

carpeta_imagenes = resolver_carpeta_imagenes()
buscar_imagenes()

gui.bucle(function(ev, fuente)
  local cambia, w, h = gui.tamano_cambiado()
  if cambia then
    if w and w >= 100 then colocar(w, h) else mostrar() end
  end
  if ev == gui.EVENT_GADGETACTION then
    if fuente == boton_anterior then anterior()
    elseif fuente == boton_siguiente then siguiente() end
  end
end)
