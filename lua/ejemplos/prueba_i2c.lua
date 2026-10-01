-- prueba_i2c.lua -- comprueba el bus I2C (GPIO 2 = SDA, GPIO 3 = SCL).
-- En QEMU, con la EEPROM simulada en 0x50. En la Pi, sin nada conectado
-- comprueba que el bus funciona; si tienes sensores, los encuentra.
local P = require("nemo_prueba")
local gpio = require("nemo_gpio")

local p = P.nueva("Prueba de GPIO: I2C")
local qemu = P.es_qemu()
p:nota(qemu and "En QEMU: EEPROM simulada en 0x50" or "En la Raspberry Pi: bus I2C 1 de verdad")

p:seccion("Errores")
local r, e = gpio.i2c_leer(0x80, 1)
p:ok("direccion de mas de 7 bits (0x80)", r == nil, e)
r, e = gpio.i2c_escribir(0x50, string.rep("x", 300))
p:ok("mas de 256 bytes de una vez", r == nil, e)

p:seccion("El bus: todas las direcciones, de 0x08 a 0x77")
local colgado, encontrados, errores = false, {}, 0
for dir = 0x08, 0x77 do
  local rr, ee = gpio.i2c_leer(dir, 1)
  if rr then encontrados[#encontrados + 1] = dir
  elseif ee and ee:find("no termino") then colgado = true
  elseif not (ee and ee:find("nadie")) then errores = errores + 1 end
end
p:ok("ninguna direccion cuelga el bus", not colgado)
p:ok("sin errores inesperados", errores == 0, errores .. " errores")
local lista = {}
for _, d in ipairs(encontrados) do lista[#lista + 1] = string.format("0x%02x", d) end
p:nota("Responden: " .. (#lista > 0 and table.concat(lista, " ") or "ninguna"))
local hay = gpio.i2c_buscar()
p:ok("i2c_buscar da lo mismo", hay and #hay == #encontrados)

if qemu then
  p:seccion("La EEPROM simulada (0x50)")
  p:ok("solo responde la 0x50", #encontrados == 1 and encontrados[1] == 0x50)
  p:ok("escribir 4 bytes en la posicion 16", gpio.i2c_escribir_registro(0x50, 16, { 72, 111, 108, 97 }) == 5)
  local b = gpio.i2c_leer_registro(0x50, 16, 4)
  p:ok("leerlos de vuelta: Hola", b and string.char(table.unpack(b)) == "Hola")
  local ida = {}
  for i = 1, 200 do ida[i] = (i * 37 + 11) % 256 end
  p:ok("200 bytes de una vez", gpio.i2c_escribir_registro(0x50, 0, ida) == 201)
  local vuelta = gpio.i2c_leer_registro(0x50, 0, 200)
  local iguales = vuelta and #vuelta == 200
  for i = 1, 200 do if not iguales or vuelta[i] ~= ida[i] then iguales = false; break end end
  p:ok("  y vuelven iguales, ceros incluidos", iguales)
  r, e = gpio.i2c_leer(0x3C, 1)
  p:ok("una direccion sin nada: nadie responde", r == nil and e:find("nadie"), e)
else
  p:seccion("Lo que haya conectado")
  if #encontrados == 0 then
    p:nota("No hay nada conectado: el bus funciona, pero no hay a quien hablar.")
    p:nota("Con un sensor en las patillas 1, 3, 5 y 9, apareceria su direccion.")
  end
  for _, d in ipairs(encontrados) do
    if d == 0x76 or d == 0x77 then
      local id = gpio.i2c_leer_registro(d, 0xD0)
      local que = id and (id[1] == 0x58 and "un BMP280" or id[1] == 0x60 and "un BME280" or string.format("identificador 0x%02x", id[1]))
      p:ok(string.format("0x%02x: posible sensor de presion", d), id ~= nil, que)
    elseif d == 0x3C or d == 0x3D then
      p:nota(string.format("0x%02x: probablemente una pantalla OLED (SSD1306)", d))
    elseif d >= 0x50 and d <= 0x57 then
      p:nota(string.format("0x%02x: probablemente una memoria EEPROM", d))
    else
      p:nota(string.format("0x%02x: un aparato (mira su hoja de datos)", d))
    end
  end
end
p:fin()
