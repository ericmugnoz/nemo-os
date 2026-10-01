-- nemo_gpio.lua -- los pines GPIO del conector de la Raspberry Pi 4.
--
--   local gpio = require("nemo_gpio")
--   gpio.modo(17, gpio.SALIDA)
--   gpio.escribir(17, 1)            -- enciende lo que haya en el pin 17
--   gpio.modo(4, gpio.ARRIBA)       -- entrada con resistencia a positivo
--   if gpio.leer(4) == 0 then ... end   -- un pulsador entre el 4 y masa
--
-- Pines del 2 al 27 (numeracion BCM, la de "GPIO17", no la de la
-- patilla del conector). El 14 y el 15 estan reservados: son la
-- terminal. El primer programa que configura un pin se lo queda; al
-- terminar, sus pines vuelven a entrada sin resistencia. En QEMU no hay
-- pines: se simulan, y cada cambio se anuncia en la terminal.
--
-- Cada funcion devuelve su resultado, o nil y un mensaje si falla.
local M = {}

local S = { MODO = 264, ESCRIBIR = 265, LEER = 266, PWM = 267, I2C_ESCRIBIR = 268, I2C_LEER = 269, SPI = 270 }

M.ENTRADA = 0   -- entrada sin resistencia (al aire, lee valores al azar)
M.SALIDA  = 1
M.ARRIBA  = 2   -- entrada con resistencia a positivo: al aire lee 1
M.ABAJO   = 3   -- entrada con resistencia a masa: al aire lee 0
M.PRIMERO, M.ULTIMO = 2, 27

local ERRORES = {
  [-1] = "valor fuera de rango: pin (del 2 al 27), modo, direccion, longitud o velocidad",
  [-2] = "pin reservado: el 14 y el 15 son la terminal",
  [-3] = "el pin es de otro programa, o no se ha configurado antes",
  [-4] = "el pin no esta configurado como salida",
  [-5] = "ese canal de PWM ya lo usa el otro pin (12 y 18 comparten; 13 y 19 tambien)",
  [-6] = "nadie responde en esa direccion I2C (revisa la direccion y los cables)",
  [-7] = "el bus I2C no termino: un aparato lo retiene o esta mal conectado",
}
local function resultado(r) if r < 0 then return nil, ERRORES[r] or ("error " .. r) end return r end

-- Configura un pin. 'modo' es M.ENTRADA, M.SALIDA, M.ARRIBA o M.ABAJO.
function M.modo(pin, modo)
  local r, e = resultado(nemo.syscall(S.MODO, pin, modo or M.ENTRADA))
  if not r then return nil, e end
  return true
end

-- Pone una salida a 1 o a 0 (tambien valen true/false).
function M.escribir(pin, valor)
  local v = (valor == true or (type(valor) == "number" and valor ~= 0)) and 1 or 0
  local r, e = resultado(nemo.syscall(S.ESCRIBIR, pin, v))
  if not r then return nil, e end
  return true
end

-- Lee un pin: 1 o 0. Cualquier programa puede leer cualquier pin.
function M.leer(pin)
  return resultado(nemo.syscall(S.LEER, pin))
end

-- Invierte una salida (1 -> 0, 0 -> 1). Devuelve el valor nuevo.
function M.alternar(pin)
  local v, e = M.leer(pin)
  if not v then return nil, e end
  local ok, e2 = M.escribir(pin, 1 - v)
  if not ok then return nil, e2 end
  return 1 - v
end

-- ---- PWM por hardware (septiembre de 2026) ----
-- Solo en los pines 12, 13, 18 y 19. El 12 y el 18 comparten canal, y el
-- 13 y el 19 tambien: se pueden usar dos a la vez (uno de cada pareja).
M.PINES_PWM = { 12, 13, 18, 19 }

-- Una senal de 'hz' ciclos por segundo, encendida el 'porcentaje' del
-- tiempo (0 a 100, con decimales). Para el brillo de un LED, unos
-- 1000 Hz. Para pararla, gpio.modo(pin, gpio.ENTRADA).
function M.pwm(pin, hz, porcentaje)
  local diez = math.floor((porcentaje or 0) * 100 + 0.5)
  local r, e = resultado(nemo.syscall(S.PWM, pin, math.floor(hz), diez))
  if not r then return nil, e end
  return true
end

