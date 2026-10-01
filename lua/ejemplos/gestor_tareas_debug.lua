-- gestor_tareas.lua -- lista las tareas vivas y permite cerrar
-- cualquiera (util para una que se haya quedado bloqueada). Se
-- actualiza sola cada segundo con un temporizador.
local gui = require("nemo_gui")

gui.crear_ventana("Gestor de tareas", 120, 80, 360, 320)

local ok, sistema = pcall(require, "nemo_sistema")
if not ok then
  print("FALLO AL CARGAR nemo_sistema:")
  print(tostring(sistema))
  gui.crear_etiqueta("ERROR -- ver la shell", 10, 10, 340, 60)
  gui.bucle(function(ev) if ev == gui.EVENT_WINDOWCLOSE then return false end end)
  return
end
print("nemo_sistema cargo bien dentro de gestor_tareas")

local etiqueta = gui.crear_etiqueta("Tareas vivas:", 10, 10, 340, 20)
local lista = gui.crear_lista(10, 35, 340, 200)
local boton_cerrar = gui.crear_boton("Cerrar tarea seleccionada", 10, 245, 340, 32)
local boton_actualizar = gui.crear_boton("Actualizar ahora", 10, 285, 340, 26)

local tareas_actuales = {}  -- indice de la lista (0-based) -> slot real
local firma_anterior = nil  -- para no reconstruir la lista (y perder la seleccion) si no cambio nada

local function firma_de(tareas)
  -- OJO: 'turnos' cambia constantemente para cualquier tarea activa
  -- -- si entrara en la firma, la lista se consideraria "cambiada" en
  -- CADA comprobacion y se reconstruiria siempre, exactamente el
  -- problema que se queria evitar. La firma es solo de que TAREAS
  -- existen (por su slot), no de sus contadores.
  local partes = {}
  for _, t in ipairs(tareas) do partes[#partes + 1] = t.slot end
  return table.concat(partes, ",")
end

local function actualizar(forzar)
  local tareas = sistema.listar_tareas()
  local firma = firma_de(tareas)
  -- Sin esto, el temporizador de 1Hz reconstruiria la lista cada
  -- segundo aunque no cambiara nada, y SYS_LISTBOX_CLEAR resetea la
  -- seleccion -- en la practica, casi imposible seleccionar una tarea
  -- y pulsar "Cerrar" a tiempo antes del siguiente refresco.
  if firma == firma_anterior and not forzar then return end
  firma_anterior = firma

  gui.lista_limpiar(lista)
  tareas_actuales = {}
  for _, t in ipairs(tareas) do
    local marca = (t.ventana >= 0) and "" or " (consola)"
    gui.lista_anadir(lista, string.format("[%d] %s -- %d turnos%s", t.slot, t.nombre, t.turnos, marca))
    tareas_actuales[#tareas_actuales] = t.slot   -- indice 0-based, igual que lista_seleccionado()
  end
  gui.poner_texto(etiqueta, "Tareas vivas: " .. #tareas)
end

local temporizador = gui.crear_temporizador(1) -- 1 Hz: comprueba cambios cada segundo
actualizar(true)

gui.bucle(function(ev, fuente)
  if ev == gui.EVENT_GADGETACTION then
    if fuente == boton_actualizar then
      actualizar(true)
    elseif fuente == boton_cerrar then
      local sel = gui.lista_seleccionado(lista)
      if sel >= 0 and tareas_actuales[sel] then
        sistema.matar_tarea(tareas_actuales[sel])
        actualizar(true)
      end
    end
  elseif ev == gui.EVENT_TIMERTICK then
    actualizar(false)
  end
end)
