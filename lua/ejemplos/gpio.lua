-- gpio.lua -- panel de los pines GPIO del conector de la Raspberry Pi 4.
--
-- Una fila por pin (del 2 al 27, numeracion BCM). Clic en el MODO para
-- cambiarlo: entrada -> salida -> entrada a positivo -> entrada a masa.
-- Clic en el VALOR de una salida para encenderla o apagarla. Las
-- entradas se leen en directo, diez veces por segundo.
--
-- Al cerrar el panel, el sistema devuelve sus pines a entrada. En QEMU
-- los pines se simulan: cada cambio aparece en la terminal.
local gui = require("nemo_gui")
local gpio = require("nemo_gpio")

local ANCHO, ALTO = 460, 520      -- tamaño inicial; al redimensionar, todo se recoloca
local FONDO    = gui.rgb(245, 246, 250)
local TINTA    = gui.rgb(30, 32, 44)
local GRIS     = gui.rgb(150, 152, 165)
local ENCENDIDO = gui.rgb(40, 170, 70)
local APAGADO  = gui.rgb(200, 204, 214)
local ROJO     = gui.rgb(190, 50, 50)

local NOMBRES = { [gpio.ENTRADA] = "entrada", [gpio.SALIDA] = "SALIDA",
                  [gpio.ARRIBA] = "entrada +", [gpio.ABAJO] = "entrada -" }
local SIGUIENTE = { [gpio.ENTRADA] = gpio.SALIDA, [gpio.SALIDA] = gpio.ARRIBA,
                    [gpio.ARRIBA] = gpio.ABAJO, [gpio.ABAJO] = gpio.ENTRADA }

