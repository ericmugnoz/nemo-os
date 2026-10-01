-- aronnax.lua -- Aronnax, el diseñador visual de Nemo Basic.
--
-- Por el profesor Pierre Aronnax de "Veinte mil leguas de viaje
-- submarino", el que observa, dibuja y cataloga lo que ve por los
-- ventanales del Nautilus.
--
-- Se diseña la ventana de un programa colocando controles con el ratón;
-- al guardar, junto al proyecto (.anx) se escribe el programa de Nemo
-- Basic generado (.nb), listo para compilar. Los controles del Nautilus
-- (LED, interruptor, pulsador, PWM, analogico) estan conectados a los pines.
--
--   paleta: elegir un control, y clic en el formulario para colocarlo
--   clic en un control: seleccionarlo; arrastrarlo: moverlo
--   el asa de su esquina: cambiar su tamaño; la del formulario: el de la ventana
--   flechas: mover el seleccionado; Supr: borrarlo
--   inspector: clic en una propiedad para cambiarla (Enter acepta, Esc cancela)
--   doble clic en un control (o en su fila "Evento"): su funcion, en el codigo
--   pestañas Diseño / Codigo; "Ver programa": el programa entero que se genera
--   Ejecutar: guarda, compila y lo lanza; si hay un error, lleva a tu linea
--   Ctrl (o Cmd) + Z / Y: deshacer / rehacer; + C / X / V / D: copiar, cortar,
--   pegar y duplicar el control elegido
local gui = require("nemo_gui")
local fs = require("nemo_archivos")
local A = require("aronnax_proyecto")
-- Una biblioteca antigua (una copia suelta de otra version)
-- hacia fallar a Aronnax en una linea cualquiera; ahora se dice claramente.
if (A.VERSION or 0) < 3 then
  local de = debug and debug.getinfo and debug.getinfo(A.nuevo, "S").source or "?"
  error("aronnax_proyecto.lua es de una version anterior (" .. tostring(A.VERSION or 1) .. ", hace falta la 3), cargada de " .. de ..
        ". Borra esa copia: la buena esta en SISTEMA.")
end

local ANCHO, ALTO = 980, 620
gui.crear_ventana("Aronnax", 40, 30, ANCHO, ALTO)
gui.usar_fuente("mono", 10)
local ALTO_MONO = gui.alto_fuente()
local AVANCE = 6                     -- la mono de 10 avanza 6 px por caracter
gui.usar_fuente("sans", 12)
local ALTO_LETRA = gui.alto_fuente()

-- Zonas de la ventana
local BARRA_H, PALETA_W, INSP_W, ESTADO_H = 30, 140, 230, 20
local FILA = 20                      -- alto de una fila de la paleta y del inspector
local REJILLA = 8
local TITULO_H = 22                  -- barra de titulo del formulario (dibujada)

-- Colores
local FONDO = gui.rgb(60, 66, 76)
local PANEL = gui.rgb(210, 208, 200)
local TINTA = gui.rgb(20, 22, 28)
local TENUE = gui.rgb(110, 112, 120)
local ACENTO = gui.rgb(40, 110, 200)
local FORM = gui.rgb(212, 208, 200)
local PUNTO = gui.rgb(150, 150, 150)
local NAUTILUS = gui.rgb(30, 120, 110)
local TELEMETRIA = gui.rgb(150, 110, 40)   -- la seccion de red, como NAUTILUS es la del GPIO

-- Estado
local p = A.nuevo()
local sel = nil                      -- control seleccionado (nil = la ventana)
local colocar = nil                  -- tipo elegido en la paleta, esperando un clic en el formulario
local arrastre = nil                 -- { modo = "mover"|"tamano"|"ventana", dx, dy }
local editando = nil                 -- { prop = ..., texto = "..." }
local archivo = nil                  -- { nombre, carpeta } del proyecto abierto o guardado
local mensaje = "Elige un control en la paleta y haz clic en el formulario."
local modo = "diseno"                -- "diseno" o "codigo"
local ver_programa = false           -- en el codigo: ver el programa entero (solo lectura)
local ultimo_clic = { control = nil, t = -1000 }   -- para el doble clic
local DOBLE_CLIC = 50                -- latidos de 10 ms: medio segundo, como el escritorio
local menu_sel = nil                 -- fila elegida en la seccion Menus: { m = menu, e = entrada o nil }
local imagenes_barra = {}            -- tiras de iconos ya cargadas (para dibujar la barra en el formulario)
-- Deshacer / rehacer: antes de cada cambio se guarda una FOTO
-- del proyecto entero (el mismo texto del .anx, que es pequeño). Asi sirve
-- para todo -- colocar, mover, el inspector, los menus, el codigo -- sin
-- programar la operacion contraria de cada cosa.
local historial, rehechos = {}, {}
local MAX_HISTORIAL = 100
local portapapeles = nil             -- un control copiado (Ctrl+C)
local compilando = nil               -- { linea = "", inicio = n, pro = ..., carpeta = ..., desde = ticks }
-- Despues de lanzar el programa se sigue escuchando su salida: un error en
-- tiempo de ejecucion ("Error en tiempo de ejecucion, linea 23: ...") lleva
-- tambien a tu linea
local ejecutando = nil               -- { inicio, linea }
local eligiendo_plantilla = false    -- la lista de plantillas, sobre el formulario
-- Seleccion multiple: 'sel' sigue siendo el control principal (el
-- del inspector) y 'varios' son todos los elegidos, el principal incluido.
local varios = {}
local banda = nil                    -- el recuadro que se arrastra sobre el fondo

-- ---- Seleccion multiple ----
local function esta_elegido(c)
  for _, x in ipairs(varios) do if x == c then return true end end
  return false
