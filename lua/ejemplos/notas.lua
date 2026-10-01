-- notas.lua -- notas rapidas de varias lineas, guardadas cada una en
-- su propio archivo dentro de la carpeta NOTAS (se crea sola si no
-- existe). Lista a la izquierda, edicion a la derecha -- el cursor
-- se mueve igual que en editor.lua (mismo driver de teclado, mismas
-- limitaciones: solo flechas, sin Inicio/Fin/Supr todavia).
local gui = require("nemo_gui")
local fs = require("nemo_archivos")

local ANCHO, ALTO = 560, 420
gui.crear_ventana("Notas", 90, 60, ANCHO, ALTO)
gui.usar_fuente("mono", 10)   -- antes, la 5x7 en mayusculas

-- Tamaño: el de la ventana AHORA; ver colocar.
local ALTO_C = ALTO
local FILA_H = math.max(10, gui.alto_fuente() + 1)   -- el alto de linea de la fuente, mas un pixel
local avance = gui.avance_fuente()

-- Carpeta NOTAS: se busca, y si no existe se crea, una sola vez.
local function carpeta_notas()
  local e = fs.buscar("NOTAS", fs.RAIZ, fs.VOL_NEMOFS)
  if e and fs.es_carpeta_entrada(e) then return e.inodo end
  fs.crear_carpeta("NOTAS", fs.RAIZ, fs.VOL_NEMOFS)
  e = fs.buscar("NOTAS", fs.RAIZ, fs.VOL_NEMOFS)
  return e and e.inodo or fs.RAIZ -- si ni crearla funciono, mejor la raiz que nada
end
local carpeta = carpeta_notas()

local lista = gui.crear_lista(10, 10, 160, 330)
local boton_nueva = gui.crear_boton("Nueva", 10, 350, 75, 30)
local boton_borrar = gui.crear_boton("Borrar", 95, 350, 75, 30)
local boton_guardar = gui.crear_boton("Guardar", 180, 350, 370, 30)
local ZONA_X, ZONA_Y, ZONA_ANCHO = 180, 10, 370
local ZONA_ALTO = 335
local MAX_FILAS = math.floor((350 - ZONA_Y) / FILA_H)

local notas = {}      -- indice de la lista (0-based) -> nombre de archivo
local lineas = { "" }
local cl, cc, scroll = 1, 1, 1
local nombre_actual = nil
local modificado = false

local function refrescar_lista()
  local entradas = fs.listar(carpeta, fs.VOL_NEMOFS)
  table.sort(entradas, function(a, b) return a.nombre < b.nombre end)
  gui.lista_limpiar(lista)
  notas = {}
  -- BUG REAL CORREGIDO: esto era 'notas[#notas] = e.nombre'.
  -- El operador # de Lua NO cuenta el indice 0, asi que #notas valia 0
  -- en TODAS las vueltas y las notas se pisaban unas a otras en la
  -- misma casilla: al final solo quedaba la ultima. Efecto que se veia:
  -- elegir la primera fila abria la ULTIMA nota, y las demas filas no
  -- hacian nada (notas[sel] era nil). El indice se lleva ahora a mano,
  -- y sigue siendo 0-based porque asi lo devuelve lista_seleccionado().
  local i = 0
  for _, e in ipairs(entradas) do
    if not fs.es_carpeta_entrada(e) then
      gui.lista_anadir(lista, e.nombre)
      notas[i] = e.nombre
      i = i + 1
    end
  end
end

