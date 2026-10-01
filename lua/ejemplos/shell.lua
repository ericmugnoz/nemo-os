-- shell.lua -- interprete de comandos completo, en Lua, para las DOS
-- unidades del sistema (NemoFS = "C:", FAT = "F:", igual que la
-- shell real en C).
--
-- Pantalla dibujada a mano (gui.rect/gui.texto) en vez de un gadget
-- TextArea -- ese es de solo lectura y no da control fino sobre el
-- cursor ni el scroll. Navegacion de carpetas real por inodo/cluster
-- (ver nemo_archivos.lua, que usa SYS_FILE_LIST -- la unica syscall
-- de listado que de verdad distingue el volumen); historial con
-- flecha arriba/abajo.
local gui = require("nemo_gui")
local fs = require("nemo_archivos")

local ANCHO, ALTO = 640, 440
gui.crear_ventana("Shell Lua", 80, 60, ANCHO, ALTO)
gui.usar_fuente("mono", 10)   -- antes, la 5x7 en mayusculas

-- El tamaño de la zona de dibujo: el ACTUAL, que cambia al maximizar o
-- restaurar la ventana. Antes era fijo, y al ampliar, el
-- resto se veia negro. (Y se le restaba la barra de titulo, que en
-- realidad se pinta por ENCIMA de la zona de dibujo: sobraban 24 px.)
local FILA_H = math.max(10, gui.alto_fuente() + 1)   -- el alto de linea de la fuente, mas un pixel
local ANCHO_C, ALTO_C, MAX_LINEAS
local function medir()
  local w, h = gui.tamano_ventana()
  if not w or w < 100 or h < 60 then w, h = ANCHO, ALTO end
  local cambia = (w ~= ANCHO_C or h ~= ALTO_C)
  ANCHO_C, ALTO_C = w, h
  MAX_LINEAS = math.floor((ALTO_C - FILA_H - 8) / FILA_H)
  return cambia
end
medir()

local lineas = {}          -- scrollback, la mas vieja en [1]
local entrada = ""
local historial = {}
local historial_pos = 0    -- 0 = no navegando historial

local volumen = fs.VOL_NEMOFS
-- pila de carpetas para cd: cada elemento {inodo=N, nombre="X"}
local pila = { { inodo = fs.RAIZ, nombre = "" } }

local function letra_unidad() return volumen == fs.VOL_FAT and "F:" or "C:" end

local function ruta_actual()
  local partes = {}
  for i = 2, #pila do partes[#partes + 1] = pila[i].nombre end
  return letra_unidad() .. "/" .. table.concat(partes, "/")
end

local function cambiar_unidad(vol)
  volumen = vol
  pila = { { inodo = fs.RAIZ, nombre = "" } }
end

