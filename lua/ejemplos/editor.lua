-- editor.lua -- editor de texto multilinea de verdad, en Lua.
--
-- Cursor que se mueve con las flechas (no hay Inicio/Fin/Supr en el
-- driver de teclado todavia -- solo flechas, Enter y Backspace),
-- scroll automatico para seguir al cursor, y Archivo>Nuevo/Abrir/
-- Guardar/Guardar como por menu de verdad (no hay Ctrl detectable en
-- el driver, asi que los atajos de teclado no son una opcion todavia).
local gui = require("nemo_gui")
local fs = require("nemo_archivos")

local ANCHO, ALTO = 700, 500
gui.crear_ventana("Editor - Sin titulo", 60, 40, ANCHO, ALTO)
gui.usar_fuente("mono", 10)   -- antes, la 5x7 en mayusculas

-- Menu Archivo
local raiz = gui.menu_raiz()
local menu_archivo = gui.crear_menu("ARCHIVO", 0, raiz)
local id_nuevo         = gui.crear_menu("NUEVO", 1, menu_archivo)
local id_abrir         = gui.crear_menu("ABRIR", 2, menu_archivo)
local id_guardar       = gui.crear_menu("GUARDAR", 3, menu_archivo)
local id_guardar_como  = gui.crear_menu("GUARDAR COMO", 4, menu_archivo)
local id_salir         = gui.crear_menu("SALIR", 5, menu_archivo)

-- Medidas: las ACTUALES de la ventana, que cambian al
-- maximizarla o restaurarla. gui.tamano_ventana() ya descuenta la barra
-- de menus; la de titulo se pinta por encima de la zona de dibujo.
local STATUS_H = 12
local FILA_H = math.max(10, gui.alto_fuente() + 1)   -- el alto de linea de la fuente, mas un pixel
local ANCHO_C, ALTO_V, ALTO_C, MAX_FILAS
local function medir()
  local w, h = gui.tamano_ventana()
  if not w or w < 100 or h < 60 then w, h = ANCHO, ALTO - gui.MENUBAR_H end
  local cambia = (w ~= ANCHO_C or h ~= ALTO_V)
  ANCHO_C, ALTO_V = w, h
  ALTO_C = h - STATUS_H                 -- el area de texto; debajo, la barra de estado
  MAX_FILAS = math.max(1, math.floor(ALTO_C / FILA_H))
  return cambia
end
medir()
local avance = gui.avance_fuente()
local MARGEN_X = 4

-- ---- estado del documento ----
local lineas = { "" }
local cl, cc = 1, 1     -- linea y columna del cursor (columna 1 = antes del primer caracter)
local scroll = 1        -- primera linea visible
local modificado = false
local carpeta_actual = fs.RAIZ
local nombre_archivo = nil

local function titulo_ventana()
  local nombre = nombre_archivo or "Sin titulo"
  return "Editor - " .. nombre .. (modificado and " *" or "")
end
local function actualizar_titulo() gui.titulo(titulo_ventana()) end

local function ajustar_scroll()
  if cl < scroll then scroll = cl end
  if cl > scroll + MAX_FILAS - 1 then scroll = cl - MAX_FILAS + 1 end
  if scroll < 1 then scroll = 1 end
end

