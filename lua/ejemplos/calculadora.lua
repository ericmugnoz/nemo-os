-- calculadora.lua -- calculadora sencilla, un operando pendiente y un
-- operador pendiente (sin precedencia -- como cualquier calculadora
-- basica de bolsillo: cada operacion se resuelve segun llega).
local gui = require("nemo_gui")

gui.crear_ventana("Calculadora", 150, 100, 220, 300, { maximizar = false })

local pantalla = gui.crear_etiqueta("0", 10, 10, 200, 30)

local acumulado = 0
local operador_pendiente = nil
local entrada_actual = "0"
local recien_calculado = false

local function refrescar() gui.poner_texto(pantalla, entrada_actual) end

local function pulsar_digito(d)
  if recien_calculado then entrada_actual = "0"; recien_calculado = false end
  if entrada_actual == "0" then entrada_actual = d
  else entrada_actual = entrada_actual .. d end
  refrescar()
end

local function pulsar_punto()
  if recien_calculado then entrada_actual = "0"; recien_calculado = false end
  if not entrada_actual:find("%.") then
    entrada_actual = entrada_actual .. "."
    refrescar()
  end
end

local function aplicar_pendiente()
  local n = tonumber(entrada_actual) or 0
  if operador_pendiente == "+" then acumulado = acumulado + n
  elseif operador_pendiente == "-" then acumulado = acumulado - n
  elseif operador_pendiente == "*" then acumulado = acumulado * n
  elseif operador_pendiente == "/" then
    if n == 0 then entrada_actual = "Error: /0"; refrescar(); operador_pendiente = nil; return false end
    acumulado = acumulado / n
  else
    acumulado = n
  end
  return true
end

local function pulsar_operador(op)
  if not aplicar_pendiente() then return end
  operador_pendiente = op
  entrada_actual = tostring(acumulado)
  recien_calculado = true
end

local function pulsar_igual()
  if not aplicar_pendiente() then return end
  entrada_actual = tostring(acumulado)
  operador_pendiente = nil
  recien_calculado = true
  refrescar()
end

local function pulsar_c()
  acumulado, operador_pendiente, entrada_actual, recien_calculado = 0, nil, "0", false
  refrescar()
end

local function pulsar_signo()
  local n = tonumber(entrada_actual) or 0
  entrada_actual = tostring(-n)
  refrescar()
end

-- Botones en una rejilla 4x5: fila de C/signo, luego digitos+operadores.
local BW, BH, MX, MY = 45, 40, 8, 8
local botones = {}   -- id de gadget -> funcion a ejecutar
local rejilla = {}   -- id de gadget -> {col, fila}, para recolocarlos
local function boton(texto, col, fila, accion)
  local x = MX + (col - 1) * (BW + 4)
  local y = 50 + (fila - 1) * (BH + 4)
  local id = gui.crear_boton(texto, x, y, BW, BH)
  botones[id] = accion
  rejilla[id] = { col, fila }
end

-- La ventana ha cambiado de tamaño: la pantalla y la rejilla
-- de botones se estiran para llenarla.
local function colocar(w, h)
  gui.limpiar(gui.COLOR_VENTANA)
  gui.mover_gadget(pantalla, 10, 10, w - 20, 30)
  local bw = math.max(30, (w - 2 * MX - 3 * 4) // 4)
  local bh = math.max(24, (h - 50 - MY - 4 * 4) // 5)
  for id, cf in pairs(rejilla) do
    gui.mover_gadget(id, MX + (cf[1] - 1) * (bw + 4), 50 + (cf[2] - 1) * (bh + 4), bw, bh)
  end
end

boton("C", 1, 1, pulsar_c)
boton("+/-", 2, 1, pulsar_signo)
boton("/", 4, 1, function() pulsar_operador("/") end)

boton("7", 1, 2, function() pulsar_digito("7") end)
boton("8", 2, 2, function() pulsar_digito("8") end)
boton("9", 3, 2, function() pulsar_digito("9") end)
boton("*", 4, 2, function() pulsar_operador("*") end)

boton("4", 1, 3, function() pulsar_digito("4") end)
boton("5", 2, 3, function() pulsar_digito("5") end)
boton("6", 3, 3, function() pulsar_digito("6") end)
boton("-", 4, 3, function() pulsar_operador("-") end)

boton("1", 1, 4, function() pulsar_digito("1") end)
boton("2", 2, 4, function() pulsar_digito("2") end)
boton("3", 3, 4, function() pulsar_digito("3") end)
boton("+", 4, 4, function() pulsar_operador("+") end)

boton("0", 1, 5, function() pulsar_digito("0") end)
boton(".", 2, 5, pulsar_punto)
boton("=", 3, 5, pulsar_igual)

gui.bucle(function(ev, fuente)
  local cambia, w, h = gui.tamano_cambiado()
  if cambia and w and w >= 100 then colocar(w, h) end
  if ev == gui.EVENT_GADGETACTION and botones[fuente] then
    botones[fuente]()
  end
end)
