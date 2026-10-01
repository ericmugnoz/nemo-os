-- contador.lua -- un boton y una etiqueta: el "gadgetdemo" en Lua
local gui = require("nemo_gui")
gui.crear_ventana("Contador", 100, 100, 300, 160)
local etiqueta = gui.crear_etiqueta("Pulsaciones: 0", 20, 20, 260, 24)
local boton    = gui.crear_boton("Pulsa aqui", 20, 60, 120, 32)
local n = 0
gui.bucle(function(ev, fuente, datos)
  if ev == gui.EVENT_GADGETACTION and fuente == boton then
    n = n + 1
    gui.poner_texto(etiqueta, "Pulsaciones: " .. n)
  end
end)