local function cargar_nota(nombre)
  local contenido = fs.leer_en(nombre, carpeta, fs.VOL_NEMOFS) or ""
  lineas = {}
  for linea in (contenido .. "\n"):gmatch("(.-)\n") do lineas[#lineas + 1] = linea end
  if #lineas == 0 then lineas = { "" } end
  cl, cc, scroll = 1, 1, 1
  nombre_actual = nombre
  modificado = false
  gui.titulo("Notas - " .. nombre)
end

local function guardar_nota()
  if not nombre_actual then return end
  if fs.escribir_en(nombre_actual, table.concat(lineas, "\n"), carpeta, fs.VOL_NEMOFS) then
    modificado = false
    gui.titulo("Notas - " .. nombre_actual)
  end
end

local function nueva_nota()
  local t = gui.fecha_hora()
  local nombre = string.format("NOTA_%02d%02d%02d_%02d%02d%02d.TXT",
    t.anio % 100, t.mes, t.dia, t.hora, t.minuto, t.segundo)
  fs.escribir_en(nombre, "", carpeta, fs.VOL_NEMOFS)
  refrescar_lista()
  cargar_nota(nombre)
end

local function borrar_nota()
  if not nombre_actual then return end
  fs.borrar_en(nombre_actual, carpeta, fs.VOL_NEMOFS)
  nombre_actual = nil
  lineas = { "" }
  cl, cc, scroll = 1, 1, 1
  gui.titulo("Notas")
  refrescar_lista()
end

local function ajustar_scroll()
  if cl < scroll then scroll = cl end
  if cl > scroll + MAX_FILAS - 1 then scroll = cl - MAX_FILAS + 1 end
  if scroll < 1 then scroll = 1 end
end

local function redibujar_texto()
  gui.rect(ZONA_X, ZONA_Y, ZONA_ANCHO, ZONA_ALTO, gui.rgb(255, 255, 255))
  if not nombre_actual then
    gui.texto(ZONA_X + 4, ZONA_Y + 4, "Elige una nota, o pulsa Nueva.", gui.rgb(120, 120, 120))
    return
  end
  local y = ZONA_Y
  for i = scroll, math.min(scroll + MAX_FILAS - 1, #lineas) do
    gui.texto(ZONA_X + 4, y, lineas[i], gui.rgb(0, 0, 0))
    if i == cl then
      local cx = ZONA_X + 4 + (cc - 1) * avance
      gui.rect(cx, y, 2, FILA_H - 1, gui.rgb(0, 0, 0))
    end
    y = y + FILA_H
  end
end

local function insertar(c)
  local linea = lineas[cl]
  lineas[cl] = linea:sub(1, cc - 1) .. c .. linea:sub(cc)
  cc = cc + 1; modificado = true
end
local function nueva_linea()
  local linea = lineas[cl]
  local resto = linea:sub(cc)
  lineas[cl] = linea:sub(1, cc - 1)
  table.insert(lineas, cl + 1, resto)
  cl = cl + 1; cc = 1; modificado = true
end
local function borrar_atras()
  if cc > 1 then
    local linea = lineas[cl]
    lineas[cl] = linea:sub(1, cc - 2) .. linea:sub(cc)
    cc = cc - 1; modificado = true
  elseif cl > 1 then
    local anterior = lineas[cl - 1]
    cc = #anterior + 1
    lineas[cl - 1] = anterior .. lineas[cl]
    table.remove(lineas, cl)
    cl = cl - 1; modificado = true
  end
end
local function mover(codigo)
  if codigo == 0x13 then
    if cc > 1 then cc = cc - 1 elseif cl > 1 then cl = cl - 1; cc = #lineas[cl] + 1 end
  elseif codigo == 0x14 then
    if cc <= #lineas[cl] then cc = cc + 1 elseif cl < #lineas then cl = cl + 1; cc = 1 end
  elseif codigo == 0x11 then
    if cl > 1 then cl = cl - 1; cc = math.min(cc, #lineas[cl] + 1) end
  elseif codigo == 0x12 then
    if cl < #lineas then cl = cl + 1; cc = math.min(cc, #lineas[cl] + 1) end
  end
end

-- La ventana ha cambiado de tamaño: la lista y la zona de texto crecen
-- con ella; los botones se quedan abajo.
local function colocar(w, h)
  ALTO_C = h
  local abajo = h - 40                           -- la fila de botones
  gui.limpiar(gui.COLOR_VENTANA)
  gui.mover_gadget(lista, 10, 10, 160, math.max(60, abajo - 20))
  gui.mover_gadget(boton_nueva, 10, abajo, 75, 30)
  gui.mover_gadget(boton_borrar, 95, abajo, 75, 30)
  ZONA_ANCHO = math.max(120, w - ZONA_X - 10)
  gui.mover_gadget(boton_guardar, 180, abajo, ZONA_ANCHO, 30)
  ZONA_ALTO = math.max(40, abajo - ZONA_Y - 5)
  MAX_FILAS = math.max(1, math.floor(ZONA_ALTO / FILA_H))
end

refrescar_lista()

gui.bucle(function(ev, fuente)
  local cambio = false
  local cambia, w, h = gui.tamano_cambiado()
  if cambia then
    if w and w >= 100 then colocar(w, h) end
    ajustar_scroll()
    redibujar_texto()
  end

  if ev == gui.EVENT_GADGETACTION then
    if fuente == boton_nueva then nueva_nota(); cambio = true
    elseif fuente == boton_borrar then borrar_nota(); cambio = true
    elseif fuente == boton_guardar then guardar_nota(); cambio = true
    elseif fuente == lista then
      local sel = gui.lista_seleccionado(lista)
      if sel >= 0 and notas[sel] then cargar_nota(notas[sel]); cambio = true end
    end
  end

  if nombre_actual then
    local tecla = gui.leer_tecla()
    if tecla ~= 0 then
      cambio = true
      if tecla == gui.TECLA_ENTER then nueva_linea()
      elseif tecla == gui.TECLA_BACKSPACE then borrar_atras()
      elseif tecla >= 0x11 and tecla <= 0x14 then mover(tecla)
      elseif tecla >= 32 and tecla < 127 then insertar(string.char(tecla))
      else cambio = false end
    end
  end

  if cambio then
    ajustar_scroll()
    redibujar_texto()
  end
end)
