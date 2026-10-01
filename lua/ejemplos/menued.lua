-- menued.lua -- editor del menu de Inicio de Nemo OS
--
-- El menu sale de MENU.CFG, un archivo de TEXTO en la raiz. Esto es una
-- forma comoda de tocarlo sin escribir el formato a mano -- pero el
-- formato sigue ahi, y si algo se rompe se arregla con el editor de
-- texto. Esa es la razon de que MENU.CFG sea texto y no binario.
--
-- Al guardar se llama a SYS_MENU_RELOAD: el cambio se ve en el menu sin
-- reiniciar.

local gui = require("nemo_gui")
local fs = require("nemo_archivos")

local S = { MENU_RELOAD = 278, CATALOGO = 235, DRAW_ICON = 32 }

-- nemo_gui no envuelve el dibujado de iconos del catalogo: es una
-- syscall directa. Escala 0 o 1 = 24x24.
local function icono(x, y, id) nemo.syscall(S.DRAW_ICON, x, y, id, 1) end

local ARCHIVO = "MENU.CFG"
local FILA = 24
local BARRA = 34
local ESTADO = 20
local ICONO = 20
local MAX_SEC, MAX_ITEM = 10, 14

local ventana_w, ventana_h = 760, 560
local secciones = {}          -- { nombre=, icono=, items={ {nombre=,destino=,icono=} } }
local sel_sec, sel_item = 1, 0   -- sel_item 0 = la seccion en si
local estado = ""
local repintar = true
local catalogo = 0
local escribiendo = false     -- nil | "nombre" | "destino"
local buf = ""
local eligiendo_icono = false

