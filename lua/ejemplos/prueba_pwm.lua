-- prueba_pwm.lua -- comprueba el PWM por hardware. Funciona en QEMU y en
-- la Pi, SIN NADA CONECTADO. Si en la Pi tienes un LED (con su
-- resistencia) en el GPIO 18, patilla 12, al final lo veras encenderse y
-- apagarse suavemente.
local P = require("nemo_prueba")
local gpio = require("nemo_gpio")
local sistema = require("nemo_sistema")

local p = P.nueva("Prueba de GPIO: PWM")
local qemu = P.es_qemu()
p:nota(qemu and "En QEMU: PWM simulado (cada cambio sale en la terminal)" or "En la Raspberry Pi: PWM por hardware")

-- En la Pi, leer el pin muchas veces: cuantas veces esta a 1
local function muestras(pin, n)
  local unos = 0
  for _ = 1, n do if gpio.leer(pin) == 1 then unos = unos + 1 end end
  return unos
end

p:seccion("Errores")
local ok, e = gpio.pwm(17, 1000, 50)
p:ok("el 17 no tiene PWM (solo 12, 13, 18, 19)", not ok and e ~= nil, e)
ok, e = gpio.pwm(18, 5, 50)
p:ok("5 Hz: por debajo del minimo (10 Hz)", not ok, e)
ok, e = gpio.pwm(18, 200000, 50)
p:ok("200 kHz: por encima del maximo (100 kHz)", not ok, e)
ok, e = gpio.pwm(18, 1000, 150)
p:ok("150 %: fuera de rango", not ok, e)

p:seccion("Canales: 12 y 18 comparten uno; 13 y 19, el otro")
p:ok("PWM en el 18", gpio.pwm(18, 1000, 50))
ok, e = gpio.pwm(12, 1000, 50)
p:ok("el 12 ya no puede: su canal lo usa el 18", not ok and e and e:find("canal"), e)
p:ok("pero el 13 si (el otro canal)", gpio.pwm(13, 1000, 25))
ok, e = gpio.pwm(19, 1000, 50)
p:ok("y el 19 no: comparte con el 13", not ok and e and e:find("canal"), e)
p:ok("parar el 18 (pasarlo a entrada)", gpio.modo(18, gpio.ENTRADA))
p:ok("ahora el 12 si puede", gpio.pwm(12, 1000, 50))
gpio.modo(12, gpio.ENTRADA); gpio.modo(13, gpio.ENTRADA)
ok, e = gpio.escribir(18, 1)
p:ok("un pin en entrada no se escribe", not ok, e)

p:seccion("La senal llega al pin (GPIO 18)")
if qemu then
  p:nota("En QEMU el pin simulado no oscila: esto solo se comprueba en la Pi.")
  p:ok("PWM al 0, al 50 y al 100 %", gpio.pwm(18, 1000, 0) and gpio.pwm(18, 1000, 50) and gpio.pwm(18, 1000, 100))
else
  gpio.pwm(18, 1000, 0);   sistema.esperar(20)
  local u0 = muestras(18, 500)
  p:ok("al 0 %: siempre 0", u0 == 0, u0 .. " unos de 500")
  gpio.pwm(18, 1000, 100); sistema.esperar(20)
  local u100 = muestras(18, 500)
  p:ok("al 100 %: siempre 1", u100 == 500, u100 .. " unos de 500")
  gpio.pwm(18, 1000, 50);  sistema.esperar(20)
  local u50 = muestras(18, 2000)
  p:ok("al 50 %: unas veces 1 y otras 0", u50 > 100 and u50 < 1900, u50 .. " unos de 2000")
end

p:seccion("Un servo en el 18 (si no hay servo, no pasa nada)")
p:ok("0 grados", gpio.servo(18, 0))
sistema.esperar(300)
p:ok("90 grados", gpio.servo(18, 90))
sistema.esperar(300)
p:ok("180 grados", gpio.servo(18, 180))
sistema.esperar(300)
p:ok("de 1 a 2 ms, 90 grados", gpio.servo(18, 90, 1, 2))

p:seccion("Un LED en el 18: se enciende y se apaga suave (3 s)")
for vuelta = 1, 2 do
  for pc = 0, 100, 5 do gpio.pwm(18, 1000, pc); sistema.esperar(35) end
  for pc = 100, 0, -5 do gpio.pwm(18, 1000, pc); sistema.esperar(35) end
end
p:ok("parar el PWM", gpio.modo(18, gpio.ENTRADA))
p:fin()
