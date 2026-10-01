-- reloj.lua -- reloj digital sencillo, la hora del RTC del sistema
-- (sin zona horaria ni configuracion -- la que lleve el hardware).
local gui = require("nemo_gui")

gui.crear_ventana("Reloj", 200, 150, 260, 130, { maximizar = false, minimizar = false })
local et_hora = gui.crear_etiqueta("--:--:--", 20, 15, 220, 40)
local et_fecha = gui.crear_etiqueta("----------", 20, 65, 220, 24)

local function dos_digitos(n) return string.format("%02d", n) end

local function actualizar()
  local t = gui.fecha_hora()
  gui.poner_texto(et_hora, string.format("%s:%s:%s",
    dos_digitos(t.hora), dos_digitos(t.minuto), dos_digitos(t.segundo)))
  gui.poner_texto(et_fecha, string.format("%s/%s/%d",
    dos_digitos(t.dia), dos_digitos(t.mes), t.anio))
end

gui.crear_temporizador(1) -- 1 Hz
actualizar()

-- La ventana ha cambiado de tamaño: la hora y la fecha,
-- centradas en vertical y a todo lo ancho.
local function colocar(w, h)
  gui.limpiar(gui.COLOR_VENTANA)
  local y0 = math.max(10, (h - 74) // 2)
  gui.mover_gadget(et_hora, 20, y0, w - 40, 40)
  gui.mover_gadget(et_fecha, 20, y0 + 50, w - 40, 24)
end

gui.bucle(function(ev)
  local cambia, w, h = gui.tamano_cambiado()
  if cambia and w and w >= 100 then colocar(w, h) end
  if ev == gui.EVENT_TIMERTICK then actualizar() end
end)