local function redibujar()
  gui.rect(0, 0, ANCHO_C, ALTO_C, gui.rgb(255, 255, 255))
  local y = 0
  for i = scroll, math.min(scroll + MAX_FILAS - 1, #lineas) do
    gui.texto(MARGEN_X, y, lineas[i], gui.rgb(0, 0, 0))
    if i == cl then
      local cx = MARGEN_X + (cc - 1) * avance
      gui.rect(cx, y, 2, FILA_H - 1, gui.rgb(0, 0, 0)) -- cursor: barra vertical fina
    end
    y = y + FILA_H
  end
  -- barra de estado, debajo del area de texto
  gui.rect(0, ALTO_C, ANCHO_C, STATUS_H, gui.rgb(200, 200, 200))
  local estado = string.format("Ln %d, Col %d  --  %d lineas", cl, cc, #lineas)
  gui.texto(MARGEN_X, ALTO_C + 2, estado, gui.rgb(0, 0, 0))
end

-- ---- edicion ----
local function insertar(c)
  local linea = lineas[cl]
  lineas[cl] = linea:sub(1, cc - 1) .. c .. linea:sub(cc)
  cc = cc + 1
  modificado = true
end

local function nueva_linea()
  local linea = lineas[cl]
  local resto = linea:sub(cc)
  lineas[cl] = linea:sub(1, cc - 1)
  table.insert(lineas, cl + 1, resto)
  cl = cl + 1
  cc = 1
  modificado = true
end

local function borrar_atras()
  if cc > 1 then
    local linea = lineas[cl]
    lineas[cl] = linea:sub(1, cc - 2) .. linea:sub(cc)
    cc = cc - 1
    modificado = true
  elseif cl > 1 then
    local anterior = lineas[cl - 1]
    cc = #anterior + 1
    lineas[cl - 1] = anterior .. lineas[cl]
    table.remove(lineas, cl)
    cl = cl - 1
    modificado = true
  end
end

local function mover(codigo)
  if codigo == gui.TECLA_ENTER then return
  elseif codigo == 0x13 then -- izquierda
    if cc > 1 then cc = cc - 1
    elseif cl > 1 then cl = cl - 1; cc = #lineas[cl] + 1 end
  elseif codigo == 0x14 then -- derecha
    if cc <= #lineas[cl] then cc = cc + 1
    elseif cl < #lineas then cl = cl + 1; cc = 1 end
  elseif codigo == 0x11 then -- arriba
    if cl > 1 then cl = cl - 1; cc = math.min(cc, #lineas[cl] + 1) end
  elseif codigo == 0x12 then -- abajo
    if cl < #lineas then cl = cl + 1; cc = math.min(cc, #lineas[cl] + 1) end
  end
end

-- ---- archivo ----
local function nuevo_documento()
  lineas = { "" }
  cl, cc, scroll = 1, 1, 1
  modificado = false
  nombre_archivo = nil
  carpeta_actual = fs.RAIZ
  actualizar_titulo()
end

local function cargar(nombre, carpeta)
  local contenido = fs.leer_en(nombre, carpeta, fs.VOL_NEMOFS)
  if not contenido then return false end
  lineas = {}
  for linea in (contenido .. "\n"):gmatch("(.-)\n") do lineas[#lineas + 1] = linea end
  if #lineas == 0 then lineas = { "" } end
  cl, cc, scroll = 1, 1, 1
  modificado = false
  nombre_archivo = nombre
  carpeta_actual = carpeta
  actualizar_titulo()
  return true
end

local function contenido_completo() return table.concat(lineas, "\n") end

local function guardar_en(nombre, carpeta)
  if fs.escribir_en(nombre, contenido_completo(), carpeta, fs.VOL_NEMOFS) then
    nombre_archivo = nombre
    carpeta_actual = carpeta
    modificado = false
    actualizar_titulo()
    return true
  end
  return false
end

local function guardar()
  if nombre_archivo then
    guardar_en(nombre_archivo, carpeta_actual)
  else
    local nombre, carpeta = gui.dialogo_guardar(carpeta_actual)
    if nombre then guardar_en(nombre, carpeta) end
    redibujar() -- el dialogo pinto encima de todo el contenido
  end
end

actualizar_titulo()
redibujar()

gui.bucle(function(ev, fuente)
  local cambio = medir()          -- la ventana ha cambiado de tamaño: repintar entera

  if ev == gui.EVENT_MENUACTION then
    cambio = true
    if fuente == id_nuevo then
      nuevo_documento()
    elseif fuente == id_abrir then
      local nombre, carpeta = gui.dialogo_abrir(fs.RAIZ)
      if nombre then cargar(nombre, carpeta) end
      redibujar() -- el dialogo pinto encima de todo el contenido
    elseif fuente == id_guardar then
      guardar()
    elseif fuente == id_guardar_como then
      local nombre, carpeta = gui.dialogo_guardar(carpeta_actual)
      if nombre then guardar_en(nombre, carpeta) end
      redibujar()
    elseif fuente == id_salir then
      return false
    end
  end

  local tecla = gui.leer_tecla()
  if tecla ~= 0 then
    cambio = true
    if tecla == gui.TECLA_ENTER then nueva_linea()
    elseif tecla == gui.TECLA_BACKSPACE then borrar_atras()
    elseif tecla >= 0x11 and tecla <= 0x14 then mover(tecla)
    elseif tecla >= 32 and tecla < 127 then insertar(string.char(tecla))
    else cambio = false end
  end

  if cambio then
    ajustar_scroll()
    redibujar()
  end
end)