end
local function elegir(c, anadir)
  if not c then varios = {}; sel = nil; return end
  if anadir and esta_elegido(c) then
    for i, x in ipairs(varios) do if x == c then table.remove(varios, i) break end end
    sel = varios[#varios]
  else
    if not anadir then varios = {} end
    varios[#varios + 1] = c
    sel = c
  end
end
local function mayus_pulsada() return gui.tecla_pulsada(42) or gui.tecla_pulsada(54) end

local ancho_v, alto_v = ANCHO, ALTO

local function estado(t) mensaje = t end

-- ---------------------------------------------------------------------
-- Geometria
-- ---------------------------------------------------------------------
local function origen_form()        -- donde empieza el CONTENIDO de la ventana diseñada
  -- (la barra de menus la dibuja el kernel entre el titulo y el contenido,
  -- y los controles se colocan debajo, igual que aqui)
  return PALETA_W + 24, BARRA_H + 24 + TITULO_H + ((#p.menus > 0) and A.ALTO_MENUS or 0)
end
local function ajustar(v) return math.floor((v + REJILLA // 2) / REJILLA) * REJILLA end
local function dentro(px, py, x, y, w, h) return px >= x and py >= y and px < x + w and py < y + h end

-- ---------------------------------------------------------------------
-- Dibujo de los controles (simulados: se pueden arrastrar)
-- ---------------------------------------------------------------------
local function texto_centrado_v(x, y, h, t, color) gui.texto(x, y + (h - ALTO_LETRA) // 2, t, color) end

-- Las imagenes de los controles Imagen, cargadas una vez: img, ancho, alto
-- (o nil si no se encuentra: el programa las buscara en la raiz, en DOCUMENTOS
-- y en DOCUMENTOS/IMAGENES, igual que aqui gui.cargar_imagen)
local imagenes = {}
local function imagen_de(nombre)
  if not nombre or nombre == "" then return nil end
  local e = imagenes[nombre]
  if e == nil then
    local img = gui.cargar_imagen(nombre)
    if img then local w, h = gui.tamano_imagen(img); e = { img, w or 32, h or 32 } else e = false end
    imagenes[nombre] = e
  end
  if e then return e[1], e[2], e[3] end
  return nil
end

local function dibujar_control(c, ox, oy)
  local x, y, w, h = ox + c.x, oy + c.y, c.ancho, c.alto
  local t = c.tipo
  if t == "Boton" then
    gui.rect(x, y, w, h, gui.rgb(80, 88, 96)); texto_centrado_v(x + 6, y, h, c.texto or "", gui.rgb(255, 255, 255))
  elseif t == "Etiqueta" then
    texto_centrado_v(x, y, h, c.texto or "", TINTA)
  elseif t == "Campo" then
    gui.rect(x, y, w, h, gui.rgb(24, 28, 32)); gui.rect(x, y + h - 1, w, 1, gui.rgb(120, 120, 120))
  elseif t == "Casilla" or t == "Opcion" or t == "Interruptor" then
    local by = y + h // 2 - 6
    gui.rect(x, by, 12, 12, gui.rgb(160, 160, 160)); gui.rect(x + 1, by + 1, 10, 10, gui.rgb(24, 28, 32))
    texto_centrado_v(x + 18, y, h, c.texto or "", t == "Interruptor" and NAUTILUS or TINTA)
  elseif t == "Lista" then
    gui.rect(x, y, w, h, gui.rgb(24, 28, 32))
    for i = 0, 2 do if 4 + i * 18 + 14 < h then gui.rect(x + 6, y + 8 + i * 18, math.max(4, w // 2), 3, gui.rgb(110, 116, 124)) end end
  elseif t == "Desplegable" then
    gui.rect(x, y, w, h, gui.rgb(24, 28, 32)); texto_centrado_v(x + w - 12, y, h, "v", gui.rgb(160, 160, 160))
  elseif t == "Pestanas" then
    gui.rect(x, y, w, h, gui.rgb(190, 188, 180))
    for i = 0, 2 do if i * 60 + 56 <= w then gui.rect(x + i * 60, y, 56, math.min(h, 22), i == 0 and gui.rgb(230, 228, 220) or gui.rgb(170, 168, 160)) end end
  elseif t == "Deslizador" or t == "Pwm" then
    gui.rect(x, y + h // 2 - 2, w, 4, gui.rgb(100, 100, 108)); gui.rect(x + w // 2 - 4, y, 8, h, t == "Pwm" and NAUTILUS or gui.rgb(80, 88, 96))
    if t == "Pwm" then                 -- el rotulo, DENTRO del control: fuera pisaba a los vecinos
      local r = "PWM " .. tostring(c.pin)
      gui.rect(x, y + (h - ALTO_LETRA) // 2, gui.medir_texto(r) + 6, ALTO_LETRA, FORM)
      gui.texto(x + 2, y + (h - ALTO_LETRA) // 2, r, NAUTILUS)
    end
  elseif t == "Progreso" or t == "Analogico" then
    gui.rect(x, y, w, h, gui.rgb(24, 28, 32)); gui.rect(x + 1, y + 1, (w - 2) // 2, h - 2, t == "Analogico" and NAUTILUS or ACENTO)
    if t == "Analogico" then gui.texto(x + 4, y + (h - ALTO_LETRA) // 2, "canal " .. tostring(c.canal), gui.rgb(230, 240, 240)) end
  elseif t == "Texto" then
    gui.rect(x, y, w, h, gui.rgb(24, 28, 32)); gui.texto(x + 4, y + 4, "texto...", gui.rgb(140, 140, 140))
  elseif t == "Panel" then
    gui.rect(x, y, w, h, gui.rgb(180, 178, 170)); gui.rect(x, y, w, 1, gui.rgb(140, 140, 140))
  elseif t == "Arbol" then
    gui.rect(x, y, w, h, gui.rgb(24, 28, 32))
    local filas = { { 0, "- Raíz" }, { 14, "Rama" }, { 14, "+ Otra rama" }, { 0, "+ Más" } }
    for i, f in ipairs(filas) do
      local fy = y + 4 + (i - 1) * (ALTO_LETRA + 2)
      if fy + ALTO_LETRA <= y + h then gui.texto(x + 6 + f[1], fy, f[2], gui.rgb(200, 204, 210)) end
    end
  elseif t == "Lienzo" then
    gui.rect(x, y, w, h, gui.rgb(250, 250, 246)); gui.rect(x, y, w, 1, gui.rgb(150, 150, 150)); gui.rect(x, y + h - 1, w, 1, gui.rgb(150, 150, 150))
    gui.rect(x, y, 1, h, gui.rgb(150, 150, 150)); gui.rect(x + w - 1, y, 1, h, gui.rgb(150, 150, 150))
    gui.linea(x + 6, y + h - 8, x + w // 2, y + 8, ACENTO); gui.linea(x + w // 2, y + 8, x + w - 6, y + h - 8, ACENTO)
    gui.texto(x + 6, y + 4, "lienzo", TENUE)
  elseif t == "Imagen" then
    local img, iw, ih = imagen_de(c.imagen)
    if img then
      -- como la dibuja el kernel: en mosaico si el control es mas grande
      -- (aqui se ven los azulejos enteros; el kernel recorta el ultimo)
      for ty = 0, h - 1, ih do for tx = 0, w - 1, iw do gui.dibujar_imagen(img, x + tx, y + ty) end end
    else
      gui.rect(x, y, w, h, gui.rgb(150, 150, 160)); gui.rect(x + 1, y + 1, w - 2, h - 2, gui.rgb(220, 220, 228))
      gui.texto(x + 3, y + 2, "?", TENUE)
    end
  elseif t == "Led" or t == "Pulsador" then
    gui.rect(x, y, w, h, t == "Led" and gui.rgb(60, 200, 90) or gui.rgb(240, 190, 40))
    local r = tostring(c.pin)            -- el numero del pin, dentro del cuadrado
    gui.texto(x + (w - gui.medir_texto(r)) // 2, y + (h - ALTO_LETRA) // 2, r, TINTA)
  elseif t == "Sensor" or t == "Mensaje" then
    -- En el programa los dos son una etiqueta que dice como va. Aqui se
    -- dibujan con su destino escrito, que es el dato que hay que ver de
    -- un vistazo al mirar el formulario: a donde manda, o por donde
    -- escucha.
    gui.rect(x, y, w, h, gui.rgb(40, 34, 22))
    gui.rect(x, y, 3, h, TELEMETRIA)     -- la banda del lado, como marca de la seccion
    local r
    if t == "Sensor" then
      r = tostring(c.ip or "?") .. ":" .. tostring(c.puerto or 0) .. tostring(c.ruta or "")
    else
      r = "escucha en el " .. tostring(c.puerto or 0)
    end
    gui.texto(x + 6, y + (h - ALTO_LETRA) // 2, r, gui.rgb(230, 200, 140))
  end
end

-- ---------------------------------------------------------------------
-- El codigo (fase 3)
-- ---------------------------------------------------------------------
-- Mientras se esta en la pestaña Codigo, el editor ('ed') es quien manda;
-- p.codigo se pone al dia (sincronizar) al volver al diseño, al guardar,
-- al ver el programa y antes de tocar el codigo desde fuera.
local ed = { lineas = { "" }, fila = 1, col = 0, arriba = 1, arriba_prog = 1, error_fila = nil }
local GUTTER = 44

local function cargar_codigo()
  ed.lineas = {}
  for l in (p.codigo .. "\n"):gmatch("(.-)\n") do ed.lineas[#ed.lineas + 1] = l end
  if #ed.lineas > 1 and ed.lineas[#ed.lineas] == "" then table.remove(ed.lineas) end
  if #ed.lineas == 0 then ed.lineas = { "" } end
  if ed.fila > #ed.lineas then ed.fila = #ed.lineas end
  if ed.col > #ed.lineas[ed.fila] then ed.col = #ed.lineas[ed.fila] end
end

local function sincronizar()
  if modo ~= "codigo" then return end
  if #ed.lineas == 1 and ed.lineas[1] == "" then p.codigo = ""
  else p.codigo = table.concat(ed.lineas, "\n") .. "\n" end
end

-- La primera linea del codigo del usuario que usa un control: su nombre
-- suelto (SetGadgetText Etiqueta4, ...) o como prefijo (Led1_Pon). 0 si ninguna.
local function linea_que_usa(nombre)
  local n = 0
  for l in (p.codigo .. "\n"):gmatch("(.-)\n") do
    n = n + 1
    local sin_comentario = l:gsub(";.*$", "")
    if sin_comentario:find("%f[%w_]" .. nombre .. "%f[^%w_]") or sin_comentario:find("%f[%w_]" .. nombre .. "_") then return n end
  end
  return 0
end

-- Llevar al codigo de un control: su funcion de evento, creada si no existe.
-- Los que no tienen eventos (etiquetas, LED, progreso...) llevan a donde el
-- codigo los usa; si aun no se usan, se queda en el diseño y lo explica
-- (antes cambiaba al codigo sin mover el cursor, y parecia que
-- llevaba a la funcion de otro control).
local function ir_a_codigo(c)
  sincronizar()
  if not A.funcion_evento(c) then
    local n = linea_que_usa(c.nombre)
    if n == 0 then
      estado(c.nombre .. " no tiene eventos, y tu código aún no lo usa. Se cambia desde otras funciones: " ..
             ((c.tipo == "Led") and (c.nombre .. "_Pon(1)") or ("SetGadgetText " .. c.nombre .. ", ...")))
      return
    end
    modo = "codigo"; ver_programa = false; editando = nil; colocar = nil; arrastre = nil
    cargar_codigo()
    ed.fila = math.min(n, #ed.lineas); ed.col = #ed.lineas[ed.fila]
    estado(c.nombre .. " no tiene eventos: aquí es donde tu código lo usa (línea " .. n .. ").")
    return
  end
  local linea = A.asegurar_funcion(p, c)
  modo = "codigo"; ver_programa = false; editando = nil; colocar = nil; arrastre = nil
  cargar_codigo()
  ed.fila = math.min(linea, #ed.lineas); ed.col = #ed.lineas[ed.fila]
  estado("Escribe lo que hace " .. A.funcion_evento(c) .. ". Diseño, para volver al formulario.")
end

-- ---- El historial ----
local function foto()
  sincronizar()
  return { texto = A.escribir(p), sel = sel and sel.nombre or nil, fila = ed.fila, col = ed.col }
end
-- Llamar ANTES de cambiar algo
local function recordar()
  historial[#historial + 1] = foto()
  if #historial > MAX_HISTORIAL then table.remove(historial, 1) end
  rehechos = {}
end
local function volver_a(f)
  local q = A.leer(f.texto)
  if not q then return end
  p = q
  sel = nil; varios = {}
  if f.sel then for _, c in ipairs(p.controles) do if c.nombre == f.sel then sel = c; varios = { c } end end end
  editando, colocar, arrastre, menu_sel = nil, nil, nil, nil
  if modo == "codigo" then cargar_codigo(); ed.fila = math.min(f.fila or 1, #ed.lineas); ed.col = math.min(f.col or 0, #ed.lineas[ed.fila]) end
end
local function deshacer()
  if #historial == 0 then estado("No hay nada que deshacer.") return end
  rehechos[#rehechos + 1] = foto()
  volver_a(table.remove(historial))
  estado("Deshecho. (Ctrl+Y para rehacer; quedan " .. #historial .. ")")
end
local function rehacer()
  if #rehechos == 0 then estado("No hay nada que rehacer.") return end
  historial[#historial + 1] = foto()
  volver_a(table.remove(rehechos))
  estado("Rehecho.")
end
-- ¿Hay cambios sin guardar? Se compara con el proyecto tal como se guardo
-- o se abrio por ultima vez (texto del .anx).
local guardado_como = A.escribir(p)
local confirmar = nil                -- "Nuevo", "Abrir" o "Salir": pendiente de un segundo clic
local function marcar_guardado() sincronizar(); guardado_como = A.escribir(p) end
local function hay_cambios() sincronizar(); return A.escribir(p) ~= guardado_como end
-- true si se puede seguir; si hay cambios, la primera vez avisa y pide repetir
local function puede_descartar(accion, como_repetir)
  if not hay_cambios() or confirmar == accion then confirmar = nil; return true end
  confirmar = accion
  estado("Hay cambios sin guardar. " .. como_repetir .. " para descartarlos, o pulsa Guardar.")
  return false
end

-- Escribir seguido en una linea del codigo cuenta como UN paso, como en
-- cualquier editor: solo se guarda la foto al empezar
local escribiendo = { fila = -1, t = -1000 }
local function recordar_escritura()
  local ahora = nemo.ticks()
  if escribiendo.fila ~= ed.fila or ahora - escribiendo.t > 100 then recordar() end
  escribiendo.fila, escribiendo.t = ed.fila, ahora
end

-- Llevar a una funcion que no es de un control (menus, barra, temporizador)
local function ir_a_funcion(nombre, parametro)
  sincronizar()
  local linea = A.asegurar_funcion_nombre(p, nombre, parametro)
  modo = "codigo"; ver_programa = false; editando = nil; colocar = nil; arrastre = nil
  cargar_codigo()
  ed.fila = math.min(linea, #ed.lineas); ed.col = #ed.lineas[ed.fila]
  estado("Escribe lo que hace " .. nombre .. ". Diseño, para volver al formulario.")
end

-- Renombrar un control tambien en el codigo: sus funciones (Boton1_Click)
-- y donde se usa (SetGadgetText Boton1, ...). Como hace Delphi.
local function renombrar_en_codigo(viejo, nuevo)
  local t = p.codigo:gsub("%f[%w_]" .. viejo .. "_", nuevo .. "_")
  t = t:gsub("%f[%w_]" .. viejo .. "%f[^%w_]", nuevo)
  p.codigo = t
end

-- Colores
local C_TEXTO, C_CLAVE, C_CADENA, C_COMENT, C_NUM = gui.rgb(220, 222, 228), gui.rgb(110, 170, 255),
      gui.rgb(230, 180, 110), gui.rgb(120, 150, 120), gui.rgb(200, 140, 230)
local C_FONDO_COD, C_GENERADO = gui.rgb(26, 30, 36), gui.rgb(40, 44, 52)
local CLAVES = {}
for w in ("if then else elseif endif end function for to step next while wend repeat until forever " ..
          "global local dim return and or not xor mod select case default exit goto gosub type field new " ..
          "delete each const true false null data read restore"):gmatch("%S+") do CLAVES[w] = true end

local function columnas(t) return utf8.len(t) or #t end    -- caracteres (con tildes, no bytes)

-- Una linea con colores, en (x, y)
local function dibujar_linea(x, y, linea)
  local i, n = 1, #linea
  local function pinta(desde, hasta, color)
    gui.texto(x + columnas(linea:sub(1, desde - 1)) * AVANCE, y, linea:sub(desde, hasta), color)
  end
  while i <= n do
    local c = linea:sub(i, i)
    if c == ";" then pinta(i, n, C_COMENT); return
    elseif c == '"' then
      local j = linea:find('"', i + 1, true) or n
      pinta(i, j, C_CADENA); i = j + 1
    elseif c:match("[%a_]") then
      local j = linea:find("[^%w_$#%%]", i) or (n + 1)
      local palabra = linea:sub(i, j - 1)
      pinta(i, j - 1, CLAVES[palabra:lower()] and C_CLAVE or C_TEXTO); i = j
    elseif c:match("%d") then
      local j = linea:find("[^%w%.]", i) or (n + 1)
      pinta(i, j - 1, C_NUM); i = j
    else
      local j = linea:find('[;"%w_]', i + 1) or (n + 1)
      pinta(i, j - 1, C_TEXTO); i = j
    end
  end
end

local function zona_codigo() return 0, BARRA_H, ancho_v, alto_v - BARRA_H - ESTADO_H end
local function alto_linea() return ALTO_MONO + 2 end
local function visibles() local _, _, _, h = zona_codigo(); return math.max(1, (h - 8) // alto_linea()) end

local function asegurar_visible()
  local v = visibles()
  if ed.fila < ed.arriba then ed.arriba = ed.fila end
  if ed.fila >= ed.arriba + v then ed.arriba = ed.fila - v + 1 end
  if ed.arriba < 1 then ed.arriba = 1 end
end

-- ---- Ayuda mientras escribes ----
-- Con dos letras o mas, una lista de nombres que encajan (tus controles, tus
-- funciones y las ordenes de Nemo Basic): flechas para elegir, Tab o Intro
-- para completar, Esc para cerrar. Y en la linea de estado, la firma de la
-- orden que estas escribiendo.
local sugerencias = nil        -- { lista = {...}, elegida = n, desde = col }
local function palabra_antes(l, col)
  local i = col
  while i > 0 and l:sub(i, i):match("[%w_$#]") do i = i - 1 end
  return l:sub(i + 1, col), i
end
local function cerrar_sugerencias() sugerencias = nil end
local function refrescar_sugerencias()
  if modo ~= "codigo" or ver_programa then cerrar_sugerencias() return end
  local l = ed.lineas[ed.fila]
  local pal, desde = palabra_antes(l, ed.col)
  if #pal < 2 then cerrar_sugerencias() return end
  local lista = A.sugerencias(p, pal, 8)
  -- si lo unico que encaja es lo ya escrito, no molesta
  if #lista == 0 or (#lista == 1 and lista[1][1]:lower() == pal:lower()) then cerrar_sugerencias() return end
  sugerencias = { lista = lista, elegida = 1, desde = desde }
end
local function aplicar_sugerencia()
  if not sugerencias then return false end
  local nombre = sugerencias.lista[sugerencias.elegida][1]
  local l = ed.lineas[ed.fila]
  ed.lineas[ed.fila] = l:sub(1, sugerencias.desde) .. nombre .. l:sub(ed.col + 1)
  ed.col = sugerencias.desde + #nombre
  local n, args = A.firma(nombre)
  estado(n and (n .. " " .. args) or (nombre .. " puesto."))
  cerrar_sugerencias()
  return true
end
-- La orden que se esta escribiendo en esta linea, para enseñar su firma
local function firma_en_curso()
  local l = ed.lineas[ed.fila]:sub(1, ed.col)
  local nombre = l:match("([%w_$#]+)%s*%(?[^()]*$")
  if not nombre then return nil end
  return A.firma(nombre)
end

local function dibujar_codigo()
  local x0, y0, w, h = zona_codigo()
  gui.rect(x0, y0, w, h, C_FONDO_COD)
  gui.rect(x0, y0, GUTTER - 6, h, gui.rgb(34, 38, 46))
  gui.usar_fuente("mono", 10)
  local lh = alto_linea()
  if ver_programa then
    sincronizar()
    local prog, inicio = A.generar(p)
    local lineas = {}
    for l in prog:gmatch("(.-)\n") do lineas[#lineas + 1] = l end
    local v = visibles()
    ed.arriba_prog = math.max(1, math.min(ed.arriba_prog, math.max(1, #lineas - v + 1)))
    for k = 0, v - 1 do
      local n = ed.arriba_prog + k
      if n > #lineas then break end
      local y = y0 + 4 + k * lh
      if n < inicio then gui.rect(GUTTER - 4, y, w - GUTTER + 4, lh, C_GENERADO) end
      gui.texto(4, y, string.format("%4d", n), gui.rgb(100, 106, 116))
      dibujar_linea(GUTTER, y, lineas[n])
    end
  else
    asegurar_visible()
    for k = 0, visibles() - 1 do
      local n = ed.arriba + k
      if n > #ed.lineas then break end
      local y = y0 + 4 + k * lh
      if n == ed.error_fila then gui.rect(GUTTER - 4, y, w - GUTTER + 4, lh, gui.rgb(110, 30, 34))
      elseif n == ed.fila then gui.rect(GUTTER - 4, y, w - GUTTER + 4, lh, gui.rgb(34, 40, 50)) end
      gui.texto(4, y, string.format("%4d", n), n == ed.fila and gui.rgb(180, 186, 196) or gui.rgb(100, 106, 116))
      dibujar_linea(GUTTER, y, ed.lineas[n])
    end
    local cy = y0 + 4 + (ed.fila - ed.arriba) * lh
    local cx = GUTTER + columnas(ed.lineas[ed.fila]:sub(1, ed.col)) * AVANCE
    gui.rect(cx, cy, 2, lh, gui.rgb(255, 255, 255))
    if sugerencias then                     -- la lista, bajo el cursor
      local n = #sugerencias.lista
      local ancho = 150
      for _, it in ipairs(sugerencias.lista) do
        local a = (#it[1] + #(it[2] or "") + 3) * AVANCE + 16
        if a > ancho then ancho = a end
      end
      if ancho > w - 20 then ancho = w - 20 end
      local sx = math.min(cx, x0 + w - ancho - 4)
      local sy = cy + lh
      if sy + n * lh + 4 > y0 + h then sy = math.max(y0, cy - n * lh - 4) end
      gui.rect(sx, sy, ancho, n * lh + 4, gui.rgb(46, 52, 62))
      gui.rect(sx, sy, ancho, 1, gui.rgb(120, 128, 140))
      for i, it in ipairs(sugerencias.lista) do
        local iy = sy + 2 + (i - 1) * lh
        if i == sugerencias.elegida then gui.rect(sx + 1, iy, ancho - 2, lh, gui.rgb(70, 92, 130)) end
        gui.texto(sx + 6, iy, it[1], gui.rgb(236, 238, 242))
        if it[2] then gui.texto(sx + 8 + (#it[1] + 1) * AVANCE, iy, it[2], gui.rgb(150, 158, 170)) end
      end
    end
  end
  gui.usar_fuente("sans", 12)
end

-- Mover el cursor sin partir letras de varios bytes
local function atras(l, c) c = c - 1; while c > 0 and (l:byte(c + 1) & 0xC0) == 0x80 do c = c - 1 end; return math.max(0, c) end
local function adelante(l, c) c = c + 1; while c < #l and (l:byte(c + 1) & 0xC0) == 0x80 do c = c + 1 end; return math.min(#l, c) end
local function ajustar_col()
  local l = ed.lineas[ed.fila]
  if ed.col > #l then ed.col = #l end
  while ed.col > 0 and ed.col < #l and (l:byte(ed.col + 1) & 0xC0) == 0x80 do ed.col = ed.col - 1 end
end

local function editor_caracter(ch)
  if ver_programa then return false end
  if sugerencias and (ch == 9 or ch == 10 or ch == 13) then return aplicar_sugerencia() end
  if ch == 27 then cerrar_sugerencias() return true end
  if ch == 10 or ch == 13 or ch == 8 or ch == 127 or ch == 9 or ch >= 32 then recordar_escritura() end
  ed.error_fila = nil                -- al editar, la marca del error se quita
  local l = ed.lineas[ed.fila]
  if ch == 10 or ch == 13 then
    local sangria = l:match("^(%s*)")
    ed.lineas[ed.fila] = l:sub(1, ed.col)
    table.insert(ed.lineas, ed.fila + 1, sangria .. l:sub(ed.col + 1))
    ed.fila = ed.fila + 1; ed.col = #sangria
  elseif ch == 8 or ch == 127 then
    if ed.col > 0 then
      local c = atras(l, ed.col)
      ed.lineas[ed.fila] = l:sub(1, c) .. l:sub(ed.col + 1); ed.col = c
    elseif ed.fila > 1 then
      local prev = ed.lineas[ed.fila - 1]
      ed.lineas[ed.fila - 1] = prev .. l; table.remove(ed.lineas, ed.fila)
      ed.fila = ed.fila - 1; ed.col = #prev
    end
  elseif ch == 9 then
    ed.lineas[ed.fila] = l:sub(1, ed.col) .. "  " .. l:sub(ed.col + 1); ed.col = ed.col + 2
  elseif ch >= 32 then
    ed.lineas[ed.fila] = l:sub(1, ed.col) .. string.char(ch) .. l:sub(ed.col + 1); ed.col = ed.col + 1
  else return false end
  refrescar_sugerencias()
  local n, args = firma_en_curso()
  if n and not sugerencias then estado(n .. " " .. args) end
  return true
end

local TECLA_INICIO, TECLA_FIN, TECLA_REPAG, TECLA_AVPAG = 102, 107, 104, 109
local function editor_codigo(k)
  local v = visibles()
  -- Con la lista de sugerencias abierta, las flechas arriba y abajo eligen en
  -- ella. Solo la cierran las teclas que de verdad la cancelan:
  -- al escribir una letra llega TAMBIEN su codigo de tecla, y cerrarla con
  -- cualquier codigo hacia que la lista se abriera y se cerrara al instante
  -- (en el sistema real no se veia nunca).
  if sugerencias and not ver_programa then
    if k == 103 then sugerencias.elegida = (sugerencias.elegida - 2) % #sugerencias.lista + 1; return true end
    if k == 108 then sugerencias.elegida = sugerencias.elegida % #sugerencias.lista + 1; return true end
    if k == 105 or k == 106 or k == TECLA_INICIO or k == TECLA_FIN or
       k == TECLA_REPAG or k == TECLA_AVPAG or k == 111 then cerrar_sugerencias() end
  end
  if ver_programa then
    if k == 103 then ed.arriba_prog = ed.arriba_prog - 1 elseif k == 108 then ed.arriba_prog = ed.arriba_prog + 1
    elseif k == TECLA_REPAG then ed.arriba_prog = ed.arriba_prog - v elseif k == TECLA_AVPAG then ed.arriba_prog = ed.arriba_prog + v
    else return false end
    return true
  end
  local l = ed.lineas[ed.fila]
  if k == 105 then            -- izquierda
    if ed.col > 0 then ed.col = atras(l, ed.col) elseif ed.fila > 1 then ed.fila = ed.fila - 1; ed.col = #ed.lineas[ed.fila] end
  elseif k == 106 then        -- derecha
    if ed.col < #l then ed.col = adelante(l, ed.col) elseif ed.fila < #ed.lineas then ed.fila = ed.fila + 1; ed.col = 0 end
  elseif k == 103 then if ed.fila > 1 then ed.fila = ed.fila - 1; ajustar_col() end
  elseif k == 108 then if ed.fila < #ed.lineas then ed.fila = ed.fila + 1; ajustar_col() end
  elseif k == TECLA_INICIO then ed.col = 0
  elseif k == TECLA_FIN then ed.col = #l
  elseif k == TECLA_REPAG then ed.fila = math.max(1, ed.fila - v); ajustar_col()
  elseif k == TECLA_AVPAG then ed.fila = math.min(#ed.lineas, ed.fila + v); ajustar_col()
  elseif k == 111 then        -- Supr
    cerrar_sugerencias()
    recordar_escritura()
    if ed.col < #l then ed.lineas[ed.fila] = l:sub(1, ed.col) .. l:sub(adelante(l, ed.col) + 1)
    elseif ed.fila < #ed.lineas then ed.lineas[ed.fila] = l .. ed.lineas[ed.fila + 1]; table.remove(ed.lineas, ed.fila + 1) end
  else return false end
  return true
end

local function clic_codigo(mx, my)
  if ver_programa then return end
  local x0, y0 = zona_codigo()
  local n = ed.arriba + (my - y0 - 4) // alto_linea()
  ed.fila = math.max(1, math.min(#ed.lineas, n))
  local l = ed.lineas[ed.fila]
  local cols = math.max(0, (mx - x0 - GUTTER + AVANCE // 2) // AVANCE)
  local b = utf8.offset(l, cols + 1)
  ed.col = b and math.min(#l, b - 1) or #l
end

-- ---------------------------------------------------------------------
-- La paleta
-- ---------------------------------------------------------------------
local NOMBRES = { Boton = "Botón", Etiqueta = "Etiqueta", Campo = "Campo de texto", Casilla = "Casilla",
  Opcion = "Opción", Lista = "Lista", Desplegable = "Desplegable", Pestanas = "Pestañas", Deslizador = "Deslizador",
  Progreso = "Progreso", Texto = "Caja de texto", Panel = "Panel", Imagen = "Imagen", Arbol = "Árbol", Lienzo = "Lienzo", Led = "LED", Interruptor = "Interruptor",
  Pulsador = "Pulsador", Pwm = "PWM", Analogico = "Analógico",
  Sensor = "Sensor HTTP", Mensaje = "Mensajes UDP" }

-- filas de la paleta: { tipo = ... } o { titulo = ... }
local function filas_paleta()
  local r = { { titulo = "Controles" } }
  for _, t in ipairs(A.ORDEN) do
    if not A.TIPOS[t].gpio then r[#r + 1] = { tipo = t } end
  end
  r[#r + 1] = { titulo = "Nautilus (GPIO)" }
  for _, t in ipairs(A.ORDEN) do
    if A.TIPOS[t].gpio then r[#r + 1] = { tipo = t } end
  end
  r[#r + 1] = { titulo = "Telemetría (red)" }
  for _, t in ipairs(A.ORDEN) do
    if A.TIPOS[t].red then r[#r + 1] = { tipo = t } end
  end
  return r
end
local PALETA = filas_paleta()

local function dibujar_paleta()
  gui.rect(0, BARRA_H, PALETA_W, alto_v - BARRA_H - ESTADO_H, PANEL)
  for i, f in ipairs(PALETA) do
    local y = BARRA_H + 6 + (i - 1) * FILA
    if f.titulo then
      gui.texto(8, y + 2, f.titulo, f.titulo:find("Nautilus") and NAUTILUS
                or f.titulo:find("Telemetr") and TELEMETRIA or TENUE)
    else
      if colocar == f.tipo then gui.rect(4, y, PALETA_W - 8, FILA, ACENTO) end
      gui.texto(16, y + 2, NOMBRES[f.tipo], colocar == f.tipo and gui.rgb(255, 255, 255) or TINTA)
    end
  end
end

local function clic_paleta(mx, my)
  local i = (my - BARRA_H - 6) // FILA + 1
  local f = PALETA[i]
  if f and f.tipo then
    colocar = (colocar == f.tipo) and nil or f.tipo
    estado(colocar and ("Haz clic en el formulario para colocar: " .. NOMBRES[colocar]) or "")
  end
end

-- ---------------------------------------------------------------------
-- El inspector
-- ---------------------------------------------------------------------
-- Propiedades que se muestran: { clave, rotulo, clase } -- clase: "texto",
-- "numero", "nombre", "si_no", "pin", "canal", "fijo"
local function propiedades()
  if not sel then
    local r = { { "titulo", "Título", "texto" }, { "x", "X", "numero" }, { "y", "Y", "numero" },
                { "ancho", "Ancho", "numero" }, { "alto", "Alto", "numero" }, { "maximizar", "Maximizar", "si_no" },
                { "minimizar", "Minimizar", "si_no" }, { "cerrar", "Cerrar", "si_no" },
                { "barra", "Barra", "barra" }, { "temporizador", "Temporizador", "numero_p" } }
    if p.barra then r[#r + 1] = { "evento_barra", "Evento barra", "fijo" } end
    if (p.temporizador or 0) > 0 then r[#r + 1] = { "evento_temp", "Evento tiempo", "fijo" } end
    return r
  end
  local t = A.TIPOS[sel.tipo]
  local r = { { "tipo", "Tipo", "fijo" }, { "nombre", "Nombre", "nombre" } }
  if t.texto then r[#r + 1] = { "texto", "Texto", "texto" } end
  for _, k in ipairs({ "x", "y", "ancho", "alto" }) do r[#r + 1] = { k, k == "x" and "X" or k == "y" and "Y" or (k:sub(1, 1):upper() .. k:sub(2)), "numero" } end
  if t.gpio and t.gpio ~= "analogico" then r[#r + 1] = { "pin", "Pin GPIO", "pin" } end
  if t.gpio == "analogico" then r[#r + 1] = { "canal", "Canal (MCP3008)", "canal" } end
  -- Telemetria. Todas las propiedades usan clases que ya
  -- existian: la IP y la ruta son texto, el puerto y el periodo numeros.
  if t.red == "http" then
    r[#r + 1] = { "ip", "Servidor (IP)", "texto" }
    r[#r + 1] = { "puerto", "Puerto", "numero" }
    r[#r + 1] = { "ruta", "Ruta", "texto" }
    r[#r + 1] = { "cada", "Cada (ms, 0 = a mano)", "numero" }
  elseif t.red == "udp" then
    r[#r + 1] = { "puerto", "Puerto", "numero" }
  end
  if t.red then r[#r + 1] = { "funciones", "Funciones", "fijo" } end
  -- La caja de texto guarda menos de lo que parece, y no lo dice en
  -- ningun sitio. Se pone aqui, pegado al control, porque es donde se
  -- mira antes de decidir usarla para algo que despues se guarda.
  if sel.tipo == "Texto" then r[#r + 1] = { "limites", "Guarda", "fijo" } end
  if sel.tipo == "Imagen" then r[#r + 1] = { "imagen", "Archivo", "imagen" } end
  r[#r + 1] = { "ancla", "Anclaje", "ancla" }         -- que hace al cambiar el tamaño la ventana
  if t.evento then r[#r + 1] = { "evento", "Evento", "fijo" } end
  return r
end

local function valor(prop)
  local obj = sel or p.ventana
  local k, clase = prop[1], prop[3]
  if k == "tipo" then return NOMBRES[sel.tipo] end
  if k == "evento" then return A.funcion_evento(sel) .. "()" end
  if k == "barra" then return p.barra and p.barra.archivo:gsub("%.nimg$", "") or "ninguna" end
  if k == "temporizador" then return (p.temporizador or 0) > 0 and (p.temporizador .. " por segundo") or "ninguno" end
  if k == "evento_barra" then return "Barra_Click(boton)" end
  if k == "evento_temp" then return "Temporizador_Tick()" end
  -- Telemetria: la lista de lo que puedes llamar y lo que te llaman a
  -- ti. Va aqui, a la vista, porque son nombres que nadie se sabe de
  -- memoria y buscarlos en la guia rompe el hilo.
  if k == "limites" then
    return A.LINEAS_TEXTO .. " lineas x " .. A.LARGO_LINEA_TEXTO ..
           " (las de mas se pierden)"
  end
  if k == "funciones" then
    if sel.tipo == "Sensor" then
      return sel.nombre .. "_Manda(datos$)" ..
             (((tonumber(sel.cada) or 0) > 0) and ("  ·  tu " .. sel.nombre .. "_Valor$()") or "")
    end
    return sel.nombre .. "_Manda / _Grita / _Responde"
  end
  local v = obj[k]
  if k == "ancla" then return v or "fijo" end          -- sin anclaje: fijo
  if clase == "si_no" then return v and "sí" or "no" end
  return tostring(v == nil and "" or v)
end

local INSP_Y0 = BARRA_H + 34
-- El inspector se desplaza con la rueda cuando no cabe: todas
-- sus posiciones salen de insp_y0(), que resta el desplazamiento
local insp_scroll = 0
local function insp_y0() return INSP_Y0 - insp_scroll end

-- La seccion Menus del inspector (solo con la ventana elegida): una fila
-- por menu y por entrada, y debajo los botones
local function filas_menus()
  local r = {}
  for _, m in ipairs(p.menus) do
    r[#r + 1] = { m = m }
    for _, e in ipairs(m.entradas) do
      r[#r + 1] = { m = m, e = e }
      for _, s in ipairs(e.entradas or {}) do r[#r + 1] = { m = m, e = s, madre = e } end   -- su submenu
    end
  end
  return r
end
-- Alinear, igualar y repartir (con dos o mas elegidos)
local function ordenar_por(campo)
  local l = {}
  for _, c in ipairs(varios) do l[#l + 1] = c end
  table.sort(l, function(a, b) return a[campo] < b[campo] end)
  return l
end
local function apanar(que)
  if #varios < 2 then estado("Elige dos o más controles (Mayúsculas + clic, o arrastra sobre el fondo).") return end
  recordar()
  local v = p.ventana
  if que == "izquierda" then
    local x = math.huge; for _, c in ipairs(varios) do x = math.min(x, c.x) end
    for _, c in ipairs(varios) do c.x = x end
  elseif que == "derecha" then
    local r = -math.huge; for _, c in ipairs(varios) do r = math.max(r, c.x + c.ancho) end
    for _, c in ipairs(varios) do c.x = math.max(0, r - c.ancho) end
  elseif que == "arriba" then
    local y = math.huge; for _, c in ipairs(varios) do y = math.min(y, c.y) end
    for _, c in ipairs(varios) do c.y = y end
  elseif que == "abajo" then
    local b = -math.huge; for _, c in ipairs(varios) do b = math.max(b, c.y + c.alto) end
    for _, c in ipairs(varios) do c.y = math.max(0, b - c.alto) end
  elseif que == "tamano" then
    for _, c in ipairs(varios) do
      if c ~= sel then
        c.ancho = math.min(sel.ancho, v.ancho - c.x)
        c.alto = math.min(sel.alto, v.alto - c.y)
      end
    end
  elseif que == "repartir-h" or que == "repartir-v" then
    local horizontal = que == "repartir-h"
    local l = ordenar_por(horizontal and "x" or "y")
    if #l < 3 then estado("Para repartir hacen falta tres o más.") return end
    local primero, ultimo = l[1], l[#l]
    local desde = horizontal and primero.x or primero.y
    local hasta = horizontal and ultimo.x or ultimo.y
    local paso = (hasta - desde) / (#l - 1)
    for i = 2, #l - 1 do
      if horizontal then l[i].x = ajustar(desde + paso * (i - 1)) else l[i].y = ajustar(desde + paso * (i - 1)) end
    end
  end
  estado(#varios .. " controles: " .. que .. ".")
end

local function y_menus() return insp_y0() + #propiedades() * FILA + 26 end
-- Con varios controles elegidos, sus botones van debajo de las propiedades
local function y_varios() return insp_y0() + #propiedades() * FILA + 36 end

-- Los botones de la seccion "Varios controles" del inspector
local BOTONES_VARIOS = { { "Izquierda", "izquierda" }, { "Derecha", "derecha" }, { "Arriba", "arriba" }, { "Abajo", "abajo" },
                         { "Igualar tamaño", "tamano" }, { "Repartir ↔", "repartir-h" }, { "Repartir ↕", "repartir-v" } }
local function rect_boton_varios(i, x0)
  return x0 + 8 + (i % 2) * 108, y_varios() + (i // 2) * 26, 100, 22
end

local BOTONES_MENU = { { "+ Menú", 0 }, { "+ Entrada", 1 }, { "+ Separador", 2 }, { "Quitar", 3 }, { "+ Subentrada", 4 } }
local function rect_boton_menu(i, x0)
  local y = y_menus() + #filas_menus() * FILA + 6
  local col, fila = i % 2, i // 2
  return x0 + 8 + col * 108, y + fila * 26, 102, 22
end
local function dibujar_inspector()
  local x0 = ancho_v - INSP_W
  gui.rect(x0, BARRA_H, INSP_W, alto_v - BARRA_H - ESTADO_H, PANEL)
  gui.texto(x0 + 8, BARRA_H + 8, sel and (sel.nombre .. " (" .. NOMBRES[sel.tipo] .. ")") or "La ventana", TINTA)
  for i, prop in ipairs(propiedades()) do
    local y = insp_y0() + (i - 1) * FILA
    if y >= INSP_Y0 then
    local ed = editando and not editando.obj and editando.prop[1] == prop[1]
    gui.rect(x0 + 4, y, INSP_W - 8, FILA - 1, ed and gui.rgb(255, 255, 255) or gui.rgb(228, 226, 220))
    gui.texto(x0 + 8, y + 2, prop[2], TENUE)
    local v = ed and (editando.texto .. "|") or valor(prop)
    if ed and editando.nuevo then gui.rect(x0 + 108, y + 2, math.max(8, gui.medir_texto(editando.texto)) + 4, FILA - 5, gui.rgb(180, 210, 250)) end
    gui.texto(x0 + 110, y + 2, v, prop[3] == "fijo" and TENUE or TINTA)
    end
  end
  if sel then
    local y = insp_y0() + #propiedades() * FILA + 10
    gui.rect(x0 + 8, y, 110, 22, gui.rgb(170, 70, 60)); gui.texto(x0 + 16, y + 3, "Quitar (Supr)", gui.rgb(255, 255, 255))
  else
    -- los menus
    local y0 = y_menus()
    if y0 - 20 >= INSP_Y0 then gui.texto(x0 + 8, y0 - 20, "Menús", TINTA) end
    for i, f in ipairs(filas_menus()) do
      local y = y0 + (i - 1) * FILA
      if y < INSP_Y0 then goto siguiente_fila end
      local elegida = menu_sel and menu_sel.m == f.m and menu_sel.e == f.e
      local ed_aqui = editando and editando.obj and (editando.obj == (f.e or f.m))
      gui.rect(x0 + 4, y, INSP_W - 8, FILA - 1, ed_aqui and gui.rgb(255, 255, 255) or elegida and gui.rgb(190, 210, 240) or gui.rgb(228, 226, 220))
      if f.e and f.e.texto == "" then
        gui.rect(x0 + (f.madre and 44 or 24), y + FILA // 2, INSP_W - (f.madre and 60 or 40), 1, TENUE)
      else
        local t = ed_aqui and (editando.texto .. "|") or (f.e and f.e.texto or f.m.texto)
        if f.e and A.tiene_submenu(f.e) then t = t .. "  >" end                  -- abre un submenu
        gui.texto(x0 + (f.madre and 44 or f.e and 24 or 8), y + 2, t, f.e and TINTA or ACENTO)
      end
      ::siguiente_fila::
    end
    for i, b in ipairs(BOTONES_MENU) do
      local bx, by, bw, bh = rect_boton_menu(i - 1, x0)
      if by >= INSP_Y0 then gui.rect(bx, by, bw, bh, gui.rgb(170, 176, 186)); gui.texto(bx + 8, by + 3, b[1], TINTA) end
    end
  end
end

local function dibujar_varios()
  if #varios < 2 then return end
  local x0 = ancho_v - INSP_W
  gui.texto(x0 + 8, y_varios() - 20, #varios .. " controles elegidos", TINTA)
  for i, b in ipairs(BOTONES_VARIOS) do
    local bx, by, bw, bh = rect_boton_varios(i - 1, x0)
    if by >= INSP_Y0 then
      gui.rect(bx, by, bw, bh, gui.rgb(170, 176, 186))
      gui.texto(bx + 6, by + 3, b[1], TINTA)
    end
  end
end

local function alto_inspector()
  if #varios > 1 then
    local _, by, _, bh = rect_boton_varios(#BOTONES_VARIOS - 1, 0)
    return by + bh - insp_y0() + 10
  end
  if sel then return #propiedades() * FILA + 40 end
  local _, by, _, bh = rect_boton_menu(#BOTONES_MENU - 1, 0)
  return by + bh - insp_y0() + 10
end
local function desplazar_inspector(pasos)
  local visible = alto_v - ESTADO_H - INSP_Y0
  local maximo = math.max(0, alto_inspector() - visible)
  insp_scroll = math.max(0, math.min(maximo, insp_scroll - pasos * FILA))
end

-- Los pines que puede usar el control seleccionado, libres, en orden
local function pines_validos(c)
  local t = A.TIPOS[c.tipo]
  local usados = {}
  for _, o in ipairs(p.controles) do if o ~= c and o.pin and A.TIPOS[o.tipo].gpio then usados[o.pin] = true end end
  local r = {}
  if t.gpio == "pwm" then
    for _, n in ipairs({ 12, 13, 18, 19 }) do if not usados[n] then r[#r + 1] = n end end
  else
    for n = 2, 27 do if n ~= 14 and n ~= 15 and not usados[n] then r[#r + 1] = n end end
  end
  return r
end

local function siguiente_de(lista, actual)
  for i, v in ipairs(lista) do if v == actual then return lista[i % #lista + 1] end end
  return lista[1]
end

local function clic_inspector(mx, my)
  if my < INSP_Y0 then return end                 -- el titulo (lo que ha subido por encima no se pulsa)
  local x0 = ancho_v - INSP_W
  local props = propiedades()
  local i = (my - insp_y0()) // FILA + 1
  if my >= insp_y0() and props[i] then
    local prop = props[i]
    local obj = sel or p.ventana
    -- lo que cambia al momento (si/no, pin, canal, barra) se recuerda aqui;
    -- lo que se escribe, al aceptarlo (aplicar_edicion)
    if prop[3] == "si_no" or prop[3] == "pin" or prop[3] == "canal" or prop[3] == "barra" then recordar() end
    if prop[3] == "si_no" then obj[prop[1]] = not obj[prop[1]]; editando = nil
    elseif prop[3] == "pin" then
      local v = pines_validos(sel)
      if #v == 0 then estado("No queda ningún pin libre para este control.") else sel.pin = siguiente_de(v, sel.pin) end
      editando = nil
    elseif prop[3] == "canal" then sel.canal = ((sel.canal or 0) + 1) % 8; editando = nil
    elseif prop[3] == "ancla" then
      recordar()
      sel.ancla = A.ancla_siguiente(sel.ancla)
      estado(sel.nombre .. ": anclaje " .. sel.ancla .. " (clic para cambiarlo; se aplica al cambiar el tamaño de la ventana).")
    elseif prop[3] == "imagen" then
      -- elegir el archivo; el programa lo buscara en la raiz y en DOCUMENTOS
      editando = nil
      -- el dialogo empieza en DOCUMENTOS/IMAGENES, donde estan los iconos
      local docs = fs.buscar("DOCUMENTOS", fs.RAIZ, fs.VOL_NEMOFS)
      local imgs = docs and fs.buscar("IMAGENES", docs.inodo, fs.VOL_NEMOFS)
      local nombre = gui.dialogo_abrir(imgs and imgs.inodo or nil)
      if not nombre then estado("Imagen: cancelado.")
      elseif not nombre:lower():match("%.nimg$") then estado("Tiene que ser una imagen .nimg (herramientas/nimg/nimg_convert.py convierte PNG y JPEG).")
      else
        imagenes[nombre] = nil                       -- volver a cargarla, por si ha cambiado
        local img, iw, ih = imagen_de(nombre)
        if not img then
          estado(nombre .. ": el programa busca las imágenes en la raíz, en DOCUMENTOS y en DOCUMENTOS/IMAGENES. Cópiala allí.")
        else
          recordar()
          sel.imagen = nombre; sel.ancho, sel.alto = iw, ih
          estado(sel.nombre .. ": " .. nombre .. " (" .. iw .. " x " .. ih .. "). Cambiarla desde el código: SetPanelImage " .. sel.nombre .. ", \"otra.nimg\"")
        end
      end
    elseif prop[1] == "evento" then ir_a_codigo(sel)
    elseif prop[1] == "evento_barra" then ir_a_funcion("Barra_Click", "boton")
    elseif prop[1] == "evento_temp" then ir_a_funcion("Temporizador_Tick")
    elseif prop[3] == "barra" then
      -- la siguiente tira de DOCUMENTOS, o ninguna
      local actual, siguiente = p.barra and p.barra.archivo, nil
      if not actual then siguiente = A.BARRAS[1]
      else for i, b in ipairs(A.BARRAS) do if b[1] == actual then siguiente = A.BARRAS[i + 1] end end end
      p.barra = siguiente and { archivo = siguiente[1], ayudas = siguiente[2] } or nil
      editando = nil
    elseif prop[3] ~= "fijo" then
      -- como en cualquier inspector: el valor queda "seleccionado", y lo
      -- primero que se escribe lo sustituye (Retroceso, en cambio, edita)
      local v = (prop[3] == "numero_p") and tostring(p.temporizador or 0) or tostring(obj[prop[1]] or "")
      editando = { prop = prop, texto = v, nuevo = true }
    end
    return
  end
  if sel then
    local y = insp_y0() + #props * FILA + 10
    if dentro(mx, my, x0 + 8, y, 110, 22) then recordar(); A.quitar(p, sel.nombre); elegir(nil); estado("Control quitado. (Ctrl+Z para deshacer)") end
    for i, b in ipairs(BOTONES_VARIOS) do           -- alinear, igualar, repartir
      local bx, by, bw, bh = rect_boton_varios(i - 1, x0)
      if #varios > 1 and dentro(mx, my, bx, by, bw, bh) then apanar(b[2]); return end
    end
    return
  end
  -- los menus: una fila (clic la elige; otro clic, a editar su texto; doble clic en una entrada, a su funcion)
  local filas = filas_menus()
  local i = (my - y_menus()) // FILA + 1
  if my >= y_menus() and filas[i] then
    local f = filas[i]
    local ahora = nemo.ticks()
    if f.e and f.e.nombre and not A.tiene_submenu(f.e) and ultimo_clic.control == f.e and ahora - ultimo_clic.t < DOBLE_CLIC then
      ultimo_clic.control = nil
      ir_a_funcion(f.e.nombre .. "_Click"); return
    end
    ultimo_clic.control, ultimo_clic.t = f.e or f.m, ahora
    local ya = menu_sel and menu_sel.m == f.m and menu_sel.e == f.e
    menu_sel = { m = f.m, e = f.e, madre = f.madre }
    editando = nil
    if ya and not (f.e and f.e.texto == "") then
      editando = { prop = { "texto", "Texto", "texto" }, obj = f.e or f.m, texto = (f.e or f.m).texto, nuevo = true }
    end
    estado(f.e and (A.tiene_submenu(f.e) and ("Entrada " .. f.e.nombre .. ": abre un submenú (no tiene función propia).")
                    or f.e.nombre and ("Entrada " .. f.e.nombre .. ": doble clic para su función, " .. f.e.nombre .. "_Click().") or "Separador.")
                 or ("Menú " .. f.m.texto .. ": otro clic para cambiarle el texto."))
    return
  end
  -- los botones de la seccion
  for k, b in ipairs(BOTONES_MENU) do
    local bx, by, bw, bh = rect_boton_menu(k - 1, x0)
    if dentro(mx, my, bx, by, bw, bh) then
      editando = nil
      if b[2] == 0 or menu_sel then recordar() end
      if b[2] == 0 then
        local m = A.anadir_menu(p, "Menú " .. (#p.menus + 1))
        menu_sel = { m = m }
        estado("Menú añadido. Clic en él para cambiarle el texto.")
      elseif not menu_sel then
        estado("Elige primero un menú (o una entrada) en la lista.")
      elseif b[2] == 4 then
        -- dentro de la entrada elegida (o de la madre, si se eligio una subentrada)
        local madre = menu_sel.madre or menu_sel.e
        if not madre or madre.texto == "" then estado("Elige primero una entrada (no un separador) para ponerle un submenú.")
        else
          local e = A.anadir_subentrada(p, madre, "Subentrada")
          menu_sel = { m = menu_sel.m, e = e, madre = madre }
          estado("Subentrada " .. e.nombre .. " en el submenú de " .. madre.texto .. ". Doble clic para su función.")
        end
      elseif b[2] == 1 or b[2] == 2 then
        if menu_sel.madre then
          -- con una subentrada elegida, "+ Entrada" y "+ Separador" van a su submenu
          local e = A.anadir_subentrada(p, menu_sel.madre, b[2] == 1 and "Entrada" or "")
          menu_sel = { m = menu_sel.m, e = e, madre = menu_sel.madre }
        else
          local e = A.anadir_entrada(p, menu_sel.m, b[2] == 1 and "Entrada" or "")
          menu_sel = { m = menu_sel.m, e = e }
        end
        estado(b[2] == 1 and ("Entrada " .. menu_sel.e.nombre .. " añadida. Doble clic para su función.") or "Separador añadido.")
      else
        local m, e = menu_sel.m, menu_sel.e
        if e and menu_sel.madre then
          local lista = menu_sel.madre.entradas
          for j, x in ipairs(lista) do if x == e then table.remove(lista, j) break end end
        elseif e then
          for j, x in ipairs(m.entradas) do if x == e then table.remove(m.entradas, j) break end end
        else
          for j, x in ipairs(p.menus) do if x == m then table.remove(p.menus, j) break end end
        end
        menu_sel = nil
        estado("Quitado. (Sus funciones siguen en el código, por si las quieres.)")
      end
    end
  end
end

-- Aplicar lo que se ha escrito en el inspector
local function aplicar_edicion()
  recordar()                          -- (si el valor no vale, la foto sobra, pero no molesta)
  local prop, txt = editando.prop, editando.texto
  local obj = editando.obj or sel or p.ventana
  local clase = prop[3]
  if editando.obj and editando.obj.entradas == nil and editando.obj.nombre then
    -- una entrada de menu: si su codigo aun no usa el nombre, que siga al texto
    local e = editando.obj
    local usado = p.codigo:find("%f[%w_]" .. e.nombre .. "%f[^%w_]") or p.codigo:find("%f[%w_]" .. e.nombre .. "_")
    e.texto = txt
    if not usado and txt ~= "" then e.nombre = nil; e.nombre = A.nombre_menu(p, txt) end
    editando = nil
    estado("Entrada " .. e.nombre .. ": doble clic para su función, " .. e.nombre .. "_Click().")
    return
  end
  if clase == "numero_p" then
    local n = tonumber(txt)
    if not n or n < 0 or n > 100 then estado("De 1 a 100 veces por segundo (0 = ninguno).") return end
    p.temporizador = math.floor(n)
  elseif clase == "numero" then
    local n = tonumber(txt)
    if not n then estado("Eso no es un número.") return end
    n = math.floor(n)
    if (prop[1] == "ancho" or prop[1] == "alto") and n < 8 then n = 8 end
    obj[prop[1]] = n
  elseif clase == "nombre" then
    if not A.nombre_valido(txt) then estado("Un nombre empieza por letra y lleva letras, cifras o _.") return end
    for _, o in ipairs(p.controles) do
      if o ~= sel and o.nombre:lower() == txt:lower() then estado("Ya hay un control que se llama " .. o.nombre .. ".") return end
    end
    if obj.nombre ~= txt then renombrar_en_codigo(obj.nombre, txt) end
    obj.nombre = txt
  else
    obj[prop[1]] = txt
  end
  editando = nil
  estado("")
end

-- ---------------------------------------------------------------------
-- El formulario
-- ---------------------------------------------------------------------
-- La lista de plantillas, sobre el formulario
local FILA_PL = 34
local function rect_plantillas()
  -- centrada en la zona del formulario (entre la paleta y el inspector)
  local x0, y0 = PALETA_W, BARRA_H
  local w, h = ancho_v - PALETA_W - INSP_W, alto_v - BARRA_H - ESTADO_H
  local ancho = math.min(w - 40, 420)
  local alto = #A.PLANTILLAS * FILA_PL + 44
  return x0 + (w - ancho) // 2, y0 + math.max(10, (h - alto) // 3), ancho, alto
end
local function dibujar_plantillas()
  local x, y, w, h = rect_plantillas()
  gui.rect(x, y, w, h, gui.rgb(238, 236, 230))
  gui.rect(x, y, w, 1, TINTA); gui.rect(x, y + h - 1, w, 1, TINTA)
  gui.rect(x, y, 1, h, TINTA); gui.rect(x + w - 1, y, 1, h, TINTA)
  gui.texto(x + 12, y + 10, "Empezar con una plantilla", TINTA)
  for i, pl in ipairs(A.PLANTILLAS) do
    local fy = y + 34 + (i - 1) * FILA_PL
    gui.rect(x + 8, fy, w - 16, FILA_PL - 4, gui.rgb(250, 249, 245))
    gui.texto(x + 16, fy + 2, pl[1], ACENTO)
    gui.texto(x + 16, fy + 16, pl[2], TENUE)
  end
end

local function dibujar_formulario()
  local x0 = PALETA_W
  gui.rect(x0, BARRA_H, ancho_v - PALETA_W - INSP_W, alto_v - BARRA_H - ESTADO_H, FONDO)
  local ox, oy = origen_form()
  local v = p.ventana
  -- la ventana que se diseña: barra de titulo, fondo, rejilla
  -- el titulo va encima de la barra de menus, si la hay (antes, la barra lo tapaba)
  local ty = oy - TITULO_H - ((#p.menus > 0) and A.ALTO_MENUS or 0)
  gui.rect(ox, ty, v.ancho, TITULO_H, gui.rgb(40, 60, 90))
  gui.texto(ox + 8, ty + (TITULO_H - ALTO_LETRA) // 2, v.titulo, gui.rgb(255, 255, 255))
  if #p.menus > 0 then                -- la barra de menus, como la dibuja el kernel
    local my = oy - A.ALTO_MENUS
    gui.rect(ox, my, v.ancho, A.ALTO_MENUS, FORM)
    local mx = ox + 4
    for _, m in ipairs(p.menus) do gui.texto(mx, my + (A.ALTO_MENUS - ALTO_LETRA) // 2, m.texto, TINTA); mx = mx + gui.medir_texto(m.texto) + 14 end
    gui.rect(ox, oy - 1, v.ancho, 1, gui.rgb(170, 168, 160))
  end
  gui.rect(ox, oy, v.ancho, v.alto, FORM)
  for gy = REJILLA, v.alto - 1, REJILLA * 2 do
    for gx = REJILLA, v.ancho - 1, REJILLA * 2 do gui.punto(ox + gx, oy + gy, PUNTO) end
  end
  if p.barra then                     -- la barra de herramientas, con sus iconos de verdad
    local img = imagenes_barra[p.barra.archivo]
    if img == nil then
      img = gui.cargar_imagen(p.barra.archivo) or false
      if img then gui.transparente(img, gui.rgb(255, 0, 255)) end   -- el magenta de las tiras
      imagenes_barra[p.barra.archivo] = img
    end
    local n = 0; for _ in (p.barra.ayudas .. ","):gmatch("([^,]*),") do n = n + 1 end
    for i = 0, n - 1 do gui.rect(ox + i * A.ALTO_BARRA, oy, A.ALTO_BARRA, A.ALTO_BARRA, gui.rgb(0x30, 0x38, 0x40)) end
    if img then gui.dibujar_imagen(img, ox, oy) end
  end
  for _, c in ipairs(p.controles) do dibujar_control(c, ox, oy) end
  -- los demas elegidos: marco mas tenue (el principal lleva el asa)
  for _, c in ipairs(varios) do
    if c ~= sel then
      local x, y = ox + c.x, oy + c.y
      local col = gui.rgb(120, 160, 210)
      gui.rect(x - 2, y - 2, c.ancho + 4, 1, col); gui.rect(x - 2, y + c.alto + 1, c.ancho + 4, 1, col)
      gui.rect(x - 2, y - 2, 1, c.alto + 4, col); gui.rect(x + c.ancho + 1, y - 2, 1, c.alto + 4, col)
    end
  end
  if banda then                        -- el recuadro que se esta arrastrando
    local x1, y1 = math.min(banda.x0, banda.x1), math.min(banda.y0, banda.y1)
    local w1, h1 = math.abs(banda.x1 - banda.x0), math.abs(banda.y1 - banda.y0)
    gui.rect(x1, y1, w1, 1, ACENTO); gui.rect(x1, y1 + h1, w1, 1, ACENTO)
    gui.rect(x1, y1, 1, h1, ACENTO); gui.rect(x1 + w1, y1, 1, h1, ACENTO)
  end
  -- seleccion: marco y asa
  if sel then
    local x, y = ox + sel.x, oy + sel.y
    gui.rect(x - 2, y - 2, sel.ancho + 4, 1, ACENTO); gui.rect(x - 2, y + sel.alto + 1, sel.ancho + 4, 1, ACENTO)
    gui.rect(x - 2, y - 2, 1, sel.alto + 4, ACENTO); gui.rect(x + sel.ancho + 1, y - 2, 1, sel.alto + 4, ACENTO)
    gui.rect(x + sel.ancho - 2, y + sel.alto - 2, 7, 7, ACENTO)
  end
  -- el asa de la ventana diseñada
  gui.rect(ox + v.ancho - 4, oy + v.alto - 4, 8, 8, gui.rgb(230, 230, 230))
end

local function control_en(mx, my)
  local ox, oy = origen_form()
  for i = #p.controles, 1, -1 do        -- el de encima primero
    local c = p.controles[i]
    if dentro(mx, my, ox + c.x, oy + c.y, c.ancho, c.alto) then return c end
  end
end

local function pulsar_formulario(mx, my)
  local ox, oy = origen_form()
  local v = p.ventana
  editando = nil
  if colocar then
    if dentro(mx, my, ox, oy, v.ancho, v.alto) then
      recordar()
      local c = A.anadir(p, colocar, ajustar(mx - ox), ajustar(my - oy))
      if A.TIPOS[c.tipo].gpio and A.TIPOS[c.tipo].gpio ~= "analogico" then
        local libres = pines_validos(c)
        if not (function() for _, n in ipairs(libres) do if n == c.pin then return true end end end)() then c.pin = libres[1] end
      end
      elegir(c); colocar = nil
      estado(c.nombre .. " colocado. Arrástralo para moverlo; su esquina, para el tamaño.")
    end
    return
  end
  if sel and dentro(mx, my, ox + sel.x + sel.ancho - 3, oy + sel.y + sel.alto - 3, 9, 9) then
    arrastre = { modo = "tamano" }; return
  end
  if dentro(mx, my, ox + v.ancho - 5, oy + v.alto - 5, 10, 10) then
    arrastre = { modo = "ventana" }; elegir(nil); return
  end
  if p.barra and dentro(mx, my, ox, oy, v.ancho, A.ALTO_BARRA) and not control_en(mx, my) then
    local ahora = nemo.ticks()
    if ultimo_clic.control == "barra" and ahora - ultimo_clic.t < DOBLE_CLIC then
      ultimo_clic.control = nil; ir_a_funcion("Barra_Click", "boton"); return
    end
    ultimo_clic.control, ultimo_clic.t = "barra", ahora
    elegir(nil); estado("La barra de herramientas: doble clic para su función, Barra_Click(boton).")
    return
  end
  local c = control_en(mx, my)
  if c then
    local ahora = nemo.ticks()
    if ultimo_clic.control == c and ahora - ultimo_clic.t < DOBLE_CLIC then
      ultimo_clic.control = nil
      elegir(c); ir_a_codigo(c); return
    end
    ultimo_clic.control, ultimo_clic.t = c, ahora
    if mayus_pulsada() then
      elegir(c, true)
      estado(#varios .. " control(es) elegidos. Arrástralos, o usa los botones del inspector.")
      return
    end
    if not esta_elegido(c) then elegir(c) end     -- si ya estaba en el grupo, se arrastra el grupo entero
    sel = c
    arrastre = { modo = "mover", dx = mx - (ox + c.x), dy = my - (oy + c.y) }
  elseif dentro(mx, my, ox, oy, v.ancho, v.alto) then
    -- sobre el fondo: un recuadro que elige lo que quede dentro
    if not mayus_pulsada() then elegir(nil) end
    arrastre = { modo = "banda" }
    banda = { x0 = mx, y0 = my, x1 = mx, y1 = my }
  else
    elegir(nil)
  end
end

local function arrastrar(mx, my)
  local ox, oy = origen_form()
  local v = p.ventana
  -- un arrastre entero es UN paso: la foto se guarda en el primer movimiento
  if not arrastre.grabado then arrastre.grabado = true; recordar() end
  if arrastre.modo == "banda" then
    banda.x1, banda.y1 = mx, my
    return true
  elseif arrastre.modo == "mover" and sel then
    local nx = ajustar(mx - ox - arrastre.dx)
    local ny = ajustar(my - oy - arrastre.dy)
    nx = math.max(0, math.min(nx, v.ancho - sel.ancho))
    ny = math.max(0, math.min(ny, v.alto - sel.alto))
    -- con varios elegidos, todos se mueven lo mismo (sin que ninguno se salga)
    local dx, dy = nx - sel.x, ny - sel.y
    if #varios > 1 then
      for _, c in ipairs(varios) do
        dx = math.max(dx, -c.x); dy = math.max(dy, -c.y)
        dx = math.min(dx, v.ancho - c.ancho - c.x); dy = math.min(dy, v.alto - c.alto - c.y)
      end
    end
    if dx ~= 0 or dy ~= 0 then
      for _, c in ipairs(varios) do c.x = c.x + dx; c.y = c.y + dy end
      if #varios == 0 then sel.x, sel.y = nx, ny end
      return true
    end
  elseif arrastre.modo == "tamano" and sel then
    local nw = math.max(8, ajustar(mx - ox - sel.x))
    local nh = math.max(8, ajustar(my - oy - sel.y))
    nw = math.min(nw, v.ancho - sel.x); nh = math.min(nh, v.alto - sel.y)
    if nw ~= sel.ancho or nh ~= sel.alto then sel.ancho, sel.alto = nw, nh; return true end
  elseif arrastre.modo == "ventana" then
    local nw = math.max(120, ajustar(mx - ox))
    local nh = math.max(80, ajustar(my - oy))
    if nw ~= v.ancho or nh ~= v.alto then v.ancho, v.alto = nw, nh; return true end
  end
  return false
end

-- ---------------------------------------------------------------------
-- Barra de botones: nuevo, abrir, guardar
-- ---------------------------------------------------------------------
local BOTONES = { { "Nuevo", 8, 70 }, { "Abrir", 84, 70 }, { "Guardar", 160, 80 },
                  { "Diseño", 262, 80 }, { "Código", 346, 80 }, { "Ver programa", 432, 110 }, { "Ejecutar", 548, 84 },
                  { "Deshacer", 640, 80 }, { "Rehacer", 724, 76 }, { "Plantillas", 806, 92 } }

local function dibujar_barra()
  gui.rect(0, 0, ancho_v, BARRA_H, gui.rgb(40, 60, 90))
  for _, b in ipairs(BOTONES) do
    if b[1] ~= "Ver programa" or modo == "codigo" then
      local activo = (b[1] == "Diseño" and modo == "diseno") or (b[1] == "Código" and modo == "codigo" and not ver_programa)
                     or (b[1] == "Ver programa" and ver_programa)
      local fondo = activo and gui.rgb(250, 250, 250) or gui.rgb(170, 176, 186)
      if b[1] == "Ejecutar" then fondo = compilando and gui.rgb(200, 190, 120) or gui.rgb(120, 200, 130) end
      gui.rect(b[2], 4, b[3], BARRA_H - 8, fondo)
      gui.texto(b[2] + 10, 4 + (BARRA_H - 8 - ALTO_LETRA) // 2, b[1], TINTA)
    end
  end
  local nombre = archivo and archivo.nombre or "(sin guardar)"
  gui.texto(906, (BARRA_H - ALTO_LETRA) // 2, nombre, gui.rgb(230, 230, 230))
end

local function sin_extension(n) return (n:gsub("%.[Aa][Nn][Xx]$", "")) end

local function guardar()
  local nombre, carpeta
  if archivo then nombre, carpeta = archivo.nombre, archivo.carpeta
  else
    nombre, carpeta = gui.dialogo_guardar()
    if not nombre then estado("Guardar: cancelado.") return end
    if not nombre:lower():match("%.anx$") then nombre = nombre .. ".anx" end
  end
  if not fs.escribir_en(nombre, A.escribir(p), carpeta) then estado("No se pudo guardar " .. nombre .. ".") return end
  archivo = { nombre = nombre, carpeta = carpeta }
  -- y el programa generado, al lado
  -- Dos listas: los problemas impiden generar, los avisos no. Un aviso
  -- que bloquea no es un aviso -- y el primero que escribi (el de las
  -- cajas de texto) dejaba sin compilar cualquier proyecto que tuviera
  -- una.
  local problemas, avisos = A.comprobar(p)
  if #problemas > 0 then estado("Guardado, pero el programa no se genera: " .. problemas[1]) return false end
  local nb = sin_extension(nombre) .. ".nb"
  marcar_guardado()                   -- el proyecto ya esta a salvo (aunque falle el .nb)
  if fs.escribir_en(nb, (A.generar(p)), carpeta) then
    -- El aviso se ensena DESPUES de decir que salio bien, y solo el
    -- primero: son cosas que conviene saber, no que impidan nada.
    if #avisos > 0 then
      estado("Generado " .. nb .. ".  Ojo: " .. avisos[1])
    else
      estado("Guardado " .. nombre .. " y generado " .. nb .. ".")
    end
    return true
  else
    estado("Guardado " .. nombre .. ", pero no se pudo escribir " .. nb .. ".")
    return false
  end
end

-- ---------------------------------------------------------------------
-- Ejecutar (fase 4): guardar, compilar con nbc.pro y lanzar el programa
-- ---------------------------------------------------------------------
-- Igual que el IDE: nbc.pro se lanza con "carpeta:NOMBRE.nb" y su salida
-- llega, letra a letra, por SYS_READ_CONSOLE_OUTPUT (7). "listo ->" es
-- que compilo; "error ... linea N: mensaje", que no.
local SYS_LAUNCH_PROGRAM, SYS_READ_CONSOLE_OUTPUT = 5, 7
local ESPERA_MAXIMA = 1000           -- 10 s sin decir nada: el compilador no responde

local function ejecutar()
  if compilando then estado("Ya se está compilando.") return end
  sincronizar()
  if not guardar() then return end   -- guardar ya explica que ha pasado
  local nb = sin_extension(archivo.nombre) .. ".nb"
  local _, inicio = A.generar(p)
  ejecutando = nil                    -- una compilacion nueva: lo que dijera el anterior ya no cuenta
  compilando = { linea = "", inicio = inicio, pro = sin_extension(archivo.nombre) .. ".pro",
                 carpeta = archivo.carpeta or 0, desde = nemo.ticks() }
  nemo.syscall(SYS_LAUNCH_PROGRAM, "nbc.pro", tostring(archivo.carpeta or 0) .. ":" .. nb, 0xFFFFFFFF)
  estado("Compilando " .. nb .. "...")
end

-- Un error del compilador, en tu codigo: a la pestaña Codigo, en su linea
local function mostrar_error(linea_prog, texto, inicio)
  local donde, n = A.traducir_linea(inicio or compilando.inicio, linea_prog)
  if donde == "codigo" then
    if modo ~= "codigo" then cargar_codigo() end
    modo = "codigo"; ver_programa = false; editando = nil
    ed.fila = math.max(1, math.min(n, #ed.lineas)); ed.col = #ed.lineas[ed.fila]; ed.error_fila = ed.fila
    estado("Error en tu línea " .. n .. ": " .. texto)
  else
    estado("Error en la parte que genera Aronnax (línea " .. n .. " del programa): " .. texto)
  end
end

-- Cada vuelta: leer lo que diga el compilador. Devuelve si hay que repintar.
local function vigilar_ejecucion()
  if not ejecutando or compilando then return false end
  while true do
    local c = nemo.syscall(SYS_READ_CONSOLE_OUTPUT)
    if not c or c == 0 then return false end
    if c == 10 then
      local l = ejecutando.linea
      ejecutando.linea = ""
      local n, archivo, texto = l:match("[Ee]rror en tiempo de ejecucion, linea (%d+)(.-):%s*(.*)$")
      if n then
        local inicio = ejecutando.inicio
        ejecutando = nil
        local otro = archivo:match("^ de (.+)$")
        if otro then estado("Error al ejecutar, en " .. otro .. " (incluido con Include), línea " .. n .. ": " .. texto)
        else mostrar_error(tonumber(n), "al ejecutar: " .. texto, inicio) end
        return true
      end
    elseif #ejecutando.linea < 400 then
      ejecutando.linea = ejecutando.linea .. string.char(c & 0xFF)
    end
  end
end

local function vigilar_compilacion()
  if vigilar_ejecucion() then return true end
  if not compilando then return false end
  local cambio = false
  while true do
    local c = nemo.syscall(SYS_READ_CONSOLE_OUTPUT)
    if not c or c == 0 then break end
    compilando.desde = nemo.ticks()
    if c == 10 then
      local l = compilando.linea
      compilando.linea = ""
      if l:lower():find("listo ->", 1, true) then
        nemo.syscall(SYS_LAUNCH_PROGRAM, compilando.pro, "", compilando.carpeta)
        estado("Compilado. Ejecutando " .. compilando.pro .. ".")
        ejecutando = { inicio = compilando.inicio, linea = "" }
        compilando = nil
        return true
      elseif l:lower():find("error", 1, true) then
        -- un error en un archivo de Include no es de tu codigo:
        -- "error en utiles.nb, linea 5: ..." -- se dice, sin llevar a ninguna linea
        local otro, ln, txt = l:match("error en ([^,]+%.[Nn][Bb]), l[ií]n?e?a?%s+(%d+):%s*(.*)$")
        if otro then
          estado("Error en " .. otro .. " (incluido con Include), línea " .. ln .. ": " .. txt)
          compilando = nil
          return true
        end
        local n, texto = l:match("[Ll][ií]n?e?a?%s+(%d+):%s*(.*)$")
        if not n then n, texto = l:match("l\195\173nea%s+(%d+):%s*(.*)$") end
        if n then mostrar_error(tonumber(n), texto) else estado(l) end
        compilando = nil
        return true
      end
    else
      compilando.linea = compilando.linea .. string.char(c & 0xFF)
    end
  end
  if compilando and nemo.ticks() - compilando.desde > ESPERA_MAXIMA then
    estado("El compilador no ha respondido. Si se ha cerrado, mira la terminal.")
    compilando = nil
    cambio = true
  end
  return cambio
end

local function abrir()
  local nombre, carpeta = gui.dialogo_abrir()
  if not nombre then estado("Abrir: cancelado.") return end
  local texto = fs.leer_en(nombre, carpeta)
  if not texto then estado("No se pudo leer " .. nombre .. ".") return end
  local q, err = A.leer(texto)
  if not q then estado(err) return end
  p, sel, colocar, editando = q, nil, nil, nil
  archivo = { nombre = nombre, carpeta = carpeta }
  guardado_como = A.escribir(p)
  estado("Abierto " .. nombre .. ".")
end

local function clic_barra(mx)
  for _, b in ipairs(BOTONES) do
    if mx >= b[2] and mx < b[2] + b[3] then
      if b[1] ~= confirmar then confirmar = nil end          -- otro boton: se olvida el aviso
      if b[1] == "Nuevo" then
        if not puede_descartar("Nuevo", "Pulsa otra vez Nuevo") then return end
        p, sel, colocar, editando, archivo = A.nuevo(), nil, nil, nil, nil
        guardado_como = A.escribir(p)
        historial, rehechos = {}, {}
        modo = "diseno"; ed.fila, ed.col, ed.arriba = 1, 0, 1; estado("Proyecto nuevo.")
      elseif b[1] == "Abrir" then
        if not puede_descartar("Abrir", "Pulsa otra vez Abrir") then return end
        abrir(); modo = "diseno"; ed.fila, ed.col, ed.arriba = 1, 0, 1; historial, rehechos = {}, {}
      elseif b[1] == "Guardar" then sincronizar(); guardar()
      elseif b[1] == "Diseño" then sincronizar(); modo = "diseno"; ver_programa = false
      elseif b[1] == "Código" then
        if modo ~= "codigo" then cargar_codigo() end
        modo = "codigo"; ver_programa = false; editando = nil
        estado("Tu código. Doble clic en un control, en el diseño, lleva a su función.")
      elseif b[1] == "Ejecutar" then ejecutar()
      elseif b[1] == "Plantillas" then
        if puede_descartar("Plantillas", "Pulsa otra vez Plantillas") then
          eligiendo_plantilla = true
          modo = "diseno"
          estado("Elige una plantilla: cada una es un proyecto que ya funciona. Esc para dejarlo.")
        end
      elseif b[1] == "Deshacer" then deshacer()
      elseif b[1] == "Rehacer" then rehacer()
      elseif b[1] == "Ver programa" and modo == "codigo" then
        sincronizar(); ver_programa = not ver_programa
        estado(ver_programa and "El programa entero: en gris, la parte que escribe Aronnax (solo lectura)." or "")
      end
    end
  end
end

-- ---------------------------------------------------------------------
-- Todo junto
-- ---------------------------------------------------------------------
local function redibujar()
  dibujar_barra()
  if modo == "codigo" then dibujar_codigo()
  else
    dibujar_formulario()
    dibujar_paleta()
    dibujar_inspector()
    dibujar_varios()
    if eligiendo_plantilla then dibujar_plantillas() end
  end
  gui.rect(0, alto_v - ESTADO_H, ancho_v, ESTADO_H, gui.rgb(40, 44, 52))
  gui.texto(8, alto_v - ESTADO_H + (ESTADO_H - ALTO_LETRA) // 2, mensaje, gui.rgb(230, 230, 180))
end

-- ---- Copiar, cortar, pegar, duplicar ----
local CAMPOS_COPIA = { "tipo", "ancho", "alto", "texto", "pin", "canal", "imagen" }
local function copiar()
  if not sel then estado("Elige un control para copiarlo.") return false end
  portapapeles = {}
  for _, k in ipairs(CAMPOS_COPIA) do portapapeles[k] = sel[k] end
  portapapeles.x, portapapeles.y = sel.x, sel.y
  estado(sel.nombre .. " copiado. Ctrl+V para pegarlo.")
  return true
end
local function pegar()
  if not portapapeles then estado("No hay nada copiado.") return end
  recordar()
  local v = p.ventana
  local c = A.anadir(p, portapapeles.tipo, 0, 0)
  for _, k in ipairs(CAMPOS_COPIA) do if portapapeles[k] ~= nil then c[k] = portapapeles[k] end end
  -- un poco desplazado, sin salirse de la ventana
  c.x = math.max(0, math.min(portapapeles.x + REJILLA * 2, v.ancho - c.ancho))
  c.y = math.max(0, math.min(portapapeles.y + REJILLA * 2, v.alto - c.alto))
  portapapeles.x, portapapeles.y = c.x, c.y           -- el siguiente, otro poco mas alla
  -- un control del Nautilus no puede compartir pin: el siguiente libre
  local g = A.TIPOS[c.tipo].gpio
  if g and g ~= "analogico" then
    local libres = pines_validos(c)
    if #libres == 0 then A.quitar(p, c.nombre); table.remove(historial); estado("No queda ningún pin libre para otro " .. NOMBRES[c.tipo] .. ".") return end
    local libre = false
    for _, n in ipairs(libres) do if n == c.pin then libre = true end end
    if not libre then c.pin = libres[1] end
  end
  elegir(c)
  estado(c.nombre .. " pegado" .. (g and g ~= "analogico" and (" (pin " .. c.pin .. ")") or "") .. ".")
end
-- Ctrl (o Cmd, en el Mac) pulsado: la letra es un atajo, no se escribe
local function ctrl_pulsado()
  return gui.tecla_pulsada(29) or gui.tecla_pulsada(97) or gui.tecla_pulsada(125) or gui.tecla_pulsada(126)
end
local function atajo(ch)
  local c = string.char(ch):lower()
  if c == "z" then deshacer() return true end
  if c == "y" then rehacer() return true end
  if modo == "diseno" and not editando then
    if c == "c" then copiar() return true end
    if c == "x" then if copiar() then recordar(); A.quitar(p, sel.nombre); elegir(nil); estado("Cortado. Ctrl+V para pegarlo.") end return true end
    if c == "v" then pegar() return true end
    if c == "d" then if copiar() then pegar() end return true end
  end
  return true                         -- cualquier otra Ctrl+letra: no se escribe
end

-- Teclas sin caracter (codigos del teclado)
local TECLA_SUPR, TECLA_ARRIBA, TECLA_IZQ, TECLA_DER, TECLA_ABAJO = 111, 103, 105, 106, 108

local function tecla_codigo(k)
  if editando or not sel then return false end
  local v = p.ventana
  if k == TECLA_SUPR then
    recordar()
    local n = #varios
    for _, c in ipairs(varios) do A.quitar(p, c.nombre) end
    varios = {}; sel = nil
    estado((n > 1 and (n .. " controles quitados") or "Control quitado") .. ". (Ctrl+Z para deshacer)")
    return true
  end
  local dx, dy = 0, 0
  if k == TECLA_IZQ then dx = -REJILLA elseif k == TECLA_DER then dx = REJILLA
  elseif k == TECLA_ARRIBA then dy = -REJILLA elseif k == TECLA_ABAJO then dy = REJILLA end
  if dx ~= 0 or dy ~= 0 then
    recordar()
    for _, c in ipairs(varios) do            -- todos los elegidos, a la vez
      c.x = math.max(0, math.min(c.x + dx, v.ancho - c.ancho))
      c.y = math.max(0, math.min(c.y + dy, v.alto - c.alto))
    end
    return true
  end
  return false
end

local function tecla_caracter(ch)
  if not editando then return false end
  if ch == 27 then editando = nil; estado("")
  elseif ch == 10 or ch == 13 then aplicar_edicion()
  elseif ch == 8 or ch == 127 then
    editando.nuevo = false
    local t = editando.texto
    local n = #t
    while n > 0 and (t:byte(n) & 0xC0) == 0x80 do n = n - 1 end   -- una letra entera (UTF-8)
    editando.texto = t:sub(1, math.max(0, n - 1))
  elseif ch >= 32 then
    if editando.nuevo then editando.texto = ""; editando.nuevo = false end
    if #editando.texto < 40 then editando.texto = editando.texto .. string.char(ch) end
  else return false end
  return true
end

redibujar()
local antes = 0
local sel_antes = nil
local function vuelta(evento)
  local cambio = false
  local w, h = gui.tamano_ventana()
  if w ~= ancho_v or h ~= alto_v then ancho_v, alto_v = w, h; cambio = true end

  local mx, my, botones = gui.raton()
  botones = botones or 0
  local pulsa = (botones & 1) ~= 0 and (antes & 1) == 0
  local suelta = (botones & 1) == 0 and (antes & 1) ~= 0
  antes = botones
  if mx and pulsa then
    cambio = true
    -- la linea de estado no es zona de clic: antes, un clic en ella
    -- bajo el inspector pulsaba botones que se habian quedado fuera de la vista
    if eligiendo_plantilla and my >= BARRA_H then
      local x, y, w, h = rect_plantillas()
      if dentro(mx, my, x, y, w, h) then
        local i = (my - y - 34) // FILA_PL + 1
        local pl = A.PLANTILLAS[i]
        if pl then
          p = pl[3]()
          sel, colocar, editando, archivo, menu_sel = nil, nil, nil, nil, nil
          varios = {}
          historial, rehechos = {}, {}
          guardado_como = A.escribir(p)
          cargar_codigo()
          estado("Plantilla " .. pl[1] .. ": lista. Pulsa Ejecutar para verla funcionar, o Código para mirarla.")
        end
      else estado("Plantilla: cancelado.") end
      eligiendo_plantilla = false
      return
    end
    if my < BARRA_H then clic_barra(mx)
    elseif my >= alto_v - ESTADO_H then -- nada
    elseif modo == "codigo" then clic_codigo(mx, my)
    elseif mx < PALETA_W then clic_paleta(mx, my)
    elseif mx >= ancho_v - INSP_W then clic_inspector(mx, my)
    else pulsar_formulario(mx, my) end
  elseif mx and arrastre and (botones & 1) ~= 0 then
    if arrastrar(mx, my) then cambio = true end
  end
  if suelta then
    if banda then
      local ox, oy = origen_form()
      local x1, y1 = math.min(banda.x0, banda.x1) - ox, math.min(banda.y0, banda.y1) - oy
      local x2, y2 = math.max(banda.x0, banda.x1) - ox, math.max(banda.y0, banda.y1) - oy
      for _, c in ipairs(p.controles) do
        if c.x >= x1 and c.y >= y1 and c.x + c.ancho <= x2 and c.y + c.alto <= y2 and not esta_elegido(c) then
          varios[#varios + 1] = c; sel = c
        end
      end
      banda = nil
      estado(#varios == 0 and "Nada dentro del recuadro." or (#varios .. " control(es) elegidos."))
      cambio = true
    end
    arrastre = nil
  end

  while true do
    local ch = gui.leer_tecla()
    if not ch or ch == 0 then break end
    if ch == 27 and eligiendo_plantilla then eligiendo_plantilla = false; estado("Plantilla: cancelado."); cambio = true
    elseif ch >= 32 and ch < 127 and ctrl_pulsado() then
      if atajo(ch) then cambio = true end
    elseif modo == "codigo" then if editor_caracter(ch) then cambio = true end
    elseif tecla_caracter(ch) then cambio = true end
  end
  while true do
    local k = gui.siguiente_codigo()
    if not k or k == 0 then break end
    if modo == "codigo" then if editor_codigo(k) then cambio = true end
    elseif tecla_codigo(k) then cambio = true end
  end

  local r = gui.rueda()
  if r and r ~= 0 and mx then
    if modo == "codigo" then
      ed.fila = math.max(1, math.min(#ed.lineas, ed.fila - r * 3)); ajustar_col(); cambio = true
    elseif mx >= ancho_v - INSP_W then desplazar_inspector(r); cambio = true end
  end
  if sel ~= sel_antes then insp_scroll = 0; sel_antes = sel end
  if vigilar_compilacion() then cambio = true end
  if cambio then redibujar() end
end

-- El bucle, propio (gui.bucle se cierra con la X sin preguntar): con cambios
-- sin guardar, la primera X avisa y la segunda cierra.
while true do
  nemo.pump()
  local ev = gui.sondear_evento()
  if ev == gui.EVENT_WINDOWCLOSE then
    if puede_descartar("Salir", "Pulsa otra vez la X") then break end
    redibujar()
  else
    vuelta(ev)
  end
end
