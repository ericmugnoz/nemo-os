-- nota_rapida.lua -- prueba real de teclado + archivos: escribe una
-- linea de texto y la guarda en NOTA.TXT al pulsar Enter.
local gui = require("nemo_gui")
local archivos = require("nemo_archivos")

gui.crear_ventana("Nota rapida", 100, 100, 360, 120)
local etiqueta = gui.crear_etiqueta("Escribe algo y pulsa Enter:", 20, 15, 320, 20)
local texto = ""

gui.bucle(function(ev)
  local tecla = gui.leer_tecla()
  if tecla ~= 0 then
    if tecla == gui.TECLA_ENTER then
      if archivos.escribir("NOTA.TXT", texto) then
        gui.poner_texto(etiqueta, "Guardado en NOTA.TXT (" .. #texto .. " bytes)")
      else
        gui.poner_texto(etiqueta, "No se pudo guardar")
      end
    elseif tecla == gui.TECLA_BACKSPACE then
      texto = texto:sub(1, -2)
      gui.poner_texto(etiqueta, texto)
    elseif tecla >= 32 and tecla < 127 then
      texto = texto .. string.char(tecla)
      gui.poner_texto(etiqueta, texto)
    end
  end
end)