-- Dos columnas de 13 pines. La disposicion se calcula con el tamaño
-- ACTUAL de la ventana (antes era fija, y ni cabia: la ultima fila quedaba
-- bajo la barra de avisos, y al ampliar, el resto se veia negro).
local Y0, BARRA = 40, 34
local ALTO_LETRA = 14               -- el alto de linea de la fuente (se mide al crear la ventana)
local ancho_v, alto_v = ANCHO, ALTO
local FILA, COL_W, X1, X2 = 32, 202, 12, 226
local function recalcular()
  local w, h = gui.tamano_ventana()
  if not w or w < 200 or h < 200 then w, h = ANCHO, ALTO end
  ancho_v, alto_v = w, h
  FILA = math.max(24, (h - Y0 - BARRA - 6) // 13)      -- las 13 filas, en lo que quede
  COL_W = math.max(190, (w - 36) // 2)
  X1, X2 = 12, 24 + COL_W
end
local function sitio(pin)
  local i = pin - gpio.PRIMERO
  local col, fila = i // 13, i % 13
  return (col == 0) and X1 or X2, Y0 + fila * FILA
end
-- Dentro de una fila: el nombre, la caja del modo y el circulo del valor
local MODO_X, MODO_W = 66, 86
local function valor_x() return COL_W - 34 end

local modo, valor = {}, {}         -- lo que sabemos de cada pin
local aviso = "Clic: en el modo, lo cambia; en el circulo, enciende"
local color_aviso = GRIS

local function reservado(pin) return pin == 14 or pin == 15 end

local function dibujar_pin(pin)
  local x, y = sitio(pin)
  local ty = y + (FILA - 4 - ALTO_LETRA) // 2         -- el texto, centrado en la fila (alto real de la fuente)
  gui.rect(x, y, COL_W, FILA - 4, gui.rgb(255, 255, 255))
  gui.texto(x + 6, ty, string.format("GPIO %2d", pin), TINTA)
  if reservado(pin) then gui.texto(x + MODO_X + 4, ty, "terminal", GRIS); return end
  gui.rect(x + MODO_X, y + 4, MODO_W, FILA - 12, APAGADO)
  gui.texto(x + MODO_X + 6, ty, NOMBRES[modo[pin] or gpio.ENTRADA], TINTA)
  local v = valor[pin]
  local lado = math.min(18, FILA - 10)
  gui.ovalo(x + valor_x(), y + (FILA - 4 - lado) // 2, lado, lado, (v == 1) and ENCENDIDO or APAGADO)
  gui.texto(x + valor_x() + 22, ty, v == nil and "?" or tostring(v), TINTA)
end

-- El aviso de abajo, recortado al ancho de la ventana (la fuente es de
-- ancho fijo: 8 px por letra)
local function aviso_cabe()
  local max = ancho_v - 24
  if gui.medir_texto(aviso) <= max then return aviso end
  local t = aviso
  while #t > 1 and gui.medir_texto(t .. "\u{2026}") > max do t = t:sub(1, -2) end   -- medido, letra a letra
  return t .. "\u{2026}"
end

local function dibujar_todo()
  recalcular()
  gui.limpiar(FONDO)
  gui.texto(12, 12, "Pines GPIO (numeración BCM)", TINTA)
  for pin = gpio.PRIMERO, gpio.ULTIMO do dibujar_pin(pin) end
  gui.rect(0, alto_v - BARRA, ancho_v, BARRA, FONDO)
  gui.texto(12, alto_v - 24, aviso_cabe(), color_aviso)
end

local function avisar(texto, es_error)
  aviso, color_aviso = texto, es_error and ROJO or GRIS
  gui.rect(0, alto_v - BARRA, ancho_v, BARRA, FONDO)
  gui.texto(12, alto_v - 24, aviso_cabe(), color_aviso)
end

-- Leer todos los pines (las entradas cambian solas; las salidas, no)
local function leer_todos()
  for pin = gpio.PRIMERO, gpio.ULTIMO do
    if not reservado(pin) then
      local v = gpio.leer(pin)
      if v ~= valor[pin] then valor[pin] = v; dibujar_pin(pin) end
    end
  end
end

local function pin_en(x, y)
  for pin = gpio.PRIMERO, gpio.ULTIMO do
    local px, py = sitio(pin)
    if x >= px and x < px + COL_W and y >= py and y < py + FILA - 4 then return pin, x - px end
  end
end

local function clic(x, y)
  local pin, dx = pin_en(x, y)
  if not pin or reservado(pin) then return end
  if dx >= MODO_X and dx < MODO_X + MODO_W then
    local nuevo = SIGUIENTE[modo[pin] or gpio.ENTRADA]
    local ok, e = gpio.modo(pin, nuevo)
    if ok then modo[pin] = nuevo; avisar(string.format("GPIO %d: %s", pin, NOMBRES[nuevo]))
    else avisar(string.format("GPIO %d: %s", pin, e), true) end
  elseif dx >= valor_x() - 8 then
    if modo[pin] ~= gpio.SALIDA then avisar(string.format("GPIO %d es una entrada: cambia su modo a SALIDA", pin), true); return end
    local v, e = gpio.alternar(pin)
    if v then avisar(string.format("GPIO %d = %d", pin, v)) else avisar(e, true) end
  end
  valor[pin] = gpio.leer(pin)
  dibujar_pin(pin)
end

gui.crear_ventana("GPIO", 120, 60, ANCHO, ALTO)
gui.usar_fuente("sans", 12)   -- antes, la 5x7 en mayusculas
ALTO_LETRA = gui.alto_fuente()
dibujar_todo()
leer_todos()
local ultima = nemo.ticks()
gui.bucle(function(ev)
  -- la ventana ha cambiado de tamaño: recolocar y repintar
  local w, h = gui.tamano_ventana()
  if ev == gui.EVENT_WINDOWSIZE or (w and w >= 200 and (w ~= ancho_v or h ~= alto_v)) then dibujar_todo() end
  if gui.clic(1) then
    local x, y = gui.raton()
    if x then clic(x, y) end
  end
  local ahora = nemo.ticks()
  if ahora - ultima >= 10 then ultima = ahora; leer_todos() end   -- 10 latidos = 100 ms
end)
