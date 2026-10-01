-- prueba_spi.lua -- comprueba el bus SPI0. En QEMU, con el conversor
-- MCP3008 simulado (CE0) y el espejo (CE1). En la Pi, sin nada conectado
-- comprueba que las transferencias terminan; con un CABLE entre las
-- patillas 19 (MOSI) y 21 (MISO), comprueba el bus en los dos sentidos.
local P = require("nemo_prueba")
local gpio = require("nemo_gpio")

local p = P.nueva("Prueba de GPIO: SPI")
local qemu = P.es_qemu()
p:nota(qemu and "En QEMU: MCP3008 simulado en CE0 y espejo en CE1" or "En la Raspberry Pi: SPI0 de verdad")

local function datos(n, semilla) local t = {} for i = 1, n do t[i] = (i * semilla + 7) % 256 end return t end
local function iguales(a, b) if not a or #a ~= #b then return false end for i = 1, #a do if a[i] ~= b[i] then return false end end return true end

p:seccion("Errores")
local r, e = gpio.spi({ 1, 2 }, { chip = 2 })
p:ok("aparato 2 (solo hay 0 y 1)", r == nil, e)
r, e = gpio.spi({ 1, 2 }, { hz = 50000 })
p:ok("50 kHz: por debajo del minimo (100 kHz)", r == nil, e)
r, e = gpio.spi({ 1, 2 }, { modo = 4 })
p:ok("modo 4 (solo hay del 0 al 3)", r == nil, e)
r, e = gpio.spi(string.rep("x", 5000))
p:ok("mas de 4096 bytes de una vez", r == nil, e)

p:seccion("Las transferencias terminan: 4 modos y 4 velocidades")
local todas = true
for modo = 0, 3 do
  for _, hz in ipairs({ 100000, 1000000, 10000000, 50000000 }) do
    local rr = gpio.spi(datos(64, 3), { chip = 1, hz = hz, modo = modo })
    if not rr or #rr ~= 64 then todas = false; p:nota(string.format("modo %d a %d Hz: %s", modo, hz, rr and #rr or "error")) end
  end
end
p:ok("16 combinaciones, 64 bytes cada una", todas)
local grande = gpio.spi(datos(4096, 5), { chip = 1, hz = 8000000 })
p:ok("4096 bytes de una vez (el maximo)", grande and #grande == 4096)

if qemu then
  p:seccion("El conversor MCP3008 simulado (CE0)")
  local bien = true
  for canal = 0, 7 do if gpio.mcp3008(canal) ~= canal * 128 + 37 then bien = false end end
  p:ok("los 8 canales dan canal x 128 + 37", bien, "0: " .. tostring(gpio.mcp3008(0)) .. ", 7: " .. tostring(gpio.mcp3008(7)))
  p:seccion("El espejo (CE1)")
  local ida = datos(256, 11)
  p:ok("256 bytes vuelven iguales", iguales(gpio.spi(ida, { chip = 1 }), ida))
  p:ok("4096 bytes vuelven iguales", iguales(grande, datos(4096, 5)))
  p:ok("una cadena vale igual", iguales(gpio.spi("Nemo", { chip = 1 }), { 78, 101, 109, 111 }))
else
  p:seccion("En los dos sentidos: un cable entre las patillas 19 y 21")
  local ida = datos(256, 11)
  local vuelta = gpio.spi(ida, { chip = 0, hz = 1000000 })
  if iguales(vuelta, ida) then
    p:ok("puente detectado: 256 bytes vuelven iguales", true)
    p:ok("  a 10 MHz tambien", iguales(gpio.spi(ida, { chip = 0, hz = 10000000 }), ida))
    p:ok("  4096 bytes vuelven iguales", iguales(grande, datos(4096, 5)))
    p:ok("  en modo 3 tambien", iguales(gpio.spi(ida, { chip = 0, modo = 3 }), ida))
  else
    local todos0, todos255 = true, true
    for i = 1, #(vuelta or {}) do if vuelta[i] ~= 0 then todos0 = false end; if vuelta[i] ~= 255 then todos255 = false end end
    p:nota("Sin puente: lo recibido no es lo enviado (" .. (todos0 and "todo 0" or todos255 and "todo 255" or "valores sueltos") .. ").")
    p:nota("Pon un cable entre las patillas 19 y 21 y vuelve a ejecutarlo:")
    p:nota("comprobara que el SPI envia y recibe de verdad.")
  end
  p:seccion("Un MCP3008 en CE0 (si lo tienes)")
  local v = gpio.mcp3008(0)
  p:nota("canal 0: " .. tostring(v) .. "  (con un potenciometro, cambia al girarlo;")
  p:nota("sin conversor, el valor no significa nada)")
end
p:fin()
