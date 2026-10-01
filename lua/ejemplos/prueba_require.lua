-- prueba_require.lua -- diagnostico minimo: solo intenta cargar
-- nemo_sistema y muestra el error COMPLETO (todos los buscadores que
-- probo y por que fallo cada uno), sin ejecutar nada mas.
local gui = require("nemo_gui")
gui.crear_ventana("Prueba require", 100, 100, 500, 300)
local etiqueta = gui.crear_etiqueta("Probando...", 10, 10, 480, 20)

local ok, resultado = pcall(require, "nemo_sistema")
local texto
if ok then
  texto = "EXITO: nemo_sistema cargo bien"
else
  texto = "FALLO:\n" .. tostring(resultado)
end
print(texto)
gui.poner_texto(etiqueta, ok and "EXITO -- ver tambien la shell" or "FALLO -- ver la shell para el detalle")

gui.bucle(function(ev)
  if ev == gui.EVENT_WINDOWCLOSE then return false end
end)
