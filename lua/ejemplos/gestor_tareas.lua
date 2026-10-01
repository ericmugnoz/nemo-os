-- gestor_tareas.lua -- lista las tareas vivas y permite cerrar
-- cualquiera (util para una que se haya quedado bloqueada). Se
-- actualiza sola cada segundo con un temporizador.
local gui = require("nemo_gui")

gui.crear_ventana("Gestor de tareas", 120, 80, 360, 320)
local sistema = require("nemo_sistema")

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
  -- BUG REAL CORREGIDO: esto era
  -- 'tareas_actuales[#tareas_actuales] = t.slot'. El operador # de Lua
  -- no cuenta el indice 0, asi que valia 0 en todas las vueltas y solo
  -- sobrevivia la ultima tarea. Efecto: "Cerrar tarea seleccionada"
  -- mataba la ULTIMA de la lista con la primera fila elegida, y no
  -- hacia nada con cualquier otra fila. En un boton que mata procesos,
  -- eso es lo peor que puede pasar: actua, pero sobre otra cosa.
  -- El indice va a mano, 0-based como lista_seleccionado().
  local i = 0
  for _, t in ipairs(tareas) do
    local marca = (t.ventana >= 0) and "" or " (consola)"
    gui.lista_anadir(lista, string.format("[%d] %s -- %d turnos%s", t.slot, t.nombre, t.turnos, marca))
    -- Se guarda tambien el NOMBRE, no solo el hueco: antes de matar
    -- nada se comprueba que el hueco siga siendo de ese programa (ver
    -- el boton "Cerrar"). Un boton que mata procesos no debe fiarse de
    -- que una tabla de indices siga cuadrando con lo que hay en
    -- pantalla; si no cuadra, mejor no hacer nada que matar otra cosa.
    tareas_actuales[i] = { hueco = t.slot, nombre = t.nombre }
    i = i + 1
  end
  gui.poner_texto(etiqueta, "Tareas vivas: " .. #tareas)
end

-- Cerrar la tarea marcada, comprobando ANTES que sigue siendo la que
-- se ve en esa fila. La lista se rehace sola cada segundo, y entre que
-- eliges una fila y pulsas el boton el sistema puede haber cambiado:
-- una tarea que termina sola libera su hueco, y ese mismo hueco lo
-- puede ocupar otro programa. Sin esta comprobacion, el gestor mataria
-- al inquilino nuevo del hueco viejo -- y el usuario veria justo lo
-- que teme de un boton asi: que cierra algo que el no eligio.
local function cerrar_seleccionada()
  local sel = gui.lista_seleccionado(lista)
  local elegida = (sel >= 0) and tareas_actuales[sel] or nil
  if not elegida then
    gui.poner_texto(etiqueta, "Elige antes una tarea de la lista")
    return
  end
  -- ¿sigue existiendo ese hueco, y con el mismo programa dentro?
  local vive = nil
  for _, t in ipairs(sistema.listar_tareas()) do
    if t.slot == elegida.hueco then vive = t.nombre end
  end
  if vive ~= elegida.nombre then
    actualizar(true)
    gui.poner_texto(etiqueta, "La lista ha cambiado: vuelve a elegir")
    return
  end
  sistema.matar_tarea(elegida.hueco)
  actualizar(true)
end

local temporizador = gui.crear_temporizador(1) -- 1 Hz: comprueba cambios cada segundo
actualizar(true)

-- La ventana ha cambiado de tamaño: la lista crece con ella
-- y los botones se quedan abajo, a todo lo ancho.
local function colocar(w, h)
  gui.limpiar(gui.COLOR_VENTANA)
  gui.mover_gadget(etiqueta, 10, 10, w - 20, 20)
  gui.mover_gadget(lista, 10, 35, w - 20, math.max(60, h - 35 - 85))
  gui.mover_gadget(boton_cerrar, 10, h - 75, w - 20, 32)
  gui.mover_gadget(boton_actualizar, 10, h - 35, w - 20, 26)
end

gui.bucle(function(ev, fuente)
  local cambia, w, h = gui.tamano_cambiado()
  if cambia and w and w >= 100 then colocar(w, h) end
  if ev == gui.EVENT_GADGETACTION then
    if fuente == boton_actualizar then
      actualizar(true)
    elseif fuente == boton_cerrar then
      cerrar_seleccionada()
    end
  elseif ev == gui.EVENT_TIMERTICK then
    actualizar(false)
  end
end)