local function escribir(s)
  for linea in (s .. "\n"):gmatch("(.-)\n") do
    lineas[#lineas + 1] = linea
  end
  while #lineas > 500 do table.remove(lineas, 1) end -- tope razonable de memoria
end

local avance = gui.avance_fuente() -- ancho real entre caracteres, para el cursor

local function redibujar()
  gui.rect(0, 0, ANCHO_C, ALTO_C, gui.rgb(24, 28, 32))

  -- la salida se llena desde arriba; el prompt va JUSTO DESPUES de la
  -- ultima linea escrita (no fijo al fondo, que dejaba un hueco
  -- cuando aun habia poco texto) -- salvo que ya haya suficiente
  -- contenido para llegar al fondo por si solo, ahi se queda fijo y
  -- las lineas mas viejas dejan de verse (scroll normal de terminal).
  local visibles = math.min(MAX_LINEAS, #lineas)
  local y = 4
  for i = #lineas - visibles + 1, #lineas do
    if i >= 1 then
      gui.texto(4, y, lineas[i], gui.rgb(120, 220, 120))
      y = y + FILA_H
    end
  end
  local y_prompt = math.min(y, ALTO_C - FILA_H - 2)

  local prompt_texto = ruta_actual() .. "> " .. entrada
  gui.texto(4, y_prompt, prompt_texto, gui.rgb(220, 220, 220))
  local cursor_x = 4 + gui.medir_texto(prompt_texto)   -- medido: acierta tambien con tildes en la ruta
  gui.rect(cursor_x, y_prompt, avance - 1, FILA_H - 2, gui.rgb(220, 220, 220))
end

-- ---- comandos ----
local comandos = {}

comandos["pwd"] = function() escribir(ruta_actual()) end

comandos["cd"] = function(arg)
  if arg == "" or arg == "/" then
    pila = { { inodo = fs.RAIZ, nombre = "" } }
    return
  end
  if arg == ".." then
    if #pila > 1 then table.remove(pila) end
    return
  end
  local actual = pila[#pila]
  local e = fs.buscar(arg, actual.inodo, volumen)
  if not e then escribir("No existe: " .. arg); return end
  if not fs.es_carpeta_entrada(e) then escribir(arg .. " no es una carpeta"); return end
  pila[#pila + 1] = { inodo = e.inodo, nombre = arg }
end

comandos["ls"] = function()
  local actual = pila[#pila]
  local entradas = fs.listar(actual.inodo, volumen)
  if #entradas == 0 then escribir("(carpeta vacia)"); return end
  table.sort(entradas, function(a, b) return a.nombre < b.nombre end)
  for _, e in ipairs(entradas) do
    local marca = fs.es_carpeta_entrada(e) and "/" or ""
    escribir("  " .. e.nombre .. marca)
  end
end
comandos["dir"] = comandos["ls"]

comandos["mkdir"] = function(arg)
  if arg == "" then escribir("uso: mkdir <nombre>"); return end
  if volumen == fs.VOL_FAT then escribir("FAT no admite crear carpetas todavia."); return end
  local actual = pila[#pila]
  escribir(fs.crear_carpeta(arg, actual.inodo, volumen) and "Carpeta creada." or "No se pudo crear.")
end

comandos["del"] = function(arg)
  if arg == "" then escribir("uso: del <archivo>"); return end
  local actual = pila[#pila]
  escribir(fs.borrar_en(arg, actual.inodo, volumen) and "Borrado." or "No se pudo borrar.")
end
comandos["rm"] = comandos["del"]

comandos["cat"] = function(arg)
  if arg == "" then escribir("uso: cat <archivo>"); return end
  local actual = pila[#pila]
  local contenido = fs.leer_en(arg, actual.inodo, volumen)
  if not contenido then escribir("No se pudo leer: " .. arg); return end
  for linea in (contenido .. "\n"):gmatch("(.-)\n") do escribir(linea) end
end
comandos["type"] = comandos["cat"]

comandos["echo"] = function(arg) escribir(arg) end

comandos["cls"] = function() lineas = {} end
comandos["clear"] = comandos["cls"]

comandos["run"] = function(arg)
  if arg == "" then escribir("uso: run <programa.pro> [argumento]"); return end
  local nombre, resto = arg:match("^(%S+)%s*(.*)$")
  local actual = pila[#pila]
  nemo.syscall(5, nombre, resto, actual.inodo)  -- SYS_LAUNCH_PROGRAM
end

-- Cambio de unidad: "disco fat" / "disco nemofs", o solo "C:"/"F:"
-- sueltos -- las dos formas, igual que la shell real en C.
comandos["disco"] = function(arg)
  if arg == "fat" then cambiar_unidad(fs.VOL_FAT); escribir("Cambiado al disco FAT.")
  elseif arg == "nemofs" then cambiar_unidad(fs.VOL_NEMOFS); escribir("Cambiado a NemoFS.")
  else escribir("uso: disco fat | disco nemofs") end
end
comandos["c:"] = function() cambiar_unidad(fs.VOL_NEMOFS) end
comandos["f:"] = function() cambiar_unidad(fs.VOL_FAT) end

comandos["help"] = function()
  escribir("Comandos: pwd cd ls mkdir del cat echo cls run disco help exit")
  escribir("cd .. sube un nivel, cd / vuelve a la raiz")
  escribir("C: / F: cambian de unidad (o 'disco fat' / 'disco nemofs')")
end

local terminado = false
comandos["exit"] = function() terminado = true end

local function ejecutar(linea)
  escribir(ruta_actual() .. "> " .. linea)
  local cmd, arg = linea:match("^(%S*)%s*(.-)$")
  if cmd == "" then return end
  local f = comandos[cmd:lower()]
  if f then
    local ok, err = pcall(f, arg or "")
    if not ok then escribir("error: " .. tostring(err)) end
  else
    escribir("Comando desconocido: " .. cmd .. " (escribe 'help')")
  end
end

escribir("Shell Lua para Nemo OS -- escribe 'help' para ver los comandos.")
escribir("Carpeta actual: " .. ruta_actual())
redibujar()

gui.bucle(function(ev)
  if terminado then return false end

  local tecla = gui.leer_tecla()
  local cambio = medir()          -- la ventana ha cambiado de tamaño: repintar entera

  if tecla ~= 0 then
    cambio = true
    if tecla == gui.TECLA_ENTER then
      if entrada ~= "" then
        historial[#historial + 1] = entrada
        historial_pos = 0
        ejecutar(entrada)
      end
      entrada = ""
    elseif tecla == gui.TECLA_BACKSPACE then
      entrada = entrada:sub(1, -2)
    elseif tecla >= 32 and tecla < 127 then
      entrada = entrada .. string.char(tecla)
    elseif tecla == 0x11 then -- CH_UP: retroceder en el historial
      if #historial > 0 then
        if historial_pos == 0 then historial_pos = #historial
        elseif historial_pos > 1 then historial_pos = historial_pos - 1 end
        entrada = historial[historial_pos]
      end
    elseif tecla == 0x12 then -- CH_DOWN: avanzar en el historial
      if historial_pos > 0 and historial_pos < #historial then
        historial_pos = historial_pos + 1
        entrada = historial[historial_pos]
      else
        historial_pos = 0
        entrada = ""
      end
    else
      cambio = false
    end
  end

  if cambio then redibujar() end
  if terminado then return false end
end)