-- ---- leer y escribir MENU.CFG ----
local function partir(linea)
  local t = {}
  for p in linea:gmatch("%S+") do t[#t + 1] = p end
  return t
end

local function cargar()
  secciones = {}
  local txt = fs.leer_en(ARCHIVO, fs.RAIZ, nil, 8192)   -- (nombre, padre, volumen, maximo)
  if not txt then estado = "No hay MENU.CFG: se empieza de cero" return end
  for linea in (txt .. "\n"):gmatch("([^\n]*)\n") do
    local sin = linea:gsub(";.*", "")
    local t = partir(sin)
    if #t >= 2 then
      if t[1]:sub(1, 1):lower() == "s" and #secciones < MAX_SEC then
        secciones[#secciones + 1] = { nombre = t[2], icono = tonumber(t[3]) or 0, items = {} }
      elseif t[1]:sub(1, 1):lower() == "i" and #secciones > 0 and #t >= 3 then
        local sc = secciones[#secciones]
        if #sc.items < MAX_ITEM then
          sc.items[#sc.items + 1] = { nombre = t[2], destino = t[3], icono = tonumber(t[4]) or 0 }
        end
      end
    end
  end
  estado = #secciones .. " secciones"
end

local function guardar()
  local t = {
    "; MENU.CFG -- el menu de Inicio de Nemo OS",
    "; Escrito por el editor del menu; se puede tocar a mano.",
    "; Apagar y Reiniciar no se ponen aqui: van siempre al final.",
    "",
  }
  for _, sc in ipairs(secciones) do
    t[#t + 1] = string.format("seccion %s %d", sc.nombre, sc.icono)
    for _, it in ipairs(sc.items) do
      t[#t + 1] = string.format("  item %s %s %d", it.nombre, it.destino, it.icono)
    end
    t[#t + 1] = ""
  end
  if fs.escribir_en(ARCHIVO, table.concat(t, "\n"), fs.RAIZ) then
    nemo.syscall(S.MENU_RELOAD)          -- el menu se entera al momento
    estado = "Guardado y aplicado"
  else
    estado = "No se pudo guardar"
  end
end

-- ---- la lista de filas visibles: secciones con sus entradas debajo ----
local function filas()
  local f = {}
  for i, sc in ipairs(secciones) do
    f[#f + 1] = { tipo = "sec", sec = i, sc = sc }
    for k, it in ipairs(sc.items) do
      f[#f + 1] = { tipo = "item", sec = i, item = k, it = it }
    end
  end
  return f
end

local function seleccion_valida()
  if sel_sec < 1 or sel_sec > #secciones then return false end
  if sel_item > 0 and sel_item > #secciones[sel_sec].items then return false end
  return true
end

-- ---- acciones ----
local function nueva_seccion()
  if #secciones >= MAX_SEC then estado = "No caben mas secciones" return end
  secciones[#secciones + 1] = { nombre = "Nueva", icono = 0, items = {} }
  sel_sec, sel_item = #secciones, 0
  estado = "Seccion nueva: ponle nombre"
end

local function nueva_entrada()
  if not seleccion_valida() then estado = "Elige antes una seccion" return end
  local sc = secciones[sel_sec]
  if #sc.items >= MAX_ITEM then estado = "Esa seccion esta llena" return end
  sc.items[#sc.items + 1] = { nombre = "Nuevo", destino = "programa.pro", icono = 0 }
  sel_item = #sc.items
  estado = "Entrada nueva: ponle nombre y destino"
end

local function quitar()
  if not seleccion_valida() then return end
  if sel_item > 0 then
    table.remove(secciones[sel_sec].items, sel_item)
    sel_item = 0
    estado = "Entrada quitada"
  else
    table.remove(secciones, sel_sec)
    if sel_sec > #secciones then sel_sec = #secciones end
    estado = "Seccion quitada"
  end
end

-- Mover arriba o abajo DENTRO de su nivel: una seccion entre secciones,
-- una entrada dentro de su seccion. Es el orden en que se veran.
local function mover(d)
  if not seleccion_valida() then return end
  if sel_item > 0 then
    local l = secciones[sel_sec].items
    local j = sel_item + d
    if j < 1 or j > #l then return end
    l[sel_item], l[j] = l[j], l[sel_item]
    sel_item = j
  else
    local j = sel_sec + d
    if j < 1 or j > #secciones then return end
    secciones[sel_sec], secciones[j] = secciones[j], secciones[sel_sec]
    sel_sec = j
  end
  estado = "Movido"
end

local function empezar_a_escribir(que)
  if not seleccion_valida() then estado = "Elige antes una fila" return end
  if que == "destino" and sel_item == 0 then estado = "Una seccion no lanza nada" return end
  escribiendo = que
  if sel_item > 0 then
    buf = (que == "nombre") and secciones[sel_sec].items[sel_item].nombre
                            or secciones[sel_sec].items[sel_item].destino
  else
    buf = secciones[sel_sec].nombre
  end
  estado = "Escribe y pulsa Intro (Esc cancela)"
end

local function terminar_de_escribir(aceptar)
  local que = escribiendo
  escribiendo = false
  if not aceptar or buf == "" then estado = "Cancelado" return end
  -- Sin espacios: el formato separa por espacios, y un nombre con uno
  -- partiria la linea en dos al releerla.
  buf = buf:gsub("%s+", "_")
  if sel_item > 0 then
    local it = secciones[sel_sec].items[sel_item]
    if que == "nombre" then it.nombre = buf else it.destino = buf end
  else
    secciones[sel_sec].nombre = buf
  end
  estado = "Cambiado"
end

-- ---- dibujo ----
local BOTONES = {
  { "seccion", "+ Seccion" }, { "entrada", "+ Entrada" }, { "quitar", "Quitar" },
  { "nombre", "Nombre" }, { "destino", "Destino" }, { "icono", "Icono" },
  { "sube", "Sube" }, { "baja", "Baja" }, { "guardar", "Guardar" },
}
local function ancho_boton(t)
  local w = (gui.medir_texto and gui.medir_texto(t) or #t * 8) + 14
  return w < 48 and 48 or w
end
local function rect_boton(k)
  local x = 4
  for i, b in ipairs(BOTONES) do
    local w = ancho_boton(b[2])
    if i == k then return x, 4, w, 26 end
    x = x + w + 4
  end
end

-- Los iconos del catalogo son de 24x24 y no se pueden escalar a menos,
-- asi que las filas miden lo que mide un icono.
local function icono_fila(x, y, id) icono(x, y, id) end

local function pintar()
  gui.rect(0, 0, ventana_w, BARRA, 0xD4D0C8)
  for i, b in ipairs(BOTONES) do
    local x, y, w = rect_boton(i)
    gui.rect(x, y, w, 26, 0xECECEC)
    gui.rect(x, y, w, 1, 0x909090)
    local tw = gui.medir_texto and gui.medir_texto(b[2]) or #b[2] * 8
    gui.texto(x + (w - tw) // 2, y + 6, b[2], 0x202020)
  end

  local zy, zh = BARRA, ventana_h - BARRA - ESTADO
  gui.rect(0, zy, ventana_w, zh, 0xFFFFFF)

  local f = filas()
  for i, fila in ipairs(f) do
    local y = zy + (i - 1) * FILA
    if y + FILA > zy + zh then break end
    local elegida = (fila.tipo == "sec" and fila.sec == sel_sec and sel_item == 0)
                 or (fila.tipo == "item" and fila.sec == sel_sec and fila.item == sel_item)
    if elegida then gui.rect(0, y, ventana_w, FILA, 0xD8E4F4) end
    local x = (fila.tipo == "item") and 28 or 6
    local ic = (fila.tipo == "sec") and fila.sc.icono or fila.it.icono
    icono_fila(x, y + 2, ic)
    if fila.tipo == "sec" then
      gui.texto(x + ICONO + 6, y + 4, fila.sc.nombre, 0x202020)
      gui.texto(x + ICONO + 6 + 200, y + 4, "(" .. #fila.sc.items .. ")", 0x808080)
    else
      gui.texto(x + ICONO + 6, y + 4, fila.it.nombre, 0x202020)
      gui.texto(x + ICONO + 6 + 200, y + 4, fila.it.destino, 0x606060)
      gui.texto(ventana_w - 60, y + 4, "ico " .. fila.it.icono, 0x909090)
    end
  end

  -- el cuadro de escritura, encima de todo
  if escribiendo then
    local w, h = 420, 60
    local x, y = (ventana_w - w) // 2, (ventana_h - h) // 2
    gui.rect(x, y, w, h, 0xD4D0C8)
    gui.rect(x, y, w, 1, 0x000000)
    gui.texto(x + 8, y + 6, escribiendo == "destino" and "Programa a lanzar:" or "Nombre:", 0x202020)
    gui.rect(x + 8, y + 26, w - 16, 24, 0xFFFFFF)
    gui.texto(x + 12, y + 31, buf .. "_", 0x000000)
  end

  -- el catalogo de iconos, cuando se esta eligiendo
  if eligiendo_icono then
    local cols = (ventana_w - 12) // 28
    local filas_ic = (catalogo + cols - 1) // cols
    local h = filas_ic * 28 + 10
    local y = ventana_h - ESTADO - h
    gui.rect(0, y, ventana_w, h, 0xD4D0C8)
    for i = 0, catalogo - 1 do
      icono(6 + (i % cols) * 28, y + 5 + (i // cols) * 28, i)
    end
  end

  gui.rect(0, ventana_h - ESTADO, ventana_w, ESTADO, 0xD4D0C8)
  gui.texto(6, ventana_h - ESTADO + 3, estado, 0x202020)
end

-- ---- arranque ----
gui.crear_ventana("Menu de Inicio", 70, 70, ventana_w, ventana_h)
catalogo = nemo.syscall(S.CATALOGO)
if catalogo < 1 then catalogo = 13 end
cargar()

while true do
  if gui.tamano_cambiado() then
    ventana_w, ventana_h = gui.tamano_ventana()
    repintar = true
  end
  if gui.sondear_evento() == gui.EVENT_WINDOWCLOSE then break end

  -- Para escribir se usa leer_tecla (ASCII ya traducido, con mayusculas
  -- y simbolos segun el teclado), no el scancode: un scancode no sabe si
  -- estabas pulsando mayusculas.
  if escribiendo then
    local c = gui.leer_tecla()
    while c ~= 0 do
      repintar = true
      if c == 13 or c == 10 then terminar_de_escribir(true)
      elseif c == 27 then terminar_de_escribir(false)
      elseif c == 8 then buf = buf:sub(1, -2)
      elseif c >= 32 and c < 127 and #buf < 24 then buf = buf .. string.char(c)
      end
      if not escribiendo then break end
      c = gui.leer_tecla()
    end
  else
    local k = gui.siguiente_codigo()
    if k == 1 then
      repintar = true
      if eligiendo_icono then eligiendo_icono = false else break end
    end
  end

  if gui.clic(1) and not escribiendo then
    local mx, my = gui.raton()
    if mx then
      repintar = true
      if eligiendo_icono then
        local cols = (ventana_w - 12) // 28
        local filas_ic = (catalogo + cols - 1) // cols
        local h = filas_ic * 28 + 10
        local y0 = ventana_h - ESTADO - h
        if my >= y0 and my < ventana_h - ESTADO then
          local i = ((my - y0 - 5) // 28) * cols + (mx - 6) // 28
          if i >= 0 and i < catalogo and seleccion_valida() then
            if sel_item > 0 then secciones[sel_sec].items[sel_item].icono = i
            else secciones[sel_sec].icono = i end
            estado = "Icono " .. i
          end
        end
        eligiendo_icono = false
      elseif my < BARRA then
        for i, b in ipairs(BOTONES) do
          local x, y, w = rect_boton(i)
          if mx >= x and mx < x + w and my >= y and my < y + 26 then
            if b[1] == "seccion" then nueva_seccion()
            elseif b[1] == "entrada" then nueva_entrada()
            elseif b[1] == "quitar" then quitar()
            elseif b[1] == "nombre" then empezar_a_escribir("nombre")
            elseif b[1] == "destino" then empezar_a_escribir("destino")
            elseif b[1] == "icono" then
              if seleccion_valida() then eligiendo_icono = true
              else estado = "Elige antes una fila" end
            elseif b[1] == "sube" then mover(-1)
            elseif b[1] == "baja" then mover(1)
            elseif b[1] == "guardar" then guardar()
            end
            break
          end
        end
      elseif my < ventana_h - ESTADO then
        local i = (my - BARRA) // FILA + 1
        local f = filas()
        if f[i] then
          sel_sec = f[i].sec
          sel_item = (f[i].tipo == "item") and f[i].item or 0
        end
      end
    end
  end

  if repintar then pintar(); repintar = false end
  nemo.pump()
end
