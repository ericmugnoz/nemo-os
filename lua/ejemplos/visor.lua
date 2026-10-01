-- visor.lua -- Nemo OS
-- Visor de HTML y Markdown para documentos locales. Un .md se
-- convierte a HTML con nemo_md.lua y se pinta igual. El motor (parser +
-- maquetacion) esta en nemo_html.lua (SISTEMA); esto es la ventana:
-- cargar el archivo, pintar las ordenes de dibujo con el scroll que
-- toque, y responder al raton y al teclado.
--
-- Uso:  run visor.lua CARPETA:archivo.html    (asi lo lanza el explorador
--                                             al hacer doble clic en un .html)
--       run visor.lua archivo.html            (se busca en la raiz, en MANUALES
--                                             y en DOCUMENTOS, por ese orden)
--
-- Controles:
--   rueda del raton, flechas arriba/abajo      scroll
--   clic en un enlace                          abre ese archivo (misma carpeta)
--   Retroceso                                  vuelve al documento anterior
--   + / -                                      zoom (todo el texto mas grande/pequeno)
--   Esc                                        cerrar
--
-- Los enlaces son a archivos de la misma carpeta (o "carpeta/archivo.html"
-- relativo a ella): esto es un visor local, no hay red por debajo.
--
-- Paginas con comportamiento: <script type="text/lua"> (ver la seccion
-- "Scripts" mas abajo). El codigo corre en este mismo interprete pero en
-- un entorno AISLADO -- sin nemo.syscall, sin require, sin acceso al
-- sistema -- con un objeto 'doc' para leer y cambiar la pagina.

local gui = require("nemo_gui")
local archivos = require("nemo_archivos")
local html = require("nemo_html")
local md = require("nemo_md")

local S = { GET_MOUSE = 34, GET_MOUSE_WHEEL = 45, MOUSE_HIT = 56, LOAD_IMAGE_EN = 253, GET_TICKS = 2,
            COPY_IMAGE = 93, RESIZE_IMAGE = 96, DRAW_IMAGE_RECT = 98 }

-- ---------------------------------------------------------------
-- Argumento: "carpeta:archivo.html" o "archivo.html"
-- ---------------------------------------------------------------
local function separar_ruta(arg1)
  if not arg1 or arg1 == "" then return nil end
  local carpeta, nombre = arg1:match("^(%d+):(.+)$")
  if carpeta then return tonumber(carpeta), nombre end
  return nil, arg1
end

local function buscar_en(nombre, carpeta)
  local e = archivos.buscar(nombre, carpeta, archivos.VOL_NEMOFS)
  return e and e.tipo ~= archivos.TIPO_CARPETA
end