-- Un servo de modelismo: 'grados' de 0 a 180. Pulso de 0,5 ms (0 grados)
-- a 2,5 ms (180) cada 20 ms (50 Hz), lo habitual; algunos servos usan de
-- 1 a 2 ms: para ellos, 'minimo_ms' y 'maximo_ms'.
function M.servo(pin, grados, minimo_ms, maximo_ms)
  minimo_ms, maximo_ms = minimo_ms or 0.5, maximo_ms or 2.5
  grados = math.max(0, math.min(180, grados))
  local ms = minimo_ms + (maximo_ms - minimo_ms) * grados / 180
  return M.pwm(pin, 50, ms / 20 * 100)            -- el pulso, en % de los 20 ms
end

-- ---- I2C (septiembre de 2026) ----
-- El bus I2C 1: GPIO 2 = SDA (patilla 3), GPIO 3 = SCL (patilla 5), a
-- 100 kHz. Cada aparato tiene una direccion de 7 bits (0x08..0x77). Como
-- mucho 256 bytes por transferencia. El primer programa que usa el bus se
-- queda los pines 2 y 3 hasta que termina.
local function a_cadena(bytes)
  if type(bytes) == "string" then return bytes end
  local t = {}
  for i, b in ipairs(bytes) do t[i] = string.char(b & 0xFF) end
  return table.concat(t)
end

-- Envia bytes (tabla de numeros, o cadena) al aparato 'dir'.
function M.i2c_escribir(dir, bytes)
  local s = a_cadena(bytes)
  local r, e = resultado(nemo.syscall(S.I2C_ESCRIBIR, dir, s, #s))
  if not r then return nil, e end
  return r
end

-- Lee 'n' bytes del aparato 'dir' -> tabla de numeros (0..255).
function M.i2c_leer(dir, n)
  local buf = nemo.buffer(n)
  local r, e = resultado(nemo.syscall(S.I2C_LEER, dir, nemo.direccion(buf), n))
  if not r then return nil, e end
  return { nemo.bytes(buf, 0, r):byte(1, -1) }
end

-- Lo habitual con un sensor: escribir el numero de registro y leer su valor.
function M.i2c_leer_registro(dir, registro, n)
  local ok, e = M.i2c_escribir(dir, { registro })
  if not ok then return nil, e end
  return M.i2c_leer(dir, n or 1)
end

-- Escribir en un registro: 'valores' es un numero o una tabla de numeros.
function M.i2c_escribir_registro(dir, registro, valores)
  local t = { registro }
  if type(valores) == "number" then t[2] = valores else for _, v in ipairs(valores) do t[#t + 1] = v end end
  return M.i2c_escribir(dir, t)
end

-- Que hay conectado: la lista de direcciones que responden (como
-- i2cdetect en Linux). Prueba a leer un byte de cada una.
function M.i2c_buscar()
  local hay = {}
  for dir = 0x08, 0x77 do
    local r = nemo.syscall(S.I2C_LEER, dir, nemo.direccion(nemo.buffer(1)), 1)
    if r == 1 then hay[#hay + 1] = dir end
    if r == -2 or r == -3 then return nil, ERRORES[r] end
  end
  return hay
end

-- ---- SPI (septiembre de 2026) ----
-- El bus SPI0: GPIO 11 = reloj (patilla 23), 10 = MOSI (19), 9 = MISO
-- (21), y un selector por aparato: 8 = CE0 (24) y 7 = CE1 (26). Por cada
-- byte que sale, entra otro. Hasta 4096 bytes por llamada.
--
-- opciones (todas opcionales): chip = 0 o 1 (CE0/CE1; 0), hz = velocidad
-- (100 kHz..50 MHz; 1 MHz), modo = 0..3 (el que diga la hoja de datos del
-- aparato; 0). Devuelve la tabla de bytes recibidos.
function M.spi(bytes, opciones)
  opciones = opciones or {}
  local s = a_cadena(bytes)
  local buf = nemo.buffer(#s)
  local r, e = resultado(nemo.syscall(S.SPI, opciones.chip or 0, s, #s, opciones.hz or 1000000,
                                      nemo.direccion(buf), opciones.modo or 0))
  if not r then return nil, e end
  return { nemo.bytes(buf, 0, r):byte(1, -1) }
end

-- Una entrada analogica de un conversor MCP3008 (8 canales, 10 bits):
-- de 0 (0 V) a 1023 (la tension de VREF, 3,3 V si se conecta ahi).
function M.mcp3008(canal, chip)
  local r, e = M.spi({ 1, 0x80 | ((canal & 7) << 4), 0 }, { chip = chip or 0, hz = 1000000 })
  if not r then return nil, e end
  return ((r[2] & 3) << 8) | r[3]
end

return M
