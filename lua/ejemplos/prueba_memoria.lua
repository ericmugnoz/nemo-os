-- prueba_memoria.lua -- pone a prueba el monton de Lua que crece (fase 4
-- de la memoria). Cada app de Lua empieza con 4 MB; cuando se le acaban,
-- pide otra zona al kernel (SYS_MEM_PEDIR) y sigue. Aqui se pide memoria
-- a proposito, de 32 en 32 MB, y cada vez se comprueba que TODOS los
-- datos creados hasta entonces siguen intactos. En la terminal se ve cada
-- zona que entrega el kernel ("... crece: zona extra de ...").
local gui = require("nemo_gui")
local sistema = require("nemo_sistema")

gui.crear_ventana("Prueba de memoria", 180, 120, 340, 190)
local et_datos  = gui.crear_etiqueta("Datos: 0 MB", 15, 12, 310, 22)
local et_monton = gui.crear_etiqueta("", 15, 38, 310, 22)
local et_sist   = gui.crear_etiqueta("", 15, 64, 310, 22)
local et_estado = gui.crear_etiqueta("Pulsa \"Pedir 32 MB\"", 15, 90, 310, 22)
local b_pedir   = gui.crear_boton("Pedir 32 MB", 15, 130, 150, 34)
local b_soltar  = gui.crear_boton("Soltar todo", 175, 130, 150, 34)

local MB = 1024 * 1024
local bloques = {}          -- bloques de 1 MB, cada uno con contenido propio

-- Un bloque de 1 MB que se sabe comprobar: letra de relleno y numero al final.
local function hacer_bloque(i)
  return string.rep(string.char(65 + i % 26), MB - 16) .. string.format("%016d", i)
end
local function bloque_bien(i, b)
  return #b == MB and b:byte(1) == 65 + i % 26 and b:sub(-16) == string.format("%016d", i)
end

local function refrescar()
  gui.poner_texto(et_datos, string.format("Datos: %d MB", #bloques))
  gui.poner_texto(et_monton, string.format("Monton de Lua: %.1f MB", collectgarbage("count") / 1024))
  local usado, total = sistema.memoria_de_tareas()
  gui.poner_texto(et_sist, string.format("Programas (todo el sistema): %d de %d MB", usado // MB, total // MB))
end

local function pedir()
  local antes = #bloques
  for i = antes + 1, antes + 32 do
    local ok, b = pcall(hacer_bloque, i)
    if not ok then
      gui.poner_texto(et_estado, string.format("Sin memoria tras %d MB", #bloques))
      refrescar(); return
    end
    bloques[i] = b
  end
  for i, b in ipairs(bloques) do
    if not bloque_bien(i, b) then
      gui.poner_texto(et_estado, string.format("DATOS CORRUPTOS en el MB %d", i))
      refrescar(); return
    end
  end
  gui.poner_texto(et_estado, string.format("%d MB comprobados: todo intacto", #bloques))
  refrescar()
end

local function soltar()
  bloques = {}
  collectgarbage(); collectgarbage()
  gui.poner_texto(et_estado, "Soltado (las zonas siguen siendo del programa)")
  refrescar()
end

refrescar()
-- La ventana ha cambiado de tamaño: las etiquetas de los
-- resultados, a todo lo ancho (los textos largos se veian cortados).
local function colocar(w, h)
  gui.limpiar(gui.COLOR_VENTANA)
  for i, et in ipairs({ et_datos, et_monton, et_sist, et_estado }) do
    gui.mover_gadget(et, 15, 12 + (i - 1) * 26, w - 30, 22)
  end
  gui.mover_gadget(b_pedir, 15, 130, 150, 34)
  gui.mover_gadget(b_soltar, 175, 130, 150, 34)
end

gui.bucle(function(ev, fuente)
  local cambia, w, h = gui.tamano_cambiado()
  if cambia and w and w >= 100 then colocar(w, h) end
  if ev == gui.EVENT_GADGETACTION then
    if fuente == b_pedir then pedir() elseif fuente == b_soltar then soltar() end
  end
end)