-- La carpeta que contiene a la carpeta 'c' (para "../"). El sistema de
-- archivos no guarda el padre de cada carpeta, asi que se busca en el
-- arbol desde la raiz; las carpetas de Nemo OS son pocas y pequenas, y el
-- resultado se recuerda.
local padres = {}
local function carpeta_padre(c)
  if c == archivos.RAIZ then return archivos.RAIZ end
  if padres[c] then return padres[c] end
  local pendientes, vistas = { archivos.RAIZ }, {}
  while #pendientes > 0 do
    local p = table.remove(pendientes, 1)
    if not vistas[p] then
      vistas[p] = true
      for _, e in ipairs(archivos.listar(p, archivos.VOL_NEMOFS)) do
        if e.tipo == archivos.TIPO_CARPETA and e.nombre ~= "." and e.nombre ~= ".." then
          padres[e.inodo] = p
          if e.inodo == c then return p end
          pendientes[#pendientes + 1] = e.inodo
        end
      end
    end
  end
  return nil
end

-- Resuelve un href relativo a una carpeta: "x.html", "sub/x.html",
-- "../x.html", con ancla opcional ("x.html#seccion"). Devuelve carpeta,
-- nombre, ancla -- "misma", nil, ancla si es un ancla de esta pagina
-- ("#seccion") -- o nil si no existe.
local function resolver(href, carpeta)
  local ancla = href:match("#(.*)$")
  href = href:gsub("#.*$", "")
  if href == "" then return (ancla and "misma" or nil), nil, ancla end
  local c = carpeta
  for parte in href:gmatch("[^/]+") do
    if parte == ".." then
      c = carpeta_padre(c)
      if not c then return nil end
    elseif parte == "." then
      -- la misma carpeta
    elseif parte:find("%.") then
      return c, parte, ancla
    else
      local e = archivos.buscar(parte, c, archivos.VOL_NEMOFS)
      if not e or e.tipo ~= archivos.TIPO_CARPETA then return nil end
      c = e.inodo
    end
  end
  return nil
end

-- ---------------------------------------------------------------
-- Fuentes proporcionales del sistema (sans/serif/mono, negrita,
-- cursiva), una por combinacion de estilo, con sus avances para medir
-- en Lua sin syscalls. El kernel tiene 16 huecos: cuando se llenan se
-- libera la menos usada recientemente. Los tamanos se redondean a los
-- que existen en el paquete, asi la cache no se dispara.
-- ---------------------------------------------------------------
local TAMANOS = { 10, 12, 14, 16, 20, 24, 32, 40 }
local function tamano_disponible(px)
  local mejor, d = TAMANOS[1], math.huge
  for _, t in ipairs(TAMANOS) do local dd = math.abs(t - px); if dd < d then mejor, d = t, dd end end
  return mejor
end

local fuentes = {}       -- clave -> { handle=, avances=, uso= }
local contador_uso = 0
local MAX_FUENTES = 14   -- de los huecos del kernel, dejar margen a otros programas
-- Fuentes que el kernel no pudo cargar EN ESTE maquetado (tabla llena).
-- Antes se guardaba el fracaso como una fuente mas, sin identificador, y
-- ya no se volvia a intentar nunca: aunque luego quedaran huecos libres,
-- el documento seguia con la letra del sistema. Ahora se vacia en cada
-- maquetado, asi que se reintenta.
local fallidas = {}
local SIN_FUENTE = { handle = nil, avances = nil, uso = 0 }

local function clave_de(e)
  return tamano_disponible(e.tamano) .. (e.negrita and "b" or "") .. (e.cursiva and "i" or "") .. (e.mono and "m" or e.serif and "s" or "")
end

local function fuente(e)
  local k = clave_de(e)
  local f = fuentes[k]
  if not f then
    if fallidas[k] then return SIN_FUENTE end
    local n = 0; for _ in pairs(fuentes) do n = n + 1 end
    if n >= MAX_FUENTES then
      local peor, peor_uso = nil, math.huge
      for kk, ff in pairs(fuentes) do if ff.uso < peor_uso then peor, peor_uso = kk, ff.uso end end
      if peor then gui.liberar_fuente(fuentes[peor].handle); fuentes[peor] = nil end
    end
    local familia = e.mono and "mono" or e.serif and "serif" or "sans"
    local h = gui.cargar_fuente(familia, tamano_disponible(e.tamano), e.negrita, e.cursiva, false)
    if not h then fallidas[k] = true; return SIN_FUENTE end   -- se reintenta en el proximo maquetado
    f = { handle = h, avances = gui.avances_fuente(h) }
    fuentes[k] = f
  end
  contador_uso = contador_uso + 1; f.uso = contador_uso
  return f
end

-- Metrica para el motor: anchura real por texto, alto de linea y base.
-- Si no hay paquete de fuentes (cargar_fuente devuelve nil), cae a la
-- metrica fija de la 5x7 -- el visor sigue funcionando, mas feo.
local function metrica(e)
  local f = fuente(e)
  if not f.handle or not f.avances then return html.metrica_fija(e) end
  local av = f.avances
  return { ancho = function(t) return gui.medir_con_avances(av, t) end, alto_linea = av.alto_linea, ascent = av.ascent }
end

local function liberar_fuentes()
  for _, f in pairs(fuentes) do if f.handle then gui.liberar_fuente(f.handle) end end
  fuentes = {}
end

-- ---------------------------------------------------------------
-- Imagenes (.nimg) por nombre, relativas a la carpeta del documento
-- ---------------------------------------------------------------
local imagenes = {}   -- src -> { handle=, w=, h= } o false si no carga
local carpeta_doc = 0

local function imagen(src)
  local i = imagenes[src]
  if i ~= nil then return i or nil end
  local c, nombre = resolver(src, carpeta_doc)
  if not c then imagenes[src] = false; return nil end
  local h = nemo.syscall(S.LOAD_IMAGE_EN, nombre, c, 0, 0, 0)
  if h <= 0 then imagenes[src] = false; return nil end
  local w, hh = gui.tamano_imagen(h)
  imagenes[src] = { handle = h, w = w, h = hh }
  return imagenes[src]
end

-- La imagen al tamaño que le reserva la pagina (atributos width/height)
--. Antes se reservaba ese hueco pero se dibujaba la imagen a
-- su tamaño original, encima de lo que viniera despues. Ahora, la primera
-- vez, se hace una COPIA redimensionada (SYS_RESIZE_IMAGE, en el kernel) y
-- se guarda aparte. Si no se puede (el kernel no tiene huecos de imagen
-- libres, o el tamaño es excesivo), nil: quien dibuja la recorta.
local function imagen_a(src, w, h)
  local base = imagen(src)
  if not base or not w or not h or (w == base.w and h == base.h) then return base end
  local clave = src .. "@" .. w .. "x" .. h
  local i = imagenes[clave]
  if i ~= nil then return i or nil end
  local copia = nemo.syscall(S.COPY_IMAGE, base.handle)
  if copia <= 0 then imagenes[clave] = false; return nil end
  if nemo.syscall(S.RESIZE_IMAGE, copia, w, h) ~= 0 then
    gui.liberar_imagen(copia); imagenes[clave] = false; return nil
  end
  imagenes[clave] = { handle = copia, w = w, h = h }
  return imagenes[clave]
end

local function liberar_imagenes()
liberar_fuentes()
  for _, i in pairs(imagenes) do if i then gui.liberar_imagen(i.handle) end end
  imagenes = {}
end

-- ---------------------------------------------------------------
-- Scripts: <script type="text/lua"> en un entorno aislado
-- ---------------------------------------------------------------
-- Lo que una pagina puede hacer con 'doc':
--   doc.get(id)             -> elemento { texto (leer/escribir), color, fondo } o nil
--   doc.al_pulsar(id, fn)   -> fn() al hacer clic en el elemento (o boton) con ese id
--   doc.cada(ms, fn)        -> fn() cada ms milisegundos; devuelve un id; doc.parar(id)
--   doc.tecla(fn)           -> fn(caracter, codigo) al pulsar una tecla (que no use el visor)
--   doc.abrir(href)         -> navega a otro archivo, como un enlace
--   doc.titulo(texto)       -> cambia el titulo de la ventana
--   doc.hora()              -> "HH:MM:SS"; doc.fecha() -> { anio, mes, dia, hora, minuto, segundo }
--   doc.ticks()             -> milisegundos desde el arranque
--   doc.log(...)            -> por la UART (para depurar)
-- Cambiar texto o estilo marca la pagina para remaquetar y redibujar al
-- terminar el evento en curso. Los errores del script no tumban el visor:
-- se muestran en una barra roja arriba.
local script = { manejadores = {}, temporizadores = {}, tecla = nil, error = nil, sucio = false, siguiente_id = 1 }
local arbol_actual = nil   -- referencia para 'doc'
local navegar_pendiente = nil
local campo_foco = nil      -- id del campo de texto que recibe el teclado (o nil)

local function reiniciar_scripts()
  script.manejadores = {}; script.temporizadores = {}; script.tecla = nil; script.error = nil; script.sucio = false
  script.cambios = {}; script.envios = {}
  campo_foco = nil
end

local function hex(n) return string.format("#%06x", n & 0xFFFFFF) end

local function crear_doc()
  local doc = {}
  -- Un elemento del documento, visto desde el script. Vale igual para uno
  -- encontrado por su id (doc.get) que para uno recien creado (anadir):
  --   el.texto, el.color, el.fondo   leer y cambiar
  --   el.visible                     true/false (display: none)
  --   el.id, el.etiqueta             solo lectura
  --   el:anadir(etiqueta, texto, atributos) -> el hijo nuevo, al final
  --   el:borrar()                    lo quita del documento
  --   el:vaciar()                    le quita todo el contenido
  local function envolver(nodo)
    local metodos = {
      anadir = function(_, tag, texto, attrs)
        local hijo = html.crear(tag, texto, attrs)
        nodo.hijos = nodo.hijos or {}
        table.insert(nodo.hijos, hijo); script.sucio = true
        return envolver(hijo)
      end,
      borrar = function() if arbol_actual and html.quitar(arbol_actual, nodo) then script.sucio = true end end,
      vaciar = function() nodo.hijos = {}; script.sucio = true end,
    }
    return setmetatable({}, {
      __index = function(_, k)
        if metodos[k] then return metodos[k] end
        if k == "texto" then return html.texto_plano(nodo)
        elseif k == "id" then return nodo.attrs and nodo.attrs.id
        elseif k == "etiqueta" then return nodo.tag
        elseif k == "valor" then return nodo.attrs and nodo.attrs.value or ""
        elseif k == "marcado" then return nodo.attrs and nodo.attrs.checked ~= nil
        elseif k == "visible" then
          local st = (nodo.attrs and nodo.attrs.style or ""):lower()
          return not (nodo.attrs and nodo.attrs.hidden ~= nil) and not st:find("display%s*:%s*none")
        elseif k == "color" or k == "fondo" then
          local d = (nodo.attrs.style or ""):match((k == "color" and "color" or "background%-color") .. "%s*:%s*([^;]+)")
          return d and d:gsub("%s+$", "") or nil
        end
      end,
      __newindex = function(_, k, val)
        nodo.attrs = nodo.attrs or {}
        if k == "texto" then html.poner_texto(nodo, val); script.sucio = true
        elseif k == "valor" then nodo.attrs.value = tostring(val); script.sucio = true
        elseif k == "marcado" then nodo.attrs.checked = val and "" or nil; script.sucio = true
        elseif k == "visible" then
          nodo.attrs.hidden = nil
          local estilo = (nodo.attrs.style or ""):gsub("display%s*:%s*[^;]*;?", "")
          if not val then estilo = estilo .. (estilo:match(";%s*$") or estilo == "" and "" or ";") .. "display:none;" end
          nodo.attrs.style = estilo
          script.sucio = true
        elseif k == "color" or k == "fondo" then
          local prop = (k == "color") and "color" or "background-color"
          local valor = type(val) == "number" and hex(val) or tostring(val)
          local estilo = nodo.attrs.style or ""
          estilo = estilo:gsub(prop .. "%s*:%s*[^;]*;?", "")
          nodo.attrs.style = estilo .. (estilo:match(";%s*$") or estilo == "" and "" or ";") .. prop .. ":" .. valor .. ";"
          script.sucio = true
        end
      end,
    })
  end
  function doc.get(id)
    local nodo = arbol_actual and html.buscar_id(arbol_actual, id)
    return nodo and envolver(nodo) or nil
  end
  -- Desplazarse a un ancla de esta misma pagina: doc.ir("#seccion")
  function doc.ir(href) navegar_pendiente = href end
  -- Campos y formularios:
  --   doc.al_cambiar(id, fn)  fn(valor) al escribir en un campo, o
  --                           fn(true/false) al marcar una casilla
  --   doc.al_enviar(id, fn)   fn(valores) al enviar el <form> con ese id
  --                           (Enter en uno de sus campos, o su boton de
  --                           envio). 'valores' va por name, o por id.
  --   doc.foco(id)            da el foco a ese campo (nil: a ninguno)
  function doc.al_cambiar(id, fn) script.cambios[id] = fn end
  function doc.al_enviar(id, fn) script.envios[id] = fn end
  function doc.foco(id) campo_foco = id; script.sucio = true end
  function doc.al_pulsar(id, fn) script.manejadores[id] = fn end
  function doc.cada(ms, fn)
    local id = script.siguiente_id; script.siguiente_id = id + 1
    local periodo = math.max(1, math.floor((ms or 1000) / 10))   -- ticks de 10 ms
    script.temporizadores[id] = { periodo = periodo, proximo = nemo.syscall(S.GET_TICKS) + periodo, fn = fn }
    return id
  end
  function doc.parar(id) script.temporizadores[id] = nil end
  function doc.tecla(fn) script.tecla = fn end
  function doc.abrir(href) navegar_pendiente = href end
  function doc.titulo(t) gui.titulo(tostring(t)) end
  function doc.fecha() return gui.fecha_hora() end
  function doc.hora() local f = gui.fecha_hora(); return string.format("%02d:%02d:%02d", f.hora, f.minuto, f.segundo) end
  function doc.ticks() return nemo.syscall(S.GET_TICKS) * 10 end
  function doc.log(...) local t = {}; for i = 1, select("#", ...) do t[#t+1] = tostring((select(i, ...))) end; nemo.syscall(11, table.concat(t, " ") .. "\n") end
  return doc
end

-- Entorno aislado: solo lo inofensivo del lenguaje, mas 'doc'
local function crear_entorno()
  local env = {
    doc = crear_doc(),
    string = string, math = math, table = table, utf8 = utf8,
    tostring = tostring, tonumber = tonumber, type = type, pairs = pairs, ipairs = ipairs, next = next,
    select = select, pcall = pcall, error = error, assert = assert, unpack = table.unpack,
    print = function(...) local t = {}; for i = 1, select("#", ...) do t[#t+1] = tostring((select(i, ...))) end; nemo.syscall(11, table.concat(t, "\t") .. "\n") end,
  }
  env._G = env
  return env
end

local function ejecutar_scripts()
  reiniciar_scripts()
  if not arbol_actual then return end
  local codigos = html.scripts_lua(arbol_actual)
  if #codigos == 0 then return end
  local env = crear_entorno()
  for i, codigo in ipairs(codigos) do
    local fn, err = load(codigo, "=script" .. i, "t", env)
    if not fn then script.error = "script " .. i .. ": " .. tostring(err); return end
    local ok, err2 = pcall(fn)
    if not ok then script.error = "script " .. i .. ": " .. tostring(err2); return end
  end
end

-- Llama a un manejador protegido; devuelve true si hubo que redibujar
local function llamar(fn, ...)
  local ok, err = pcall(fn, ...)
  if not ok then script.error = tostring(err); return true end
  return script.sucio or navegar_pendiente ~= nil
end

-- ---------------------------------------------------------------
-- Estado del visor
-- ---------------------------------------------------------------
local ANCHO_BARRA = 10
local ventana_w, ventana_h = 640, 440
local documento = nil      -- resultado de html.maquetar
local arbol = nil
local scroll = 0
local zoom = 1
local historial = {}       -- { carpeta=, nombre= }
local actual = nil         -- { carpeta=, nombre= }
local mensaje = nil        -- texto de error a mostrar en vez del documento

local function maquetar()
  if not arbol then return end
  fallidas = {}   -- reintentar las fuentes que no se pudieron cargar la vez anterior
  documento = html.maquetar(arbol, {
    ancho = ventana_w - ANCHO_BARRA,
    zoom = zoom,
    foco = campo_foco,
    metrica = metrica,
    tamano_imagen = function(src) local i = imagen(src); if i then return i.w, i.h end end,
  })
  local max_scroll = math.max(0, documento.alto - ventana_h)
  if scroll > max_scroll then scroll = max_scroll end
  if scroll < 0 then scroll = 0 end
  gui.titulo((documento.titulo and (documento.titulo .. " - ") or "") .. "Visor")
end

local function es_markdown(nombre)
  local n = nombre:lower()
  return n:sub(-3) == ".md" or n:sub(-9) == ".markdown" or n:sub(-4) == ".txt"
end

local function cargar_sin_proteger(carpeta, nombre, guardar_historial)
  -- hasta 1MB: una guia larga en Markdown pasa de los 64KB por defecto
  local contenido = archivos.leer_en(nombre, carpeta, archivos.VOL_NEMOFS, 1024 * 1024)
  if not contenido then
    mensaje = "No se pudo abrir: " .. nombre
    return false
  end
  -- Markdown (y .txt, que se lee bien como Markdown sin formato) -> HTML
  if es_markdown(nombre) then contenido = md.documento(contenido) end
  if guardar_historial and actual then historial[#historial + 1] = actual end
  actual = { carpeta = carpeta, nombre = nombre }
  carpeta_doc = carpeta
  liberar_imagenes()
  mensaje = nil
  arbol = html.parsear(contenido)
  arbol_actual = arbol
  scroll = 0
  ejecutar_scripts()
  maquetar()
  return true
end

-- Si algo falla al abrir un documento -- casi siempre, que no cabe en la
-- memoria del visor (4 MB) --, se muestra un aviso en la ventana en vez
-- de dejar que el error cierre el visor entero. Antes, la guia de Lua,
-- demasiado grande, cerraba la ventana sin decir nada.
local function cargar(carpeta, nombre, guardar_historial)
  local ok, resultado = pcall(cargar_sin_proteger, carpeta, nombre, guardar_historial)
  if ok then return resultado end
  arbol, documento = nil, nil
  collectgarbage()
  local motivo = tostring(resultado)
  if motivo:find("not enough memory") then
    mensaje = "\"" .. nombre .. "\" es demasiado grande para la memoria del visor."
  else
    mensaje = "No se pudo mostrar \"" .. nombre .. "\": " .. motivo
  end
  return false
end

-- ---- Campos y formularios ----

local function nodo_de(id) return arbol_actual and id and html.buscar_id(arbol_actual, id) end

-- El <form> que contiene a un nodo, o nil.
local function formulario_de(nodo)
  local p = arbol_actual and html.buscar_padre(arbol_actual, nodo)
  while p do
    if p.tag == "form" then return p end
    p = html.buscar_padre(arbol_actual, p)
  end
  return nil
end

-- Los campos editables (texto y casillas), en el orden del documento.
local function campos_en(nodo, lista)
  lista = lista or {}
  if nodo.tag == "input" and nodo.attrs and nodo.attrs.id then
    local t = (nodo.attrs.type or ""):lower()
    if html.TIPOS_TEXTO[t] or t == "checkbox" then lista[#lista + 1] = nodo end
  end
  for _, h in ipairs(nodo.hijos or {}) do campos_en(h, lista) end
  return lista
end

-- Envia el formulario de un nodo: llama al manejador de doc.al_enviar con
-- los valores de sus campos (por name, o por id). true si hubo que redibujar.
local function enviar(nodo)
  local form = nodo and formulario_de(nodo)
  if not form then return false end
  local fn = script.envios[form.attrs.id or ""] or script.envios[form.attrs.name or ""]
  if not fn then return false end
  local valores = {}
  for _, c in ipairs(campos_en(form)) do
    local clave = c.attrs.name or c.attrs.id
    if (c.attrs.type or ""):lower() == "checkbox" then valores[clave] = c.attrs.checked ~= nil
    else valores[clave] = c.attrs.value or "" end
  end
  return llamar(fn, valores)
end

-- Un cambio en un campo: avisar al script si lo pidio.
local function cambiado(id, valor)
  local fn = script.cambios[id]
  script.sucio = true
  if fn then llamar(fn, valor) end
end

-- Una tecla con un campo de texto enfocado. Devuelve true si la consumio.
local function tecla_en_campo(tecla)
  local nodo = nodo_de(campo_foco)
  if not nodo then campo_foco = nil; return false end
  local valor = nodo.attrs.value or ""
  if tecla == gui.TECLA_ESC then campo_foco = nil; script.sucio = true; return true end
  if tecla == gui.TECLA_ENTER or tecla == 13 then enviar(nodo); script.sucio = true; return true end
  if tecla == gui.TECLA_TAB then
    local lista = campos_en(arbol_actual)
    for i, c in ipairs(lista) do
      if c == nodo then campo_foco = (lista[i % #lista + 1]).attrs.id; break end
    end
    script.sucio = true; return true
  end
  if tecla == gui.TECLA_BACKSPACE then
    if valor ~= "" then
      local ultimo = utf8.offset(valor, -1) or #valor   -- borrar un caracter entero, no un byte
      nodo.attrs.value = valor:sub(1, ultimo - 1)
      cambiado(campo_foco, nodo.attrs.value)
    end
    return true
  end
  local ch
  if tecla >= 32 and tecla < 127 then ch = string.char(tecla)
  elseif tecla >= 160 and tecla <= 255 then ch = utf8.char(tecla) end   -- Latin-1 (ñ, á...) a UTF-8
  if ch then
    local max = tonumber(nodo.attrs.maxlength)
    if not max or (utf8.len(valor) or #valor) < max then
      nodo.attrs.value = valor .. ch
      cambiado(campo_foco, nodo.attrs.value)
    end
    return true
  end
  return false   -- flechas y demas: para el visor
end

-- Un clic en una zona con id (campo, casilla, boton...). Devuelve true si
-- hubo que redibujar.
local function clic_en_zona(id, zona)
  local foco_antes = campo_foco
  campo_foco = (zona and zona.campo) and id or nil
  local hubo = campo_foco ~= foco_antes
  if zona and zona.casilla then
    local nodo = nodo_de(id)
    if nodo then
      nodo.attrs.checked = (nodo.attrs.checked == nil) and "" or nil
      cambiado(id, nodo.attrs.checked ~= nil)
      hubo = true
    end
  end
  if id and script.manejadores[id] then
    if llamar(script.manejadores[id]) then hubo = true end
  end
  if zona and zona.boton then
    local nodo = nodo_de(id)
    local t = nodo and (nodo.attrs.type or ""):lower()
    -- dentro de un formulario, un <button> sin type o type=submit, o un
    -- <input type=submit>, lo envian
    if nodo and ((nodo.tag == "button" and (t == "" or t == "submit")) or (nodo.tag == "input" and t == "submit")) then
      if enviar(nodo) then hubo = true end
    end
  end
  if hubo then script.sucio = true end
  return hubo
end

-- Desplazar el documento hasta un ancla (id o <a name>) de la pagina actual.
local function ir_a_ancla(nombre)
  local y = documento and documento.anclas and documento.anclas[nombre]
  if not y then mensaje = "Ancla no encontrada: #" .. tostring(nombre); return end
  -- Aqui mismo, con los limites de desplazar() -- que esta definida MAS
  -- ABAJO en el archivo y desde aqui no se ve (una funcion local solo
  -- existe a partir de su definicion). Llamarla cerraba el visor al pulsar
  -- un enlace a un ancla.
  local max_scroll = math.max(0, documento.alto - ventana_h)
  scroll = math.max(0, math.min(max_scroll, y - 8))
  -- (sin redibujar aqui: tambien esta definida mas abajo, y quien sigue
  -- el enlace -- el clic, o doc.abrir/doc.ir -- redibuja justo despues)
end

-- Seguir un enlace: otra pagina, un ancla de esta, o las dos cosas.
-- Devuelve true si lo encontro.
local function seguir_enlace(href)
  local c, n, ancla = resolver(href, actual and actual.carpeta or archivos.RAIZ)
  if c == "misma" then ir_a_ancla(ancla); return true end
  if c and buscar_en(n, c) then
    if not (actual and actual.carpeta == c and actual.nombre == n) then cargar(c, n, true) end
    if ancla then ir_a_ancla(ancla) end
    return true
  end
  return false
end

local function volver()
  local h = table.remove(historial)
  if h then cargar(h.carpeta, h.nombre, false) end
end

-- ---------------------------------------------------------------
-- Dibujo
-- ---------------------------------------------------------------
local function redibujar()
  local fondo = documento and documento.fondo or html.COLOR_FONDO
  gui.rect(0, 0, ventana_w, ventana_h, fondo)

  if mensaje then
    gui.poner_fuente(fuente({ tamano = 14, negrita = true }).handle)
    gui.texto(10, 10, mensaje, 0xCC0000)
    gui.poner_fuente(0)
    return
  end
  if not documento then return end

  local fuente_actual = nil
  for _, o in ipairs(documento.ops) do
    local y = o.y - scroll
    local h = o.h or (o.tamano and math.floor(o.tamano * 1.3)) or 0
    if y + h >= 0 and y < ventana_h then
      if o.tipo == "rect" then
        gui.rect(o.x, y, o.w, o.h, o.color)
      elseif o.tipo == "texto" then
        local f = fuente(o).handle or 0
        if f ~= fuente_actual then gui.poner_fuente(f); fuente_actual = f end
        gui.texto(o.x, y, o.texto, o.color)
      elseif o.tipo == "imagen" then
        local i = imagen_a(o.src, o.w, o.h)
        if i and i.w == o.w and i.h == o.h then
          gui.dibujar_imagen(i.handle, o.x, y)
        else
          -- sin copia a escala: la original, recortada a su hueco, para no
          -- pintar encima de lo que venga despues
          i = imagen(o.src)
          if i then
            local rw, rh = math.min(i.w, o.w or i.w), math.min(i.h, o.h or i.h)
            nemo.syscall(S.DRAW_IMAGE_RECT, i.handle, o.x, y, 0, (rw << 16) | rh)
          end
        end
      end
      -- "enlace": invisible, solo para el clic
    end
  end

  gui.poner_fuente(0)   -- dejar la fuente de sistema para lo demas

  if script.error then
    -- barra roja con el error del script, por encima del contenido
    gui.rect(0, 0, ventana_w, 18, 0xCC2222)
    gui.poner_fuente(fuente({ tamano = 12, negrita = true }).handle)
    gui.texto(6, 2, "Error en script: " .. script.error:sub(1, 90), 0xFFFFFF)
    gui.poner_fuente(0)
  end

  -- barra de desplazamiento
  if documento.alto > ventana_h then
    local bx = ventana_w - ANCHO_BARRA
    gui.rect(bx, 0, ANCHO_BARRA, ventana_h, 0xE0E0E0)
    local alto_pulgar = math.max(12, math.floor(ventana_h * ventana_h / documento.alto))
    local y_pulgar = math.floor(scroll * (ventana_h - alto_pulgar) / (documento.alto - ventana_h))
    gui.rect(bx + 2, y_pulgar, ANCHO_BARRA - 4, alto_pulgar, 0x808080)
  end
end

local function desplazar(delta)
  if not documento then return end
  local max_scroll = math.max(0, documento.alto - ventana_h)
  local nuevo = math.max(0, math.min(max_scroll, scroll + delta))
  if nuevo ~= scroll then scroll = nuevo; redibujar() end
end

-- ---------------------------------------------------------------
-- Arranque
-- ---------------------------------------------------------------
gui.crear_ventana("Visor", 60, 40, ventana_w, ventana_h)
ventana_w, ventana_h = gui.tamano_ventana()

do
  local carpeta, nombre = separar_ruta(arg and arg[1])
  if not nombre then
    mensaje = "Uso: run visor.lua archivo.html (o .md)"
  elseif carpeta then
    cargar(carpeta, nombre, false)
  else
    -- Sin carpeta: la raiz, y si no, MANUALES y DOCUMENTOS por ese orden.
    -- MANUALES es nueva: las guias.md se movieron ahi porque
    -- veinte de ellas dejaban DOCUMENTOS, que es la carpeta del usuario, sin
    -- sitio para lo suyo. DOCUMENTOS se sigue mirando: estan las paginas de
    -- ejemplo, y en una tarjeta vieja tambien las guias.
    local hecho = false
    if buscar_en(nombre, archivos.RAIZ) then
      cargar(archivos.RAIZ, nombre, false); hecho = true
    else
      for _, padre in ipairs({ "MANUALES", "DOCUMENTOS" }) do
        local d = archivos.buscar(padre, archivos.RAIZ, archivos.VOL_NEMOFS)
        if d and buscar_en(nombre, d.inodo) then
          cargar(d.inodo, nombre, false); hecho = true; break
        end
      end
    end
    if not hecho then mensaje = "No se encuentra: " .. nombre end
  end
end
redibujar()

-- ---------------------------------------------------------------
-- Bucle
-- ---------------------------------------------------------------
gui.bucle(function(evento, fuente_ev, datos)
  if evento == gui.EVENT_WINDOWSIZE then
    ventana_w, ventana_h = gui.tamano_ventana()
    maquetar(); redibujar()
    return true
  end

  -- rueda: positivo = arriba
  local rueda = nemo.syscall(S.GET_MOUSE_WHEEL)
  if rueda ~= 0 then desplazar(-rueda * 3 * 18 * zoom) end

  -- temporizadores de la pagina
  local hay_cambios = false
  if next(script.temporizadores) then
    local ahora = nemo.syscall(S.GET_TICKS)
    for _, t in pairs(script.temporizadores) do
      if ahora >= t.proximo then
        t.proximo = ahora + t.periodo
        if llamar(t.fn) then hay_cambios = true end
      end
    end
  end

  -- clic izquierdo: ¿sobre un enlace, o sobre una zona con manejador?
  if nemo.syscall(S.MOUSE_HIT, 1) == 1 and documento then
    local m = nemo.syscall(S.GET_MOUSE)
    if m ~= -1 then
      -- (x<<32) | (y<<16) | botones -- ver SYS_GET_MOUSE en syscall.c
      local mx, my = (m >> 32) & 0xFFFF, (m >> 16) & 0xFFFF
      local id, _, zona = html.zona_en(documento, mx, my + scroll)
      if clic_en_zona(id, zona) then hay_cambios = true end
      local href = html.enlace_en(documento, mx, my + scroll)
      if href then
        if not seguir_enlace(href) then mensaje = "Enlace roto: " .. href .. "   (Retroceso para seguir)" end
        redibujar()
      end
    end
  end

  local tecla = gui.leer_tecla()
  -- con un campo de texto enfocado, las teclas son para el campo (salvo
  -- las flechas, que siguen desplazando la pagina)
  if tecla ~= 0 and campo_foco then
    if tecla_en_campo(tecla) then tecla = 0; hay_cambios = true end
  end
  if tecla ~= 0 and script.tecla and tecla >= 32 and tecla < 127 then
    -- teclas imprimibles: primero al script de la pagina (las de control
    -- -- flechas, Retroceso, Esc, +/- -- se las queda el visor)
    if llamar(script.tecla, string.char(tecla), tecla) then hay_cambios = true end
    tecla = 0
  end
  if tecla ~= 0 then
    if tecla == 0x11 then desplazar(-18 * 3 * zoom)
    elseif tecla == 0x12 then desplazar(18 * 3 * zoom)
    elseif tecla == gui.TECLA_BACKSPACE then
      if mensaje and actual then mensaje = nil; redibujar()   -- quitar un aviso de enlace roto
      else volver(); redibujar() end
    elseif tecla == 43 or tecla == 61 then   -- '+' o '='
      if zoom < 3 then zoom = zoom + 1; maquetar(); redibujar() end
    elseif tecla == 45 then                  -- '-'
      if zoom > 1 then zoom = zoom - 1; maquetar(); redibujar() end
    elseif tecla == gui.TECLA_ESC then
      return false
    end
  end

  -- lo que los scripts hayan cambiado en este turno
  if navegar_pendiente then
    local href = navegar_pendiente; navegar_pendiente = nil
    if not seguir_enlace(href) then mensaje = "Enlace roto: " .. href end
    if script.sucio then script.sucio = false; maquetar() end
    redibujar()
  elseif hay_cambios then
    if script.sucio then script.sucio = false; maquetar() end
    redibujar()
  end
  return true
end)

liberar_imagenes()
-- Devolver tambien las fuentes al kernel al salir. El kernel ya recupera
-- las de un programa que termina sin liberarlas, pero solo cuando alguien
-- necesita el hueco; asi quedan libres en el acto.
for _, f in pairs(fuentes) do gui.liberar_fuente(f.handle) end
