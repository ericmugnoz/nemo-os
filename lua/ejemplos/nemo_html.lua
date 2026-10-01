-- nemo_html.lua -- Nemo OS
-- Motor de HTML: parser + maquetacion. NO dibuja nada: produce una
-- lista de "ordenes de dibujo" (texto, rectangulos, imagenes, cajas de
-- enlace) con coordenadas absolutas dentro de un documento de ancho
-- dado. visor.lua es quien las pinta con nemo_gui, con el scroll que
-- toque. Separarlo asi permite probar el motor entero en el host, sin
-- Nemo OS, sin pantalla.
--
-- Subconjunto de HTML (a proposito -- esto es un visor de documentos
-- locales, no un navegador):
--   estructura: html, head, title, body, div, section, article, header,
--               footer, nav, main, aside, center
--   bloques:    h1..h6, p, br, hr, ul, ol, li, blockquote, pre
--   inline:     b, strong, i, em, u, code, span, small, big, font, a, img
--   estilos:    atributo style="" y bloque <style> con selectores por
--               etiqueta, .clase y #id, para: color, background-color,
--               text-align, font-size (px), font-weight, text-decoration
--   otros:      entidades (&amp; &lt; &gt; &quot; &apos; &nbsp; &#NN;
--               &#xHH;), comentarios, <!DOCTYPE>
-- Lo que NO hay (todavia): tablas, formularios, JavaScript (ni se
-- pretende), CSS de cajas (margenes/padding personalizados), flotantes.
--
-- Tipografia: los tamanos son en PIXELES (font-size, encabezados...) y la
-- metrica real la pone quien llama, con opciones.metrica(estilo) ->
-- { ancho = function(texto), alto_linea = px, ascent = px }. visor.lua la
-- saca de las fuentes proporcionales del sistema (sans/mono, negrita y
-- cursiva de verdad, antialiasing). Sin metrica (pruebas en el host, o
-- un sistema sin paquete de fuentes) se usa la de la 5x7 escalada por
-- enteros, y ahi la cursiva no se ve.

local M = {}

-- ---------------------------------------------------------------
-- Metrica de la fuente (la misma que el kernel: text.c / font5x7.c)
-- ---------------------------------------------------------------
M.ANCHO_GLIFO = 5
M.ALTO_GLIFO  = 7
M.AVANCE      = 6      -- 5 de glifo + 1 de separacion, por escala

-- Metrica de respaldo: la 5x7 escalada por enteros (10px ~ 1x).
function M.escala_para(tamano) return math.max(1, math.floor(tamano / 10 + 0.5)) end
function M.ancho_texto(s, escala) return #s * M.AVANCE * escala end
function M.alto_linea(escala)      return M.ALTO_GLIFO * escala + 3 * escala end
function M.metrica_fija(estilo)
  local e = M.escala_para(estilo.tamano)
  return { ancho = function(t) return M.ancho_texto(t, e) end, alto_linea = M.alto_linea(e), ascent = M.ALTO_GLIFO * e, escala = e }
end

-- Tamanos por defecto, en pixeles
M.TAMANO_BASE = 14
M.TAMANO_ENCABEZADO = { h1 = 28, h2 = 22, h3 = 18, h4 = 16, h5 = 14, h6 = 12 }
M.TAMANO_FONT_SIZE  = { 10, 12, 14, 16, 20, 24, 32 }   -- <font size=1..7>

-- ---------------------------------------------------------------
-- Colores
-- ---------------------------------------------------------------
local COLORES = {
  black=0x000000, white=0xFFFFFF, red=0xFF0000, green=0x008000, blue=0x0000FF,
  yellow=0xFFFF00, orange=0xFFA500, purple=0x800080, gray=0x808080, grey=0x808080,
  silver=0xC0C0C0, maroon=0x800000, navy=0x000080, teal=0x008080, olive=0x808000,
  lime=0x00FF00, aqua=0x00FFFF, cyan=0x00FFFF, fuchsia=0xFF00FF, magenta=0xFF00FF,
  brown=0xA52A2A, pink=0xFFC0CB, gold=0xFFD700, darkgray=0xA9A9A9, darkgrey=0xA9A9A9,
  lightgray=0xD3D3D3, lightgrey=0xD3D3D3, darkblue=0x00008B, darkgreen=0x006400,
  darkred=0x8B0000, crimson=0xDC143C, coral=0xFF7F50, salmon=0xFA8072, tomato=0xFF6347,
  skyblue=0x87CEEB, steelblue=0x4682B4, royalblue=0x4169E1, indigo=0x4B0082,
  violet=0xEE82EE, beige=0xF5F5DC, ivory=0xFFFFF0, khaki=0xF0E68C, tan=0xD2B48C,
  chocolate=0xD2691E, firebrick=0xB22222, forestgreen=0x228B22, seagreen=0x2E8B57,
  slategray=0x708090, slategrey=0x708090, dimgray=0x696969, dimgrey=0x696969,
  whitesmoke=0xF5F5F5, gainsboro=0xDCDCDC, lavender=0xE6E6FA, mintcream=0xF5FFFA,
  transparent=nil,
}
M.COLORES = COLORES

function M.color(s)
  if not s then return nil end
  s = s:lower():gsub("^%s+",""):gsub("%s+$","")
  if s:sub(1,1) == "#" then
    local h = s:sub(2)
    if #h == 3 then h = h:sub(1,1):rep(2) .. h:sub(2,2):rep(2) .. h:sub(3,3):rep(2) end
    if #h == 6 and not h:find("[^%x]") then return tonumber(h, 16) end
    return nil
  end
  local r,g,b = s:match("^rgb%(%s*(%d+)%s*,%s*(%d+)%s*,%s*(%d+)%s*%)$")
  if r then return (math.min(255,tonumber(r))<<16) | (math.min(255,tonumber(g))<<8) | math.min(255,tonumber(b)) end
  return COLORES[s]
end

-- ---------------------------------------------------------------
-- Entidades
-- ---------------------------------------------------------------
local ENTIDADES = { amp="&", lt="<", gt=">", quot='"', apos="'", nbsp=" ", copy="(c)", reg="(R)",
                    hellip="...", mdash="--", ndash="-", laquo="<<", raquo=">>", middot="*", bull="*",
                    trade="(TM)", deg="o", euro="EUR", times="x", divide="/", larr="<-", rarr="->" }

local function decodificar_entidades(s)
  return (s:gsub("&(#?[%w]+);", function(e)
    if e:sub(1,2) == "#x" or e:sub(1,2) == "#X" then
      local n = tonumber(e:sub(3), 16); return (n and n < 128) and string.char(n) or "?"
    elseif e:sub(1,1) == "#" then
      local n = tonumber(e:sub(2)); return (n and n < 128) and string.char(n) or "?"
    end
    return ENTIDADES[e] or ("&" .. e .. ";")
  end))
end
M.decodificar_entidades = decodificar_entidades

-- ---------------------------------------------------------------
-- Parser: HTML -> arbol { tag=, attrs={}, hijos={} } / { texto= }
-- Tolerante: etiquetas sin cerrar, mayusculas, atributos sin comillas.
-- ---------------------------------------------------------------
local VACIAS = { br=true, hr=true, img=true, meta=true, link=true, input=true, wbr=true }
-- Etiquetas que cierran implicitamente a otras abiertas (lo minimo util)
local CIERRA_P = { p=true, div=true, h1=true, h2=true, h3=true, h4=true, h5=true, h6=true,
                   ul=true, ol=true, li=true, blockquote=true, pre=true, hr=true, table=true,
                   section=true, article=true, header=true, footer=true, nav=true, main=true, aside=true }
local BLOQUES = { html=true, head=true, body=true, div=true, section=true, article=true, header=true,
                  footer=true, nav=true, main=true, aside=true, center=true,
                  h1=true, h2=true, h3=true, h4=true, h5=true, h6=true, p=true, ul=true, ol=true,
                  li=true, blockquote=true, pre=true, hr=true, title=true, style=true, script=true,
                  table=true, thead=true, tbody=true, tfoot=true, tr=true, td=true, th=true, caption=true }
M.BLOQUES = BLOQUES

local function parsear_atributos(s)
  -- Escaner a mano (los patrones de Lua no tienen alternativas):
  --   nombre, nombre=valor, nombre="valor", nombre='valor'
  local attrs = {}
  local i, n = 1, #s
  while i <= n do
    local ini, fin = s:find("^%s*([%w%-:]+)", i)
    if not ini then break end
    local nombre = s:sub(ini, fin):gsub("^%s+", ""):lower()
    i = fin + 1
    local eq_ini, eq_fin = s:find("^%s*=%s*", i)
    if eq_ini then
      i = eq_fin + 1
      local c = s:sub(i, i)
      local valor
      if c == '"' or c == "'" then
        local cierre = s:find(c, i + 1, true) or (n + 1)
        valor = s:sub(i + 1, cierre - 1)
        i = cierre + 1
      else
        local vi, vf = s:find("^[^%s]+", i)
        valor = vi and s:sub(vi, vf) or ""
        i = (vf or i) + 1
      end
      attrs[nombre] = decodificar_entidades(valor)
    else
      attrs[nombre] = ""
    end
  end
  return attrs
end

function M.parsear(html)
  local raiz = { tag = "#raiz", attrs = {}, hijos = {} }
  local pila = { raiz }
  local function actual() return pila[#pila] end
  local function anadir(nodo) table.insert(actual().hijos, nodo); return nodo end
  local function cerrar_hasta(tag)
    for i = #pila, 2, -1 do
      if pila[i].tag == tag then
        for _ = #pila, i, -1 do table.remove(pila) end
        return true
      end
    end
    return false
  end

  local i, n = 1, #html
  while i <= n do
    local lt = html:find("<", i, true)
    if not lt then
      anadir({ texto = decodificar_entidades(html:sub(i)) }); break
    end
    if lt > i then anadir({ texto = decodificar_entidades(html:sub(i, lt - 1)) }) end

    if html:sub(lt, lt + 3) == "<!--" then
      local fin = html:find("-->", lt + 4, true) or n
      i = fin + 3
    elseif html:sub(lt, lt + 1) == "<!" then
      local fin = html:find(">", lt, true) or n
      i = fin + 1
    elseif html:sub(lt + 1, lt + 1) == "/" then
      local fin = html:find(">", lt, true) or n
      local tag = html:sub(lt + 2, fin - 1):match("^%s*([%w]+)")
      if tag then cerrar_hasta(tag:lower()) end
      i = fin + 1
    else
      local fin = html:find(">", lt, true) or n
      local cuerpo = html:sub(lt + 1, fin - 1)
      local autocierre = cuerpo:sub(-1) == "/"
      if autocierre then cuerpo = cuerpo:sub(1, -2) end
      local tag, resto = cuerpo:match("^%s*([%w]+)(.*)$")
      i = fin + 1
      if tag then
        tag = tag:lower()
        -- cierre implicito de <p> abierto al abrir un bloque
        if CIERRA_P[tag] and actual().tag == "p" then table.remove(pila) end
        -- <li> abre otro <li>: cerrar el anterior
        if tag == "li" and actual().tag == "li" then table.remove(pila) end
        -- celdas y filas de tabla sin cerrar: <td> cierra la celda abierta,
        -- <tr> cierra celda y fila abiertas (HTML escrito a mano las omite mucho)
        if tag == "td" or tag == "th" then
          while actual().tag == "td" or actual().tag == "th" do table.remove(pila) end
        elseif tag == "tr" then
          while actual().tag == "td" or actual().tag == "th" or actual().tag == "tr" do table.remove(pila) end
        elseif tag == "table" then
          -- una tabla nueva nunca va dentro de una celda sin cerrar (no hay anidadas aqui)
        end
        local nodo = anadir({ tag = tag, attrs = parsear_atributos(resto or ""), hijos = {} })
        if tag == "script" or tag == "style" then
          -- contenido crudo hasta la etiqueta de cierre, sin parsear
          local cierre = html:lower():find("</" .. tag, i, true)
          local hasta = cierre and (cierre - 1) or n
          nodo.hijos[1] = { texto = html:sub(i, hasta) }
          local fin2 = html:find(">", cierre or n, true) or n
          i = fin2 + 1
        elseif not VACIAS[tag] and not autocierre then
          table.insert(pila, nodo)
        end
      end
    end
  end
  return raiz
end

-- Busca el primer nodo con una etiqueta dada (para <title>, <body>, <style>)
function M.buscar(nodo, tag)
  if nodo.tag == tag then return nodo end
  for _, h in ipairs(nodo.hijos or {}) do
    local r = M.buscar(h, tag); if r then return r end
  end
  return nil
end

function M.texto_plano(nodo)
  if nodo.texto then return nodo.texto end
  local t = {}
  for _, h in ipairs(nodo.hijos or {}) do t[#t+1] = M.texto_plano(h) end
  return table.concat(t)
end

-- ---------------------------------------------------------------
-- CSS (subconjunto): <style> y style=""
-- ---------------------------------------------------------------
local function parsear_declaraciones(s)
  local d = {}
  for prop, val in s:gmatch("([%w%-]+)%s*:%s*([^;]+)") do
    d[prop:lower():gsub("%s+$","")] = val:gsub("^%s+",""):gsub("%s+$","")
  end
  return d
end

-- Devuelve lista de reglas { sel = {tag=, clase=, id=}, decl = {} }
function M.parsear_css(css)
  css = css:gsub("/%*.-%*/", "")
  local reglas = {}
  for selectores, cuerpo in css:gmatch("([^{}]+)%s*{([^}]*)}") do
    local decl = parsear_declaraciones(cuerpo)
    for sel_crudo in selectores:gmatch("[^,]+") do
      local sel = sel_crudo:gsub("^%s+",""):gsub("%s+$","")
      local r = { decl = decl }
      local tag, resto = sel:match("^([%w]*)(.*)$")
      if tag ~= "" then r.tag = tag:lower() end
      r.clase = resto:match("%.([%w%-_]+)")
      r.id = resto:match("#([%w%-_]+)")
      if r.tag or r.clase or r.id then reglas[#reglas+1] = r end
    end
  end
  return reglas
end

-- Una longitud de CSS en pixeles: "12px", "1.5em", "10pt", "0". nil si no se entiende.
local function longitud(v, zoom, tamano)
  v = v and v:lower():gsub("^%s+", ""):gsub("%s+$", "")
  if not v or v == "" then return nil end
  if v == "0" or v == "auto" then return 0 end
  local n = tonumber(v:match("^(%-?%d*%.?%d+)%s*px$")); if n then return math.floor(n * zoom + 0.5) end
  n = tonumber(v:match("^(%-?%d*%.?%d+)%s*em$"));        if n then return math.floor(n * (tamano or 16) + 0.5) end
  n = tonumber(v:match("^(%-?%d*%.?%d+)%s*pt$"));        if n then return math.floor(n * 4 / 3 * zoom + 0.5) end
  n = tonumber(v);                                        if n then return math.floor(n * zoom + 0.5) end
  return nil
end

-- margin / padding: la forma corta con 1 a 4 valores (arriba,
-- derecha, abajo, izquierda, como en CSS) y las largas (margin-top...).
-- Resultado: { t=, r=, b=, l= }, con nil en los lados sin especificar.
local function cajas(d, prop, actual, zoom, tamano)
  local c = actual
  local corta = d[prop]
  if corta then
    local v = {}
    for parte in corta:gmatch("%S+") do v[#v + 1] = longitud(parte, zoom, tamano) or 0 end
    if #v > 0 then
      c = { t = v[1], r = v[2] or v[1], b = v[3] or v[1], l = v[4] or v[2] or v[1] }
    end
  end
  for lado, clave in pairs({ t = "top", r = "right", b = "bottom", l = "left" }) do
    local largo = d[prop .. "-" .. clave]
    if largo then c = c or {}; c[lado] = longitud(largo, zoom, tamano) or 0 end
  end
  return c
end

local function aplicar_declaraciones(estilo, d, zoom)
  if not d then return end
  estilo.margen = cajas(d, "margin", estilo.margen, zoom, estilo.tamano)
  estilo.relleno = cajas(d, "padding", estilo.relleno, zoom, estilo.tamano)
  local c = M.color(d["color"]); if c then estilo.color = c end
  if d["background-color"] then
    if d["background-color"]:lower() == "transparent" then estilo.fondo = nil
    else local f = M.color(d["background-color"]); if f then estilo.fondo = f end end
  end
  if d["background"] then local f = M.color(d["background"]); if f then estilo.fondo = f end end
  if d["text-align"] then
    local a = d["text-align"]:lower()
    if a == "left" or a == "center" or a == "right" then estilo.alinear = a end
  end
  if d["font-size"] then
    local px = tonumber(d["font-size"]:match("^(%d+%.?%d*)%s*px$"))
    if px then estilo.tamano = math.max(6, math.floor(px * zoom + 0.5)) end
    local em = tonumber(d["font-size"]:match("^(%d+%.?%d*)%s*em$"))
    if em then estilo.tamano = math.max(6, math.floor(M.TAMANO_BASE * em * zoom + 0.5)) end
    local pt = tonumber(d["font-size"]:match("^(%d+%.?%d*)%s*pt$"))
    if pt then estilo.tamano = math.max(6, math.floor(pt * 4 / 3 * zoom + 0.5)) end
  end
  if d["font-style"] then
    local fs = d["font-style"]:lower()
    if fs == "italic" or fs == "oblique" then estilo.cursiva = true elseif fs == "normal" then estilo.cursiva = false end
  end
  if d["font-family"] then
    local ff = d["font-family"]:lower()
    if ff:find("mono") or ff:find("courier") or ff:find("consolas") then estilo.mono = true
    elseif ff:find("serif") and not ff:find("sans") then estilo.serif = true
    else estilo.mono = false; estilo.serif = false end
  end
  if d["font-weight"] then
    local w = d["font-weight"]:lower()
    if w == "bold" or w == "bolder" or (tonumber(w) or 0) >= 600 then estilo.negrita = true
    elseif w == "normal" or w == "lighter" then estilo.negrita = false end
  end
  if d["text-decoration"] then
    local t = d["text-decoration"]:lower()
    if t:find("underline") then estilo.subrayado = true elseif t:find("none") then estilo.subrayado = false end
  end
end

local function coincide(regla, nodo)
  if regla.tag and regla.tag ~= nodo.tag then return false end
  if regla.id and (nodo.attrs.id or "") ~= regla.id then return false end
  if regla.clase then
    local ok = false
    for c in (nodo.attrs.class or ""):gmatch("%S+") do if c == regla.clase then ok = true end end
    if not ok then return false end
  end
  return true
end

-- ---------------------------------------------------------------
-- Estilos por defecto de cada etiqueta
-- ---------------------------------------------------------------
M.COLOR_TEXTO   = 0x000000
M.COLOR_FONDO   = 0xFFFFFF
M.COLOR_ENLACE  = 0x0000CC
M.COLOR_CODIGO  = 0x800040
M.COLOR_HR      = 0x808080
M.COLOR_CITA    = 0xC0C0C0
M.COLOR_TABLA   = 0xA0A0A0   -- bordes de celda
M.COLOR_TH      = 0xE8E8EE   -- fondo de cabecera de tabla
M.COLOR_BOTON   = 0xE4E4E8   -- fondo de <button>
M.COLOR_BOTON_B = 0x606070   -- borde de <button>
M.COLOR_CAMPO   = 0xFFFFFF   -- fondo de un campo de texto
M.COLOR_CAMPO_B = 0x8080A0   -- su borde
M.COLOR_FOCO    = 0x2060D0   -- borde del campo que tiene el foco, y el cursor
M.COLOR_AYUDA   = 0x9090A0   -- placeholder
-- Tipos de <input> que se editan como texto
M.TIPOS_TEXTO = { [""] = true, text = true, password = true, email = true, number = true, search = true, tel = true, url = true }

local ENCABEZADO = M.TAMANO_ENCABEZADO

-- ---------------------------------------------------------------
-- Maquetacion
-- ---------------------------------------------------------------
-- opciones: { ancho=, zoom=, tamano_imagen=function(src)->w,h|nil,
--              metrica=function(estilo)->{ancho=fn, alto_linea=, ascent=} }
-- Devuelve { ops = {...}, alto = , titulo = , fondo = }
-- ---- Elementos ocultos ----
-- "display: none" en su estilo, o el atributo hidden. Antes de maquetar
-- se construye una VISTA del arbol sin ellos (los nodos visibles son los
-- mismos, compartidos; solo las listas de hijos son nuevas), asi que
-- ocultar y mostrar desde un script funciona sin tocar la maquetacion:
-- el script cambia el estilo y el visor vuelve a maquetar.
local function oculto(n)
  if not n.attrs then return false end
  if n.attrs.hidden ~= nil then return true end
  local st = n.attrs.style
  return st ~= nil and st:lower():find("display%s*:%s*none") ~= nil
end
local function sin_ocultos(n)
  if n.texto then return n end
  if oculto(n) then return nil end
  local hijos = {}
  for _, h in ipairs(n.hijos or {}) do
    local v = sin_ocultos(h)
    if v then hijos[#hijos + 1] = v end
  end
  return { tag = n.tag, attrs = n.attrs, hijos = hijos }
end

-- Identificadores internos de los campos sin id. De TODO el modulo, no de
-- cada maquetado: un campo creado despues desde un script no puede
-- recibir el mismo que otro que ya lo tiene.
local contador_campos = 0

function M.maquetar(raiz, opciones)
  raiz = sin_ocultos(raiz) or { tag = "#raiz", attrs = {}, hijos = {} }
  -- posicion vertical de cada elemento con id (o <a name>), para las
  -- anclas: un enlace "#seccion" desplaza el documento hasta ahi
  local anclas = {}
  -- el campo de texto que tiene el foco (id), para pintar su cursor
  local foco = opciones and opciones.foco
  local zoom = opciones.zoom or 1
  local ancho = opciones.ancho or 600
  local margen = 8 * zoom
  local tamano_imagen = opciones.tamano_imagen or function() return nil end
  local metrica_fn = opciones.metrica or M.metrica_fija
  -- cache de metricas por (tamano, negrita, cursiva, mono, serif)
  local cache_metrica = {}
  local function metrica(e)
    local k = e.tamano .. (e.negrita and "b" or "") .. (e.cursiva and "i" or "") .. (e.mono and "m" or "") .. (e.serif and "s" or "")
    local m = cache_metrica[k]
    if not m then m = metrica_fn(e); cache_metrica[k] = m end
    return m
  end

  local ops = {}
  local function op(o) ops[#ops+1] = o; return o end

  -- hoja de estilos del documento
  local reglas = {}
  local function recoger_estilos(nodo)
    if nodo.tag == "style" then
      for _, r in ipairs(M.parsear_css(M.texto_plano(nodo))) do reglas[#reglas+1] = r end
    end
    for _, h in ipairs(nodo.hijos or {}) do recoger_estilos(h) end
  end
  recoger_estilos(raiz)

  local titulo_nodo = M.buscar(raiz, "title")
  local titulo = titulo_nodo and M.texto_plano(titulo_nodo):gsub("%s+", " "):gsub("^ ",""):gsub(" $","") or nil

  local cuerpo = M.buscar(raiz, "body") or raiz
  local estilo_base = { color = M.COLOR_TEXTO, tamano = M.TAMANO_BASE * zoom, negrita = false, cursiva = false,
                        mono = false, serif = false, subrayado = false, alinear = "left", pre = false, enlace = nil }
  -- estilo del body (fondo del documento)
  local fondo_doc = M.COLOR_FONDO
  do
    local e = {}
    for _, r in ipairs(reglas) do if coincide(r, cuerpo) then aplicar_declaraciones(e, r.decl, zoom) end end
    aplicar_declaraciones(e, parsear_declaraciones(cuerpo.attrs.style or ""), zoom)
    if cuerpo.attrs.bgcolor then local f = M.color(cuerpo.attrs.bgcolor); if f then e.fondo = f end end
    if cuerpo.attrs.text then local c = M.color(cuerpo.attrs.text); if c then e.color = c end end
    if e.fondo then fondo_doc = e.fondo end
    if e.color then estilo_base.color = e.color end
    if e.tamano then estilo_base.tamano = e.tamano end
    if e.mono ~= nil then estilo_base.mono = e.mono end
    if e.serif ~= nil then estilo_base.serif = e.serif end
  end

  local function copiar(e) local c = {}; for k, v in pairs(e) do c[k] = v end; return c end

  -- Estilo de un nodo = heredado + defaults de la etiqueta + CSS + style=""
  local function estilo_de(nodo, heredado)
    local e = copiar(heredado)
    e.fondo = nil  -- el fondo no se hereda
    e.margen = nil; e.relleno = nil  -- ni los margenes y rellenos: son de cada caja
    local t = nodo.tag
    if ENCABEZADO[t] then e.tamano = ENCABEZADO[t] * zoom; e.negrita = true end
    if t == "b" or t == "strong" or t == "th" then e.negrita = true end
    if t == "th" and not nodo.attrs.align and not (nodo.attrs.style or ""):find("text%-align") then e.alinear = "center" end
    if t == "i" or t == "em" or t == "cite" or t == "var" then e.cursiva = true end
    if t == "u" then e.subrayado = true end
    if t == "code" or t == "pre" or t == "kbd" or t == "samp" or t == "tt" then e.color = M.COLOR_CODIGO; e.mono = true end
    if t == "small" then e.tamano = math.max(6, math.floor(e.tamano * 0.8 + 0.5)) end
    if t == "big" then e.tamano = math.floor(e.tamano * 1.25 + 0.5) end
    if t == "center" then e.alinear = "center" end
    if t == "pre" then e.pre = true end
    if t == "a" and nodo.attrs.href then e.color = M.COLOR_ENLACE; e.subrayado = true; e.enlace = nodo.attrs.href end
    if t == "font" then
      local c = M.color(nodo.attrs.color); if c then e.color = c end
      local sz = tonumber(nodo.attrs.size); if sz then e.tamano = (M.TAMANO_FONT_SIZE[math.max(1, math.min(7, math.floor(sz)))]) * zoom end
      local face = (nodo.attrs.face or ""):lower(); if face:find("mono") or face:find("courier") then e.mono = true end
    end
    if nodo.attrs.align then
      local a = nodo.attrs.align:lower(); if a == "left" or a == "center" or a == "right" then e.alinear = a end
    end
    for _, r in ipairs(reglas) do if coincide(r, nodo) then aplicar_declaraciones(e, r.decl, zoom) end end
    aplicar_declaraciones(e, parsear_declaraciones(nodo.attrs.style or ""), zoom)
    -- id del elemento inline mas cercano: los textos que cuelguen de el
    -- llevan una "zona" de clic con ese id (para <script type="text/lua">)
    if nodo.attrs.id then e.id = (not BLOQUES[nodo.tag]) and nodo.attrs.id or nil
    elseif BLOQUES[nodo.tag] then e.id = nil end   -- un bloque no propaga id a su texto (ya tiene su zona); un inline si lo hereda
    return e
  end

  -- ---- constructor de lineas (inline) ----
  -- Acumula "piezas" {texto,estilo} o {imagen} y las parte en lineas
  -- dentro de [x0, x0+w). Al cerrar cada linea, emite las ops.
  local Linea = {}
  Linea.__index = Linea
  local function nueva_linea(x0, w, y, alinear)
    return setmetatable({ x0 = x0, w = w, y = y, alinear = alinear, piezas = {}, ancho = 0, alto = 0, vacia = true }, Linea)
  end
  function Linea:cabe(anchura) return self.ancho + anchura <= self.w or self.vacia end
  function Linea:anadir(p)
    if p.espacio and self.vacia then return end  -- sin espacios al principio de linea
    self.piezas[#self.piezas+1] = p
    self.ancho = self.ancho + p.ancho
    if p.alto > self.alto then self.alto = p.alto end
    if not p.espacio then self.vacia = false end
  end
  function Linea:cerrar(ops_, minimo_alto)
    -- quitar espacios al final
    while #self.piezas > 0 and self.piezas[#self.piezas].espacio do
      self.ancho = self.ancho - self.piezas[#self.piezas].ancho; table.remove(self.piezas)
    end
    -- Alinear por la BASE: la linea mide max(ascent) + max(descenso).
    local max_asc, max_desc = 0, 0
    for _, p in ipairs(self.piezas) do
      local asc = p.ascent or p.alto
      if asc > max_asc then max_asc = asc end
      if p.alto - asc > max_desc then max_desc = p.alto - asc end
    end
    local alto = math.max(max_asc + max_desc, minimo_alto or 0)
    local x = self.x0
    if self.alinear == "center" then x = self.x0 + math.floor((self.w - self.ancho) / 2)
    elseif self.alinear == "right" then x = self.x0 + self.w - self.ancho end
    for _, p in ipairs(self.piezas) do
      local asc = p.ascent or p.alto
      local ytop = self.y + (max_asc - asc)   -- parte de arriba de esta pieza
      if p.campo then
        -- campo de texto: caja blanca, borde (azul si tiene el foco), valor o
        -- placeholder, cursor al final si tiene el foco
        local e = p.estilo
        local pad = p.pad
        ops_[#ops_+1] = { tipo = "rect", x = x, y = ytop, w = p.ancho, h = p.alto, color = p.foco and M.COLOR_FOCO or M.COLOR_CAMPO_B }
        ops_[#ops_+1] = { tipo = "rect", x = x + 1, y = ytop + 1, w = p.ancho - 2, h = p.alto - 2, color = M.COLOR_CAMPO }
        if p.texto ~= "" then
          ops_[#ops_+1] = { tipo = "texto", x = x + pad, y = ytop + pad // 2, texto = p.texto, tamano = e.tamano,
                            negrita = e.negrita, cursiva = e.cursiva, mono = e.mono, serif = e.serif,
                            color = p.ayuda and M.COLOR_AYUDA or e.color }
        end
        if p.foco then
          ops_[#ops_+1] = { tipo = "rect", x = x + pad + p.ancho_texto, y = ytop + pad // 2, w = math.max(1, e.tamano // 12),
                            h = p.alto - pad, color = M.COLOR_FOCO }
        end
        ops_[#ops_+1] = { tipo = "zona", x = x, y = ytop, w = p.ancho, h = p.alto, id = p.id, campo = true }
      elseif p.casilla then
        local e = p.estilo
        ops_[#ops_+1] = { tipo = "rect", x = x, y = ytop, w = p.alto, h = p.alto, color = M.COLOR_CAMPO_B }
        ops_[#ops_+1] = { tipo = "rect", x = x + 1, y = ytop + 1, w = p.alto - 2, h = p.alto - 2, color = M.COLOR_CAMPO }
        if p.marcada then
          local m3 = math.max(2, p.alto // 4)
          ops_[#ops_+1] = { tipo = "rect", x = x + m3, y = ytop + m3, w = p.alto - 2 * m3, h = p.alto - 2 * m3, color = M.COLOR_FOCO }
        end
        ops_[#ops_+1] = { tipo = "zona", x = x, y = ytop, w = p.alto, h = p.alto, id = p.id, casilla = true }
      elseif p.boton then
        -- <button>: caja con borde y fondo, texto centrado, zona de clic con su id
        local e = p.estilo
        local pad = p.pad
        ops_[#ops_+1] = { tipo = "rect", x = x, y = ytop, w = p.ancho, h = p.alto, color = e.borde or M.COLOR_BOTON_B }
        ops_[#ops_+1] = { tipo = "rect", x = x + 1, y = ytop + 1, w = p.ancho - 2, h = p.alto - 2, color = e.fondo or M.COLOR_BOTON }
        ops_[#ops_+1] = { tipo = "texto", x = x + pad, y = ytop + pad // 2, texto = p.texto, tamano = e.tamano,
                          negrita = e.negrita, cursiva = e.cursiva, mono = e.mono, serif = e.serif, color = e.color }
        ops_[#ops_+1] = { tipo = "zona", x = x, y = ytop, w = p.ancho, h = p.alto, id = p.id, boton = true }
      elseif p.texto and not p.espacio then
        local e = p.estilo
        if e.fondo then ops_[#ops_+1] = { tipo = "rect", x = x, y = self.y, w = p.ancho, h = alto, color = e.fondo } end
        ops_[#ops_+1] = { tipo = "texto", x = x, y = ytop, texto = p.texto, tamano = e.tamano,
                          negrita = e.negrita, cursiva = e.cursiva, mono = e.mono, serif = e.serif, color = e.color }
        if e.subrayado then
          ops_[#ops_+1] = { tipo = "rect", x = x, y = self.y + max_asc + 1, w = p.ancho, h = math.max(1, e.tamano // 14), color = e.color }
        end
        if e.enlace then
          ops_[#ops_+1] = { tipo = "enlace", x = x, y = self.y, w = p.ancho, h = alto, href = e.enlace }
        end
        if e.id then
          ops_[#ops_+1] = { tipo = "zona", x = x, y = self.y, w = p.ancho, h = alto, id = e.id }
        end
      elseif p.imagen then
        ops_[#ops_+1] = { tipo = "imagen", x = x, y = ytop, w = p.ancho, h = p.alto, src = p.imagen, alt = p.alt }
        if p.enlace then ops_[#ops_+1] = { tipo = "enlace", x = x, y = self.y, w = p.ancho, h = p.alto, href = p.enlace } end
      end
      x = x + p.ancho
    end
    return self.y + alto
  end

  -- Contexto de flujo inline: gestiona la linea actual y el salto
  local function nuevo_flujo(x0, w, y, alinear)
    local f = { x0 = x0, w = w, y = y, alinear = alinear, linea = nil, ultimo_alto = 0 }
    function f:asegurar() if not self.linea then self.linea = nueva_linea(self.x0, self.w, self.y, self.alinear) end end
    function f:romper(minimo)
      self:asegurar()
      local alto_min = minimo or 0
      self.y = self.linea:cerrar(ops, alto_min)
      self.linea = nil
    end
    function f:pieza(p)
      self:asegurar()
      if not self.linea:cabe(p.ancho) then
        self:romper()
        self:asegurar()
      end
      self.linea:anadir(p)
    end
    function f:terminar()
      if self.linea and not self.linea.vacia then self:romper() end
      self.linea = nil
      return self.y
    end
    return f
  end

  -- ---- recorrido ----
  local y = margen
  local pila_listas = {}   -- { tipo="ul"/"ol", contador= }

  local function pieza_texto(t, e, m, espacio)
    return { texto = t, espacio = espacio, estilo = e, ancho = m.ancho(t), alto = m.alto_linea, ascent = m.ascent }
  end

  local function texto_inline(flujo, s, e)
    local m = metrica(e)
    if e.pre then
      -- respeta espacios y saltos
      local primera = true
      for linea in (s .. "\n"):gmatch("(.-)\n") do
        if not primera then flujo:romper(m.alto_linea) end
        primera = false
        if #linea > 0 then
          local l = linea:gsub("\t", "    ")
          flujo:pieza(pieza_texto(l, e, m, false))
        end
      end
      return
    end
    -- normal: colapsar espacios, partir en palabras
    local i = 1
    for pre_esp, palabra_crudo in s:gmatch("(%s*)(%S+)") do
      local palabra = palabra_crudo
      if #pre_esp > 0 or i > 1 then flujo:pieza(pieza_texto(" ", e, m, true)) end
      -- una palabra mas ancha que la linea entera: partirla por caracteres
      while m.ancho(palabra) > flujo.w and #palabra > 1 do
        local k = #palabra
        -- retroceder por limites de UTF-8 hasta que quepa
        while k > 1 and m.ancho(palabra:sub(1, k)) > flujo.w do
          k = k - 1
          while k > 1 and palabra:byte(k) >= 0x80 and palabra:byte(k) < 0xC0 do k = k - 1 end
        end
        flujo:pieza(pieza_texto(palabra:sub(1, k), e, m, false))
        palabra = palabra:sub(k + 1)
        flujo:romper()
      end
      flujo:pieza(pieza_texto(palabra, e, m, false))
      i = i + 1
    end
    if s:match("%s$") then flujo:pieza(pieza_texto(" ", e, m, true)) end
  end

  local maquetar_bloque, maquetar_tabla -- forward

  -- Maqueta el contenido inline de un nodo dentro de un flujo ya abierto
  local function inline(nodo, flujo, e_padre)
    -- anclas en linea (<a name>, <span id>): la linea en curso. Se mira el
    -- propio nodo -- a un <a> suelto dentro de un bloque se le llama asi.
    if nodo.attrs then
      local nombre = nodo.attrs.id or (nodo.tag == "a" and nodo.attrs.name)
      if nombre and anclas[nombre] == nil then anclas[nombre] = flujo.y or y end
    end
    for _, h in ipairs(nodo.hijos or {}) do
      if h.texto then
        texto_inline(flujo, h.texto, e_padre)
      elseif h.tag == "br" then
        flujo:romper(metrica(e_padre).alto_linea)
      elseif h.tag == "input" and M.TIPOS_TEXTO[(h.attrs.type or ""):lower()] then
        -- Campo de texto. Sin id, uno interno: la zona de clic
        -- necesita identificar el campo. attrs es compartido con el arbol
        -- original, asi que el id se conserva de un maquetado al siguiente.
        if not h.attrs.id then contador_campos = contador_campos + 1; h.attrs.id = "__campo" .. contador_campos end
        local e = estilo_de(h, e_padre)
        local m = metrica(e)
        local valor = h.attrs.value or ""
        if (h.attrs.type or ""):lower() == "password" then valor = string.rep("*", utf8.len(valor) or #valor) end
        local ayuda = (valor == "" and h.attrs.placeholder) or nil
        local caracteres = tonumber(h.attrs.size) or 20
        local pad = math.floor(e.tamano * 0.4)
        local ancho = math.max(m.ancho(string.rep("n", caracteres)), m.ancho(valor) + m.ancho(" ")) + 2 * pad
        flujo:pieza({ campo = true, texto = ayuda or valor, ayuda = ayuda ~= nil, ancho_texto = (ayuda and 0) or m.ancho(valor),
                      estilo = e, id = h.attrs.id, pad = pad, foco = (foco == h.attrs.id),
                      ancho = ancho, alto = m.alto_linea + pad, ascent = m.ascent + pad // 2 })
      elseif h.tag == "input" and (h.attrs.type or ""):lower() == "checkbox" then
        if not h.attrs.id then contador_campos = contador_campos + 1; h.attrs.id = "__campo" .. contador_campos end
        local e = estilo_de(h, e_padre)
        local m = metrica(e)
        local lado = m.ascent
        flujo:pieza({ casilla = true, marcada = h.attrs.checked ~= nil, estilo = e, id = h.attrs.id,
                      ancho = lado + 2, alto = lado, ascent = lado })
      elseif h.tag == "button" or (h.tag == "input" and ((h.attrs.type or ""):lower() == "button" or (h.attrs.type or ""):lower() == "submit")) then
        local e = estilo_de(h, e_padre)
        local m = metrica(e)
        local txt = (h.tag == "input") and (h.attrs.value or ((h.attrs.type or ""):lower() == "submit" and "Enviar" or "Boton")) or M.texto_plano(h):gsub("%s+", " "):gsub("^ ", ""):gsub(" $", "")
        -- Todo boton sin id recibe uno interno: su zona de clic tiene que
        -- poder identificarlo para enviar su formulario (un <button> sin
        -- type dentro de un <form> lo envia, como en HTML).
        if not h.attrs.id then
          contador_campos = contador_campos + 1; h.attrs.id = "__campo" .. contador_campos
        end
        if txt == "" then txt = " " end
        local pad = math.floor(e.tamano * 0.6)
        flujo:pieza({ boton = true, texto = txt, estilo = e, id = h.attrs.id, pad = pad,
                      ancho = m.ancho(txt) + 2 * pad, alto = m.alto_linea + pad, ascent = m.ascent + pad // 2 })
      elseif h.tag == "img" then
        local w, hh = tamano_imagen(h.attrs.src or "")
        local e = estilo_de(h, e_padre)
        if w then
          local aw, ah = tonumber(h.attrs.width), tonumber(h.attrs.height)
          if aw and ah then w, hh = aw, ah elseif aw then hh = math.floor(hh * aw / w); w = aw elseif ah then w = math.floor(w * ah / hh); hh = ah end
          flujo:pieza({ imagen = h.attrs.src, alt = h.attrs.alt, ancho = w, alto = hh, enlace = e.enlace })
        else
          local alt = "[" .. (h.attrs.alt or h.attrs.src or "imagen") .. "]"
          flujo:pieza(pieza_texto(alt, e, metrica(e), false))
        end
      elseif BLOQUES[h.tag] then
        -- un bloque dentro de un inline: cerrar la linea y maquetarlo aparte
        flujo:terminar()
        y = flujo.y
        y = maquetar_bloque(h, flujo.x0, flujo.w, e_padre)
        flujo.y = y
      else
        local e = estilo_de(h, e_padre)
        inline(h, flujo, e)
      end
    end
  end

  -- ¿Tiene el nodo algun hijo que sea bloque? (decide si su contenido
  -- es un flujo inline o una secuencia de bloques)
  local function tiene_bloques(nodo)
    for _, h in ipairs(nodo.hijos or {}) do if h.tag and BLOQUES[h.tag] then return true end end
    return false
  end

  -- Tabla: anchura de columnas por contenido (minimo = la palabra mas
  -- larga de la columna; maximo = su celda mas larga en una sola linea),
  -- repartiendo el ancho disponible; cada celda se maqueta como un
  -- bloque dentro de su columna. Sin colspan/rowspan.
  maquetar_tabla = function(nodo, x0, w, e_padre)
    local e = estilo_de(nodo, e_padre)
    local m = metrica(e)
    local ancho_linea = m.alto_linea
    -- 1) recoger filas y celdas (thead/tbody/tfoot transparentes)
    local filas = {}
    local function recoger(n)
      for _, h in ipairs(n.hijos or {}) do
        if h.tag == "tr" then
          local celdas = {}
          for _, c in ipairs(h.hijos or {}) do if c.tag == "td" or c.tag == "th" then celdas[#celdas+1] = c end end
          if #celdas > 0 then filas[#filas+1] = celdas end
        elseif h.tag == "thead" or h.tag == "tbody" or h.tag == "tfoot" then recoger(h) end
      end
    end
    recoger(nodo)
    if #filas == 0 then return y end
    local nfilas = #filas

    -- 2) La REJILLA (colspan/rowspan,). Cada celda va en la
    --    primera columna libre de su fila, saltando las que ya ocupa un
    --    rowspan de mas arriba. Un rowspan no pasa de la ultima fila.
    local ocupada = {}
    for f = 1, nfilas do ocupada[f] = {} end
    local celdas, ncol = {}, 0
    for f, fila in ipairs(filas) do
      local c = 1
      for _, nc in ipairs(fila) do
        while ocupada[f][c] do c = c + 1 end
        local cs = math.max(1, math.min(100, math.floor(tonumber(nc.attrs.colspan) or 1)))
        local rs = math.max(1, math.floor(tonumber(nc.attrs.rowspan) or 1))
        if rs > nfilas - f + 1 then rs = nfilas - f + 1 end
        for ff = f, f + rs - 1 do for cc = c, c + cs - 1 do ocupada[ff][cc] = true end end
        celdas[#celdas+1] = { nodo = nc, f = f, c = c, cs = cs, rs = rs }
        if c + cs - 1 > ncol then ncol = c + cs - 1 end
        c = c + cs
      end
    end
    -- Los huecos sin celda (una fila mas corta que otra) quedan como celdas
    -- vacias: con borde y sin fondo, como antes.
    for f = 1, nfilas do
      for c = 1, ncol do if not ocupada[f][c] then celdas[#celdas+1] = { f = f, c = c, cs = 1, rs = 1, vacia = true } end end
    end
    table.sort(celdas, function(a, b) if a.f ~= b.f then return a.f < b.f end return a.c < b.c end)

    -- 3) anchuras minima/maxima por columna (con la metrica del texto de cada celda)
    local relleno = math.floor(e.tamano * 0.5)
    local minimo, maximo = {}, {}
    for c = 1, ncol do minimo[c] = 0; maximo[c] = 0 end
    local function anchos_celda(celda, ec)
      local mc = metrica(ec)
      local texto = M.texto_plano(celda):gsub("%s+", " ")
      local mn, mx = 0, mc.ancho(texto:gsub("^ ", ""):gsub(" $", ""))
      for palabra in texto:gmatch("%S+") do local a = mc.ancho(palabra); if a > mn then mn = a end end
      -- imagenes dentro de la celda cuentan como palabra
      local function img(n) for _, h in ipairs(n.hijos or {}) do if h.tag == "img" then local iw = tamano_imagen(h.attrs.src or ""); if iw and iw > mn then mn = iw; mx = mx + iw end elseif h.tag then img(h) end end end
      img(celda)
      return mn, mx
    end
    for _, cd in ipairs(celdas) do
      if not cd.vacia then
        cd.e = estilo_de(cd.nodo, e)
        local mn, mx = anchos_celda(cd.nodo, cd.e)
        cd.mn, cd.mx = mn + 2 * relleno, mx + 2 * relleno
        if cd.cs == 1 then
          if cd.mn > minimo[cd.c] then minimo[cd.c] = cd.mn end
          if cd.mx > maximo[cd.c] then maximo[cd.c] = cd.mx end
        end
      end
    end
    -- las que abarcan varias columnas: si no caben en la suma de las suyas
    -- (bordes interiores incluidos), reparten lo que falte entre ellas
    local function repartir(tabla, cd, necesita)
      local suma = cd.cs - 1
      for c = cd.c, cd.c + cd.cs - 1 do suma = suma + tabla[c] end
      if necesita > suma then
        local extra = necesita - suma
        for k = 0, cd.cs - 1 do tabla[cd.c + k] = tabla[cd.c + k] + extra // cd.cs + ((k < extra % cd.cs) and 1 or 0) end
      end
    end
    for _, cd in ipairs(celdas) do
      if not cd.vacia and cd.cs > 1 then repartir(minimo, cd, cd.mn); repartir(maximo, cd, cd.mx) end
    end
    for c = 1, ncol do if maximo[c] < minimo[c] then maximo[c] = minimo[c] end end

    -- 4) repartir el ancho
    local disponible = w - (ncol + 1)   -- bordes de 1px
    local suma_min, suma_max = 0, 0
    for c = 1, ncol do suma_min = suma_min + minimo[c]; suma_max = suma_max + maximo[c] end
    local anchura = {}
    if suma_max <= disponible then
      for c = 1, ncol do anchura[c] = maximo[c] end
    elseif suma_min >= disponible then
      -- ni los minimos caben: proporcional a los minimos (habra palabras partidas)
      for c = 1, ncol do anchura[c] = math.max(2 * relleno + 8, math.floor(minimo[c] * disponible / suma_min)) end
    else
      local extra = disponible - suma_min
      local flex = suma_max - suma_min
      for c = 1, ncol do
        anchura[c] = minimo[c] + (flex > 0 and math.floor(extra * (maximo[c] - minimo[c]) / flex) or 0)
      end
    end
    local colx = {}
    colx[1] = x0 + 1
    for c = 2, ncol + 1 do colx[c] = colx[c - 1] + anchura[c - 1] + 1 end

    -- 5) maquetar fila a fila
    y = y + ancho_linea // 2
    local y_tabla = y
    local ops_tabla_inicio = #ops + 1
    local y_fila_de, alto_fila = {}, {}
    local k = 1
    for f = 1, nfilas do
      local y_fila = y
      y_fila_de[f] = y_fila
      alto_fila[f] = ancho_linea
      while celdas[k] and celdas[k].f == f do
        local cd = celdas[k]
        cd.x = colx[cd.c]
        cd.w = colx[cd.c + cd.cs] - 1 - cd.x          -- sus columnas y los bordes de entre medias
        if not cd.vacia then
          local ec = cd.e
          y = y_fila + relleno // 2
          if tiene_bloques(cd.nodo) then
            -- celda con bloques dentro (parrafos, listas): como un div
            maquetar_bloque({ tag = "div", attrs = {}, hijos = cd.nodo.hijos }, cd.x + relleno, cd.w - 2 * relleno, ec)
          else
            local flujo = nuevo_flujo(cd.x + relleno, cd.w - 2 * relleno, y, ec.alinear)
            local ec_texto = copiar(ec); ec_texto.fondo = nil
            inline(cd.nodo, flujo, ec_texto)
            y = flujo:terminar()
            if y == y_fila + relleno // 2 then y = y + metrica(ec).alto_linea end  -- celda vacia
          end
          y = y + relleno // 2
          cd.alto = y - y_fila
          cd.fondo = (cd.nodo.tag == "th") and (ec.fondo or M.COLOR_TH) or ec.fondo
          if cd.rs == 1 and cd.alto > alto_fila[f] then alto_fila[f] = cd.alto end
        end
        k = k + 1
      end
      -- una celda con rowspan que TERMINA en esta fila tiene que caber:
      -- si no, esta fila (la ultima de las suyas) crece
      for _, cd in ipairs(celdas) do
        if cd.rs > 1 and cd.alto and cd.f + cd.rs - 1 == f then
          local necesita = cd.alto - (y_fila - y_fila_de[cd.f])
          if necesita > alto_fila[f] then alto_fila[f] = necesita end
        end
      end
      y = y_fila + alto_fila[f] + 1
    end

    -- 6) fondos (DETRAS de todo el contenido de la tabla) y bordes (encima),
    --    celda a celda: asi ninguna linea cruza una celda que abarca varias
    local ancho_total = colx[ncol + 1] - x0
    local fondos = {}
    for _, cd in ipairs(celdas) do
      cd.y = y_fila_de[cd.f]
      cd.h = cd.rs - 1
      for ff = cd.f, cd.f + cd.rs - 1 do cd.h = cd.h + alto_fila[ff] end
      if cd.fondo then fondos[#fondos+1] = { tipo = "rect", x = cd.x, y = cd.y, w = cd.w, h = cd.h, color = cd.fondo } end
    end
    for i = #fondos, 1, -1 do table.insert(ops, ops_tabla_inicio, fondos[i]) end
    table.insert(ops, ops_tabla_inicio, { tipo = "rect", x = x0, y = y_tabla, w = ancho_total, h = 1, color = M.COLOR_TABLA })
    for _, cd in ipairs(celdas) do
      op({ tipo = "rect", x = cd.x - 1, y = cd.y + cd.h, w = cd.w + 2, h = 1, color = M.COLOR_TABLA })        -- abajo
      op({ tipo = "rect", x = cd.x + cd.w, y = cd.y, w = 1, h = cd.h + 1, color = M.COLOR_TABLA })            -- derecha
    end
    op({ tipo = "rect", x = x0, y = y_tabla, w = 1, h = y - y_tabla, color = M.COLOR_TABLA })                -- izquierda
    y = y + ancho_linea // 2
    return y
  end

  maquetar_bloque = function(nodo, x0, w, e_padre)
    local t = nodo.tag
    if nodo.attrs then
      local nombre = nodo.attrs.id or (t == "a" and nodo.attrs.name)
      if nombre and anclas[nombre] == nil then anclas[nombre] = y end
    end
    if t == "head" or t == "title" or t == "style" or t == "script" then return y end
    local e = estilo_de(nodo, e_padre)
    local ancho_linea = metrica(e).alto_linea
    local sangria = math.floor(e.tamano * 1.5)

    if t == "table" then
      return maquetar_tabla(nodo, x0, w, e)
    end
    if t == "hr" then
      y = y + ancho_linea // 2
      op({ tipo = "rect", x = x0, y = y, w = w, h = math.max(1, zoom), color = M.COLOR_HR })
      y = y + ancho_linea // 2 + zoom
      return y
    end

    -- Margenes: los de su CSS, si los tiene; si no, el "aire"
    -- de siempre arriba y abajo en encabezados, parrafos y listas. Los
    -- laterales estrechan la caja. El relleno mete el contenido hacia
    -- dentro; el fondo y la zona de clic cubren la caja con su relleno.
    local aire = (ENCABEZADO[t] or t == "p" or t == "ul" or t == "ol" or t == "blockquote" or t == "pre") and ancho_linea // 2 or 0
    local mg, rl = e.margen or {}, e.relleno or {}
    local m_arriba, m_abajo = mg.t or aire, mg.b or aire
    local ml, mr = mg.l or 0, mg.r or 0
    local pt, pr, pb, pl = rl.t or 0, rl.r or 0, rl.b or 0, rl.l or 0
    -- El aire por defecto queda DENTRO de la caja (como siempre: el fondo
    -- gris de un bloque de codigo tiene su respiro por encima); solo un
    -- margin-top explicito queda fuera, como en CSS.
    local y_inicio = y
    y = y + m_arriba
    if mg.t then y_inicio = y end
    local y_contenido = y
    x0 = x0 + ml; w = math.max(1, w - ml - mr)          -- la caja del bloque
    local ops_inicio = #ops + 1
    y = y + pt

    local xi, wi = x0 + pl, math.max(1, w - pl - pr)     -- su contenido
    if t == "blockquote" then
      xi = xi + sangria; wi = wi - sangria
      -- barra lateral: se anade al final, cuando se sepa el alto
    end
    if t == "ul" or t == "ol" then
      table.insert(pila_listas, { tipo = t, contador = tonumber(nodo.attrs.start) or 1 })
      xi = xi + sangria; wi = wi - sangria
    end
    if t == "li" then
      local lista = pila_listas[#pila_listas]
      local marca
      if lista and lista.tipo == "ol" then marca = tostring(lista.contador) .. "."; lista.contador = lista.contador + 1
      else marca = "*" end
      local mm = metrica(e)
      op({ tipo = "texto", x = xi - mm.ancho(marca) - math.floor(e.tamano * 0.4), y = y, texto = marca, tamano = e.tamano,
           negrita = false, cursiva = false, mono = e.mono, serif = e.serif, color = e.color })
    end

    -- El fondo del bloque se pinta UNA vez como rectangulo del bloque
    -- entero (abajo); el texto directo del bloque no lo repite. Los
    -- inline con fondo propio (<span style="background...">) si lo llevan.
    local e_texto = copiar(e); e_texto.fondo = nil

    if tiene_bloques(nodo) and t ~= "pre" then
      -- secuencia de bloques (y texto suelto entre ellos, que se agrupa en flujos)
      local flujo = nil
      for _, h in ipairs(nodo.hijos or {}) do
        if h.tag and BLOQUES[h.tag] then
          if flujo then y = flujo:terminar(); flujo = nil end
          y = maquetar_bloque(h, xi, wi, e)
        else
          if not flujo then flujo = nuevo_flujo(xi, wi, y, e.alinear) end
          if h.texto then texto_inline(flujo, h.texto, e_texto)
          elseif h.tag == "br" then flujo:romper(ancho_linea)
          elseif h.tag == "img" then inline({ hijos = { h } }, flujo, e_texto)
          else inline(h, flujo, estilo_de(h, e_texto)) end
          y = flujo.y
        end
      end
      if flujo then y = flujo:terminar() end
    else
      local flujo = nuevo_flujo(xi, wi, y, e.alinear)
      inline(nodo, flujo, e_texto)
      y = flujo:terminar()
      if t == "li" and y == y_inicio then y = y + ancho_linea end  -- <li> vacio ocupa una linea igual
    end

    if t == "ul" or t == "ol" then table.remove(pila_listas) end
    y = y + pb                                            -- el relleno de abajo
    if t == "blockquote" then
      table.insert(ops, ops_inicio, { tipo = "rect", x = x0 + math.floor(sangria * 0.25), y = y_contenido, w = math.max(2, math.floor(sangria * 0.2)), h = y - y_contenido, color = M.COLOR_CITA })
    end
    if e.fondo and (t == "div" or t == "p" or t == "pre" or t == "section" or t == "article" or t == "header" or t == "footer" or t == "nav" or t == "main" or t == "aside" or ENCABEZADO[t] or t == "blockquote") then
      table.insert(ops, ops_inicio, { tipo = "rect", x = x0, y = y_inicio, w = w, h = y - y_inicio, color = e.fondo })
    end
    if nodo.attrs.id then
      -- delante del contenido del bloque: asi un hijo con id (mas al final
      -- de la lista) gana al mirar desde atras en zona_en
      table.insert(ops, ops_inicio, { tipo = "zona", x = x0, y = y_inicio, w = w, h = y - y_inicio, id = nodo.attrs.id })
    end

    y = y + m_abajo
    return y
  end

  maquetar_bloque(cuerpo, margen, ancho - 2 * margen, estilo_base)
  y = y + margen

  return { ops = ops, alto = y, titulo = titulo, fondo = fondo_doc, anclas = anclas }
end

-- Zona (elemento con id, o boton) bajo un punto -> id, o nil. Se mira la
-- ultima primero: la mas interior (un <span id> dentro de un <div id>) gana.
function M.zona_en(resultado, x, y)
  for i = #resultado.ops, 1, -1 do
    local o = resultado.ops[i]
    if o.tipo == "zona" and x >= o.x and x < o.x + o.w and y >= o.y and y < o.y + o.h then return o.id, o.boton, o end
  end
  return nil
end

-- Scripts de la pagina: lista de codigos de <script type="text/lua">
-- (o type="lua"), en orden. Los <script> sin type o con otro tipo
-- (javascript...) se ignoran.
function M.scripts_lua(raiz)
  local codigos = {}
  local function rec(n)
    if n.tag == "script" then
      local t = (n.attrs.type or ""):lower()
      if t == "text/lua" or t == "lua" or t == "application/lua" then codigos[#codigos+1] = M.texto_plano(n) end
    end
    for _, h in ipairs(n.hijos or {}) do rec(h) end
  end
  rec(raiz)
  return codigos
end

-- Primer nodo con ese id, o nil
function M.buscar_id(nodo, id)
  if nodo.attrs and nodo.attrs.id == id then return nodo end
  for _, h in ipairs(nodo.hijos or {}) do
    local r = M.buscar_id(h, id); if r then return r end
  end
  return nil
end

-- ---- Modificar el arbol desde un script ----

-- Un elemento nuevo, con un texto dentro (opcional) y atributos (opcional).
function M.crear(tag, texto, attrs)
  local a = {}
  for k, v in pairs(attrs or {}) do a[tostring(k):lower()] = tostring(v) end
  local nodo = { tag = tostring(tag):lower(), attrs = a, hijos = {} }
  if texto ~= nil and texto ~= "" then nodo.hijos[1] = { texto = tostring(texto) } end
  return nodo
end

-- El padre de 'nodo' dentro de 'raiz', y su posicion entre los hijos; o nil.
function M.buscar_padre(raiz, nodo)
  for i, h in ipairs(raiz.hijos or {}) do
    if h == nodo then return raiz, i end
    local p, j = M.buscar_padre(h, nodo)
    if p then return p, j end
  end
  return nil
end

-- Quita 'nodo' del arbol. true si estaba.
function M.quitar(raiz, nodo)
  local p, i = M.buscar_padre(raiz, nodo)
  if not p then return false end
  table.remove(p.hijos, i)
  return true
end

-- Sustituye el contenido de un nodo por un texto plano
function M.poner_texto(nodo, texto)
  nodo.hijos = { { texto = tostring(texto) } }
end

-- Enlace bajo un punto (coordenadas del documento), o nil
function M.enlace_en(resultado, x, y)
  for i = #resultado.ops, 1, -1 do
    local o = resultado.ops[i]
    if o.tipo == "enlace" and x >= o.x and x < o.x + o.w and y >= o.y and y < o.y + o.h then return o.href end
  end
  return nil
end

return M
