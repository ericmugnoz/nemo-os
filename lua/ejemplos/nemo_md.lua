-- nemo_md.lua -- Nemo OS
-- Markdown -> HTML, para verlo con visor.lua (que ya sabe pintar HTML).
-- Las guias de Nemo OS estan en Markdown; con esto se leen en el
-- propio sistema, con sus tablas, su codigo y sus enlaces.
--
-- Subconjunto (el "Markdown de GitHub" que usan las guias):
--   bloques:  # .. ###### encabezados; parrafos; ---/***/___ regla;
--             >  citas (anidables); - * + listas; 1. listas numeradas
--             (anidadas por sangria de 2 o 4 espacios); ```lang bloques
--             de codigo (cercados) y bloques sangrados con 4 espacios;
--             | a | b | tablas con fila separadora |---|:--:|--:|;
--             encabezados setext (=== / --- bajo una linea de texto)
--   inline:   **negrita** __negrita__ *cursiva* _cursiva_ `codigo`
--             ~~tachado~~ (se muestra como cursiva: no hay tachado)
--             [texto](enlace) ![alt](imagen) <http://...> autoenlaces
--             \\* escapes; dos espacios al final = salto de linea
--   html:     etiquetas inline sueltas (<b>, <br>...) pasan tal cual
-- Lo que no hay: notas al pie, listas de tareas ([ ]), definiciones,
-- html en bloque, enlaces por referencia [x]: url.
--
-- Uso:  local html = require("nemo_md").a_html(texto_markdown)

local M = {}

local function escapar(s)
  return (s:gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;"))
end

-- ---------------------------------------------------------------
-- Anclas de los encabezados
-- ---------------------------------------------------------------
-- Un enlace como [Chuleta](#8-chuleta) necesita que el encabezado
-- "## 8. Chuleta" lleve id="8-chuleta". La regla es la de GitHub:
-- se quitan las marcas de formato, se pasa a minusculas, se tira todo
-- lo que no sea letra, cifra, espacio o guion, y los espacios pasan a
-- guiones. Las vocales acentuadas y la ñ se conservan, que es lo que
-- espera un indice escrito en castellano.

-- minuscula de un punto de codigo, ASCII y latino (A-Z, A-grave..Thorn)
local function minuscula(c)
  if c >= 65 and c <= 90 then return c + 32 end
  if c >= 0xC0 and c <= 0xDE and c ~= 0xD7 then return c + 32 end
  return c
end

-- ¿es letra o cifra? ASCII, latino-1 y latino extendido-A
local function es_letra(c)
  if c >= 48 and c <= 57 then return true end                 -- 0-9
  if c >= 97 and c <= 122 then return true end                -- a-z
  if c == 95 then return true end                             -- guion bajo
  if c >= 0xDF and c <= 0xFF and c ~= 0xF7 then return true end -- ss..y-dieresis
  if c >= 0x100 and c <= 0x17F then return true end           -- latino extendido-A
  return false
end

function M.ancla(texto)
  -- fuera las marcas de formato, que no son parte del titulo
  -- El guion bajo NO se quita: en estas guias casi siempre es parte de un
  -- nombre (`nemo_gpio`), no una marca de cursiva.
  local t = texto:gsub("[%*`~\\]", ""):gsub("%[([^%]]*)%]%b()", "%1")
  local partes = {}
  -- utf8.codes protesta ante un byte invalido; si el documento no es UTF-8
  -- limpio, se cae a una version solo-ASCII en vez de romper el visor.
  local ok = pcall(function()
    for _, c in utf8.codes(t) do
      c = minuscula(c)
      if es_letra(c) then
        partes[#partes+1] = utf8.char(c)
      elseif c == 32 or c == 9 or c == 45 then
        partes[#partes+1] = "-"
      end
      -- lo demas (puntos, comas, dos puntos, ¿ ¡ — ...) se tira
    end
  end)
  if not ok then
    partes = {}
    for k = 1, #t do
      local c = minuscula(t:byte(k))
      if c < 128 and es_letra(c) then partes[#partes+1] = string.char(c)
      elseif c == 32 or c == 9 or c == 45 then partes[#partes+1] = "-" end
    end
  end
  return table.concat(partes)
end

-- ---------------------------------------------------------------
-- Inline
-- ---------------------------------------------------------------
local function inline(s)
  -- 1) proteger codigo `...` y escapes \x sustituyendolos por marcadores
  local guardado = {}
  local function guardar(html) guardado[#guardado+1] = html; return "\1" .. #guardado .. "\2" end
  s = s:gsub("\\([\\`%*_{}%[%]%(%)#%+%-%.!|~<>])", function(c) return guardar(escapar(c)) end)
  s = s:gsub("``(.-)``", function(c) return guardar("<code>" .. escapar(c) .. "</code>") end)
  s = s:gsub("`([^`]+)`", function(c) return guardar("<code>" .. escapar(c) .. "</code>") end)
  -- 2) html inline suelto: se deja pasar tal cual (etiquetas cortas conocidas)
  s = s:gsub("(</?%a[%w]*%s*/?>)", function(t) return guardar(t) end)
  s = s:gsub("(<%a[%w]*%s+[^<>]->)", function(t) return guardar(t) end)
  -- 3) escapar lo que queda
  s = escapar(s)
  -- 4) imagenes y enlaces
  s = s:gsub("!%[([^%]]*)%]%(([^%)%s]+)%s*\"?([^\"%)]*)\"?%)", function(alt, src)
    return guardar('<img src="' .. src .. '" alt="' .. alt .. '">')
  end)
  s = s:gsub("%[([^%]]+)%]%(([^%)%s]+)%s*\"?([^\"%)]*)\"?%)", function(texto, href)
    return "<a href=\"" .. href .. "\">" .. texto .. "</a>"
  end)
  s = s:gsub("&lt;(https?://[^%s&]+)&gt;", function(u) return '<a href="' .. u .. '">' .. u .. '</a>' end)
  -- 5) enfasis (negrita antes que cursiva, para que ** no se coma como * *)
  s = s:gsub("%*%*(.-)%*%*", "<b>%1</b>")
  s = s:gsub("__(.-)__", "<b>%1</b>")
  s = s:gsub("~~(.-)~~", "<i>%1</i>")
  -- cursiva: un * o _ que NO va pegado a una letra por fuera (asi 2*3*4 y
  -- snake_case_name se quedan como estan). Se acolcha con espacios para
  -- que el patron valga tambien al principio y al final.
  s = " " .. s .. " "
  s = s:gsub("([^%w%*])%*([^%*%s][^%*\n]-)%*([^%w%*])", "%1<i>%2</i>%3")
  s = s:gsub("([^%w_])_([^_%s][^_\n]-)_([^%w_])", "%1<i>%2</i>%3")
  s = s:sub(2, -2)
  -- 6) salto de linea duro: dos espacios al final
  s = s:gsub("  $", "<br>")
  -- 7) restaurar
  s = s:gsub("\1(%d+)\2", function(i) return guardado[tonumber(i)] end)
  return s
