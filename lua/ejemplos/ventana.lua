-- ventana.lua -- abre una ventana real y espera a que la cierres
local gui = require("nemo_gui")
local v = gui.crear_ventana("Lua en Nemo OS", 100, 100, 400, 300)
print("ventana creada, handle:", v)
gui.bucle(function(ev, fuente, datos)
  if ev ~= 0 then
    print("evento", string.format("0x%x", ev), "fuente", fuente, "datos", datos)
  end
end)
print("ventana cerrada, fin")
