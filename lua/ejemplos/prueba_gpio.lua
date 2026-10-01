-- prueba_gpio.lua -- comprueba los pines GPIO. Funciona en QEMU y en la
-- Pi, SIN NADA CONECTADO (en la Pi, desconecta lo que tengas en los
-- pines 4 y 17 antes de ejecutarlo).
local P = require("nemo_prueba")
local gpio = require("nemo_gpio")
local sistema = require("nemo_sistema")

local p = P.nueva("Prueba de GPIO: pines")
local qemu = P.es_qemu()
p:nota(qemu and "En QEMU: pines simulados (cada cambio sale en la terminal)" or "En la Raspberry Pi: pines de verdad")

local function lee(pin) sistema.esperar(5); return gpio.leer(pin) end

p:seccion("Errores: cada uno con su mensaje")
local ok, e = gpio.modo(14, gpio.SALIDA)
p:ok("el 14 esta reservado (la terminal)", not ok and e and e:find("reservado"), e)
ok, e = gpio.modo(1, gpio.SALIDA)
p:ok("el 1 esta fuera del conector", not ok and e and e:find("fuera"), e)
ok, e = gpio.modo(28, gpio.SALIDA)
p:ok("el 28 tambien", not ok and e and e:find("fuera"), e)
ok, e = gpio.modo(17, 9)
p:ok("un modo que no existe (9)", not ok and e ~= nil, e)
ok, e = gpio.escribir(22, 1)
p:ok("escribir en un pin sin configurar", not ok and e and e:find("configurado"), e)
gpio.modo(22, gpio.ENTRADA)
ok, e = gpio.escribir(22, 1)
p:ok("escribir en una entrada", not ok and e and e:find("salida"), e)

p:seccion("Una salida: el pin 17")
p:ok("configurar el 17 como salida", gpio.modo(17, gpio.SALIDA))
p:ok("recien configurada, empieza a 0", lee(17) == 0, "lee " .. tostring(lee(17)))
gpio.escribir(17, 1)
p:ok("escribir 1 -> se lee 1", lee(17) == 1, "lee " .. tostring(lee(17)))
gpio.escribir(17, 0)
p:ok("escribir 0 -> se lee 0", lee(17) == 0, "lee " .. tostring(lee(17)))
p:ok("alternar -> 1", gpio.alternar(17) == 1 and lee(17) == 1)
p:ok("alternar otra vez -> 0", gpio.alternar(17) == 0 and lee(17) == 0)
gpio.escribir(17, true)
p:ok("true vale como 1", lee(17) == 1)
gpio.escribir(17, false)

p:seccion("Las resistencias internas: el pin 4, al aire")
p:ok("entrada con resistencia a positivo", gpio.modo(4, gpio.ARRIBA))
p:ok("  al aire lee 1", lee(4) == 1, "lee " .. tostring(lee(4)))
p:ok("entrada con resistencia a masa", gpio.modo(4, gpio.ABAJO))
p:ok("  al aire lee 0", lee(4) == 0, "lee " .. tostring(lee(4)))
p:ok("de nuevo a positivo: vuelve a 1", gpio.modo(4, gpio.ARRIBA) and lee(4) == 1)
if not qemu then
  p:nota("Si fallan: comprueba que no haya nada conectado al pin 4 (patilla 7)")
end

p:seccion("Leer cualquier pin: no hace falta ser su dueño")
local v = gpio.leer(26)
p:ok("leer el 26 sin configurarlo", v == 0 or v == 1, "lee " .. tostring(v))

-- Dejarlo todo como estaba (el sistema tambien lo hace al cerrar)
for _, pin in ipairs({ 4, 17, 22 }) do gpio.modo(pin, gpio.ENTRADA) end
p:nota("Al cerrar esta ventana, el sistema devuelve sus pines a entrada.")
p:fin()