end
M.inline = inline

-- ---------------------------------------------------------------
-- Bloques
-- ---------------------------------------------------------------
local function es_regla(l) return l:match("^%s*[%-%*_][%s%-%*_]*$") ~= nil and #l:gsub("[%s]", "") >= 3 end
local function es_separador_tabla(l) return l:match("^%s*|?%s*:?%-+:?%s*|") ~= nil and not l:find("[^%s|:%-]") end
local function es_fila_tabla(l) return l:find("|") ~= nil end

local function alineaciones(sep)
  local a = {}
  for celda in (sep:gsub("^%s*|", ""):gsub("|%s*$", "") .. "|"):gmatch("([^|]*)|") do
    local c = celda:gsub("%s", "")
    if c:sub(1,1) == ":" and c:sub(-1) == ":" then a[#a+1] = "center"
    elseif c:sub(-1) == ":" then a[#a+1] = "right"
    else a[#a+1] = "left" end
  end
  return a
end

local function celdas_de(l)
  l = l:gsub("^%s*|", ""):gsub("|%s*$", "")
  local c = {}
  -- separar por | que no esten escapados
  local actual = ""
  local i = 1
  while i <= #l do
    local ch = l:sub(i, i)
    if ch == "\\" and l:sub(i+1, i+1) == "|" then actual = actual .. "|"; i = i + 2
    elseif ch == "|" then c[#c+1] = actual; actual = ""; i = i + 1
    else actual = actual .. ch; i = i + 1 end
  end
  c[#c+1] = actual
  for k = 1, #c do c[k] = c[k]:gsub("^%s+", ""):gsub("%s+$", "") end
  return c
end

function M.a_html(md)
  local lineas = {}
  for l in (md .. "\n"):gmatch("(.-)\r?\n") do lineas[#lineas+1] = l end
  local out = {}
  local function emitir(s) out[#out+1] = s end

  local i = 1
  local n = #lineas
  local pila_listas = {}   -- { tipo="ul"|"ol", sangria=n }
  local parrafo = {}
  local anclas_usadas = {} -- dos titulos iguales: el segundo lleva -1, -2...

  -- Emite <hN id="ancla">texto</hN>. El id es lo que hace que funcione
  -- un indice con enlaces [Titulo](#titulo) dentro del propio documento.
  local function emitir_encabezado(nivel, texto)
    local id = M.ancla(texto)
    if id ~= "" then
      local veces = anclas_usadas[id]
      if veces then
        anclas_usadas[id] = veces + 1
        id = id .. "-" .. veces
      else
        anclas_usadas[id] = 1
      end
      emitir("<h" .. nivel .. " id=\"" .. escapar(id) .. "\">" ..
             inline(texto) .. "</h" .. nivel .. ">")
    else
      emitir("<h" .. nivel .. ">" .. inline(texto) .. "</h" .. nivel .. ">")
    end
  end

  local function cerrar_parrafo()
    if #parrafo > 0 then
      emitir("<p>" .. inline(table.concat(parrafo, "\n")) .. "</p>")
      parrafo = {}
    end
  end
  local function cerrar_listas(hasta_sangria)
    while #pila_listas > 0 and pila_listas[#pila_listas].sangria >= (hasta_sangria or 0) do
      local l = table.remove(pila_listas)
      emitir("</li></" .. l.tipo .. ">")
    end
  end
  local function cerrar_todo() cerrar_parrafo(); cerrar_listas(0) end

  while i <= n do
    local l = lineas[i]
    local sangria = #(l:match("^( *)") or "")
    local contenido = l:sub(sangria + 1)

    -- bloque de codigo cercado
    local cerca, lang = l:match("^%s*(```+)%s*(%w*)")
    if not cerca then cerca, lang = l:match("^%s*(~~~+)%s*(%w*)") end
    if cerca then
      cerrar_todo()
      local buf = {}
      i = i + 1
      while i <= n and not lineas[i]:match("^%s*" .. cerca:sub(1,1) .. cerca:sub(1,1) .. cerca:sub(1,1)) do
        buf[#buf+1] = escapar(lineas[i]); i = i + 1
      end
      emitir("<pre>" .. table.concat(buf, "\n") .. "</pre>")
      i = i + 1
      goto siguiente
    end

    -- linea en blanco: cierra parrafo (las listas siguen si la siguiente linea sigue sangrada)
    if contenido == "" then
      cerrar_parrafo()
      -- si la siguiente linea no es un item de lista ni esta sangrada, cerrar listas
      local sig = lineas[i+1]
      if not sig or (not sig:match("^%s*[%-%*%+]%s") and not sig:match("^%s*%d+[%.%)]%s") and not sig:match("^  ")) then cerrar_listas(0) end
      i = i + 1
      goto siguiente
    end

    -- encabezado ATX
    do
      local almo, texto = contenido:match("^(#+)%s+(.-)%s*#*%s*$")
      if almo and #almo <= 6 then
        cerrar_todo()
        emitir_encabezado(#almo, texto)
        i = i + 1; goto siguiente
      end
    end

    -- encabezado setext: texto en parrafo + linea de === o ---
    do
      local sig = lineas[i+1]
      if sig and #parrafo == 0 and #pila_listas == 0 and sangria < 4 and (sig:match("^%s*=+%s*$") or (sig:match("^%s*%-%-+%s*$") and not es_regla(contenido))) then
        cerrar_todo()
        local nivel = sig:find("=") and 1 or 2
        emitir_encabezado(nivel, contenido)
        i = i + 2; goto siguiente
      end
    end

    -- regla
    if es_regla(contenido) and #pila_listas == 0 then
      cerrar_todo(); emitir("<hr>"); i = i + 1; goto siguiente
    end

    -- tabla: fila + separador
    if es_fila_tabla(contenido) and lineas[i+1] and es_separador_tabla(lineas[i+1]) then
      cerrar_todo()
      local alin = alineaciones(lineas[i+1])
      local cab = celdas_de(contenido)
      local t = { "<table><tr>" }
      for c, celda in ipairs(cab) do
        t[#t+1] = '<th style="text-align:' .. (alin[c] or "left") .. '">' .. inline(celda) .. "</th>"
      end
      t[#t+1] = "</tr>"
      i = i + 2
      while i <= n and es_fila_tabla(lineas[i]) and lineas[i]:match("%S") do
        local cs = celdas_de(lineas[i])
        t[#t+1] = "<tr>"
        for c = 1, #cab do
          t[#t+1] = '<td style="text-align:' .. (alin[c] or "left") .. '">' .. inline(cs[c] or "") .. "</td>"
        end
        t[#t+1] = "</tr>"
        i = i + 1
      end
      t[#t+1] = "</table>"
      emitir(table.concat(t))
      goto siguiente
    end

    -- cita
    if contenido:match("^>") then
      cerrar_todo()
      local buf = {}
      while i <= n and lineas[i]:match("^%s*>") do
        buf[#buf+1] = lineas[i]:gsub("^%s*>%s?", ""); i = i + 1
      end
      emitir("<blockquote>" .. M.a_html(table.concat(buf, "\n")) .. "</blockquote>")
      goto siguiente
    end

    -- item de lista
    do
      local marca, resto = contenido:match("^([%-%*%+])%s+(.*)$")
      local tipo = "ul"
      if not marca then marca, resto = contenido:match("^(%d+[%.%)])%s+(.*)$"); if marca then tipo = "ol" end end
      if marca then
        cerrar_parrafo()
        -- cerrar listas mas sangradas; abrir si es mas profunda que la actual
        cerrar_listas(sangria + 1)
        local top = pila_listas[#pila_listas]
        -- cambio de tipo (ul <-> ol) al mismo nivel: es otra lista
        if top and top.sangria == sangria and top.tipo ~= tipo then
          table.remove(pila_listas); emitir("</li></" .. top.tipo .. ">"); top = pila_listas[#pila_listas]
        end
        if not top or top.sangria < sangria then
          local start = tipo == "ol" and (' start="' .. marca:match("%d+") .. '"') or ""
          emitir("<" .. tipo .. start .. "><li>")
          table.insert(pila_listas, { tipo = tipo, sangria = sangria })
        else
          emitir("</li><li>")
        end
        -- lineas de continuacion (sangradas, sin ser otro item ni blanco)
        local texto = { resto }
        i = i + 1
        while i <= n do
          local l2 = lineas[i]
          local s2 = #(l2:match("^( *)") or "")
          local c2 = l2:sub(s2 + 1)
          if c2 == "" or s2 <= sangria or c2:match("^[%-%*%+]%s") or c2:match("^%d+[%.%)]%s") or c2:match("^```") then break end
          texto[#texto+1] = c2; i = i + 1
        end
        emitir(inline(table.concat(texto, "\n")))
        goto siguiente
      end
    end

    -- bloque de codigo sangrado (4 espacios / tab), solo fuera de listas
    if #pila_listas == 0 and (sangria >= 4 or l:sub(1,1) == "\t") and #parrafo == 0 then
      local buf = {}
      while i <= n and (lineas[i]:match("^    ") or lineas[i]:match("^\t") or lineas[i] == "") do
        buf[#buf+1] = escapar((lineas[i]:gsub("^    ", ""):gsub("^\t", ""))); i = i + 1
      end
      while #buf > 0 and buf[#buf] == "" do table.remove(buf) end
      emitir("<pre>" .. table.concat(buf, "\n") .. "</pre>")
      goto siguiente
    end

    -- texto de parrafo (o continuacion de item de lista si estamos dentro)
    if #pila_listas > 0 and sangria > pila_listas[#pila_listas].sangria then
      emitir(" " .. inline(contenido))
    else
      cerrar_listas(0)
      parrafo[#parrafo+1] = contenido
    end
    i = i + 1

    ::siguiente::
  end
  cerrar_todo()
  return table.concat(out)   -- sin separadores: entre etiquetas de bloque no hacen falta
end

-- Titulo: el primer encabezado de nivel 1, o nil
function M.titulo(md)
  local t = md:match("^%s*#%s+([^\n]+)") or md:match("\n#%s+([^\n]+)")
  if t then return (t:gsub("%s*#*%s*$", "")) end
  local l1, l2 = md:match("^%s*([^\n]+)\n=+%s*\n")
  return l1
end

-- Documento completo con <title> y una hoja de estilos sobria
function M.documento(md)
  local titulo = M.titulo(md) or "Documento"
  return "<!DOCTYPE html><html><head><title>" .. escapar(titulo) .. "</title><style>"
    .. "body{background-color:#fbfbfd;color:#202030} h1{color:#203060} h2{color:#204080} h3{color:#305090} "
    .. "code{color:#800040} pre{background-color:#f0f0f4} blockquote{color:#505060} th{background-color:#e4e8f0}"
    .. "</style></head><body>" .. M.a_html(md) .. "</body></html>"
end

return M
