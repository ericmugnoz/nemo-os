-- navegante.lua -- explorador de archivos con miniaturas, para Nemo OS
--
-- Dos vistas: cuadricula con miniaturas grandes, y lista con detalles.
-- Los .nimg se ven como son, no como un icono generico.
--
-- COMO SE HACEN LAS MINIATURAS, que es lo unico dificil de esto:
--
-- El sistema solo tiene 64 huecos de imagen. Una carpeta con cien .nimg
-- no cabe si cada miniatura ocupa el suyo. En vez de eso se monta UN
-- MOSAICO: se carga cada archivo, se reduce con ResizeImage (que escala
-- por vecino mas cercano, justo lo que quiere el pixel art), se vuelca en
-- su celda del mosaico y se libera. En todo momento hay dos imagenes en
-- uso -- el mosaico y la de turno -- da igual cuantos archivos haya.
--
-- Se montan DOS mosaicos en la misma pasada, uno por vista, para no tener
-- que releer los archivos al cambiar de una a otra: el grande sale del
-- original y el pequeño sale del grande, que ya esta en memoria.
--
-- Y se reconstruyen solo al cambiar de carpeta. Repintar no los toca.

local gui = require("nemo_gui")
local arch = require("nemo_archivos")

local S = { RESIZE = 96, LAUNCH = 5, DRAW_IMAGE_RECT = 98, FONDO = 276, MINIATURA = 277,
            MINIATURA_DOBLE = 284 }
local function redimensionar(img, w, h) return nemo.syscall(S.RESIZE, img, w, h) == 0 end
local function dibujar_trozo(img, x, y, rx, ry, rw, rh)
  local a3 = ((rx & 0x7FFF) << 16) | (ry & 0xFFFF) | (1 << 31)   -- opaco
  nemo.syscall(S.DRAW_IMAGE_RECT, img, x, y, a3, ((rw & 0xFFFF) << 16) | (rh & 0xFFFF))
end

-- ---- aspecto ----
local FONDO      = 0xF4F4F2
local BARRA      = 0xD4D0C8
local TINTA      = 0x202020
local SUAVE      = 0x808080
local SELECCION  = 0x3A7AC8
local CARPETA    = 0xE0B040
local CARPETA_B  = 0xC08A20
local PAPEL      = 0xFFFFFF
local PAPEL_B    = 0xA0A0A0

local BARRA_H, ESTADO_H = 34, 20
local MINI_G, MINI_P = 64, 20          -- lado de la miniatura en cada vista
local CELDA_W, CELDA_H = 96, 92        -- celda de la cuadricula
local FILA_H = 26                      -- alto de una fila de la lista
local MAX_MINIATURAS = 200

-- ---- estado ----
local ventana_w, ventana_h = 900, 600
local volumen = arch.VOL_NEMOFS
local carpeta = arch.RAIZ
local ruta = {}                        -- nombres de las carpetas por las que se ha bajado
local pila = {}                        -- para el boton de atras: {volumen, carpeta, ruta}
local entradas = {}
local seleccion = 0
local vista = "cuadricula"
local scroll = 0
local estado = ""
local repintar = true
local ultimo_clic, ultimo_indice = 0, -1

-- mosaicos de miniaturas
local mos_g, mos_p = nil, nil          -- las dos imagenes
local mos_cols = 1
local mini_de = {}                     -- indice de entrada -> celda del mosaico

-- ---- utilidades ----
local function minus(s) return s:lower() end
local function es_nimg(n) return minus(n):match("%.nimg$") ~= nil end
local function es_carpeta(e) return e.tipo == arch.TIPO_CARPETA end

local function ruta_texto()
  local v = (volumen == arch.VOL_FAT) and "FAT:" or "C:"
  if #ruta == 0 then return v .. "\\" end
  return v .. "\\" .. table.concat(ruta, "\\")
end

local function tam_texto(n)
  if n < 1024 then return n .. " B" end
  if n < 1024 * 1024 then return string.format("%.1f KB", n / 1024) end
  return string.format("%.1f MB", n / 1048576)
end

local function tipo_texto(e)
  if es_carpeta(e) then return "Carpeta" end
  local ext = minus(e.nombre):match("%.([%w]+)$")
  if not ext then return "Archivo" end
  local nombres = {
    nimg = "Imagen", pro = "Programa", lua = "Programa Lua", nb = "Nemo Basic",
    txt = "Texto", md = "Documento", html = "Pagina web", anx = "Diseño Aronnax",
  }
  return nombres[ext] or (ext:upper() .. "")
end

-- La ruta completa desde la raiz, con carpetas: "DOCUMENTOS/IMAGENES/x".
-- El formato "inodo:nombre" que usa el explorador de siempre sirve para
-- ABRIR, pero con un numero de inodo no se puede construir una ruta, y un
-- programa que quiera GUARDAR donde estaba se queda sin saber donde es.
-- Solo vale en NemoFS: en FAT no hay rutas con carpetas.
local function ruta_completa(nombre)
  if volumen ~= arch.VOL_NEMOFS or #ruta == 0 then return nombre end
  return table.concat(ruta, "/") .. "/" .. nombre
end

-- ---- las miniaturas ----
local function soltar_mosaicos()
  if mos_g then gui.liberar_imagen(mos_g); mos_g = nil end
  if mos_p then gui.liberar_imagen(mos_p); mos_p = nil end
  mini_de = {}
end

local function construir_mosaicos()
  soltar_mosaicos()
  local cuales = {}
  for i, e in ipairs(entradas) do
    if not es_carpeta(e) and es_nimg(e.nombre) then
      cuales[#cuales + 1] = i
      if #cuales >= MAX_MINIATURAS then break end
    end
  end
  if #cuales == 0 then return end

  -- el mosaico, lo mas cuadrado posible y sin pasar de 1024
  local n = #cuales
  local cols = math.ceil(math.sqrt(n))
  if cols * MINI_G > 1024 then cols = 1024 // MINI_G end
  local filas = math.ceil(n / cols)
  if filas * MINI_G > 1024 then filas = 1024 // MINI_G end
  mos_cols = cols

  mos_g = gui.crear_imagen(cols * MINI_G, filas * MINI_G)
  mos_p = gui.crear_imagen(cols * MINI_P, filas * MINI_P)
  if mos_g < 0 or mos_p < 0 then soltar_mosaicos() return end

  -- UNA llamada por imagen, que llena las dos celdas de golpe. El
  -- kernel lee el archivo fila a fila y no lo carga en la tabla del
  -- sistema, asi que funciona tambien con las que pasan de 1024 -- un
  -- fondo de pantalla de 1920x1080 no tenia miniatura antes de esto. Y
  -- el encuadre (recortar lo muy alargado, centrar lo demas) lo decide
  -- el kernel, asi que el explorador y este enseñan lo mismo.
  --
  -- Antes eran DOS llamadas, una por mosaico, y cada una recorria el
  -- archivo entero por su cuenta: en la Pi 4 eso es leer cada imagen
  -- dos veces de la tarjeta. Con una sola pasada son un 22% menos de
  -- comandos al abrir una carpeta.
  -- El ORIGEN, en el sexto argumento: 32 bits bajos la
  -- carpeta, 32 altos el volumen. Sin esto, en la TARJETA no habia
  -- miniaturas: el kernel solo sabia leer .nimg de NemoFS, la llamada
  -- devolvia -1, se caia al camino viejo, que fallaba igual, y quedaban
  -- iconos genericos sin decir por que.
  local origen = (volumen << 32) | carpeta
  local hechas = 0
  for _, idx in ipairs(cuales) do
    if hechas >= cols * filas then break end
    local ruta = ruta_completa(entradas[idx].nombre)
    local cx, cy = hechas % cols, hechas // cols
    local ok = nemo.syscall(S.MINIATURA_DOBLE, ruta,
                            (mos_g << 16) | mos_p,
                            ((cx * MINI_G) << 16) | (cy * MINI_G),
                            ((cx * MINI_P) << 16) | (cy * MINI_P),
                            (MINI_G << 16) | MINI_P, origen) == 0
    if not ok then
      -- Un sistema mas antiguo no tiene la 284. Se cae a las dos
      -- llamadas de siempre para que el Navegante siga funcionando.
      ok = nemo.syscall(S.MINIATURA, ruta, mos_g,
                        ((cx * MINI_G) << 16) | (cy * MINI_G),
                        (MINI_G << 16) | MINI_G, 1, origen) == 0
      if ok then
        nemo.syscall(S.MINIATURA, ruta, mos_p,
                     ((cx * MINI_P) << 16) | (cy * MINI_P),
                     (MINI_P << 16) | MINI_P, 1, origen)
      end
    end
    if ok then
      mini_de[idx] = hechas
      hechas = hechas + 1
    end
  end
end

-- ---- listar ----
local function ordenar(a, b)
  local ca, cb = es_carpeta(a), es_carpeta(b)
  if ca ~= cb then return ca end                 -- las carpetas, primero
  return minus(a.nombre) < minus(b.nombre)
end

local function releer()
  entradas = arch.listar(carpeta, volumen)
  table.sort(entradas, ordenar)
  seleccion = 0
  scroll = 0
  construir_mosaicos()
  local nc, na = 0, 0
  for _, e in ipairs(entradas) do
    if es_carpeta(e) then nc = nc + 1 else na = na + 1 end
  end
  estado = nc .. " carpetas, " .. na .. " archivos"
  repintar = true
end

local function entrar(e)
  pila[#pila + 1] = { volumen = volumen, carpeta = carpeta, ruta = { table.unpack(ruta) } }
  carpeta = e.inodo
  ruta[#ruta + 1] = e.nombre
  releer()
end

local function atras()
  if #pila == 0 then return end
  local p = pila[#pila]; pila[#pila] = nil
  volumen, carpeta, ruta = p.volumen, p.carpeta, p.ruta
  releer()
end

local function arriba()
  if #ruta == 0 then return end
  -- se vuelve a bajar desde la raiz: no hay syscall de "carpeta padre",
  -- y rehacer el camino es barato con carpetas de este tamaño
  local destino = { table.unpack(ruta) }
  destino[#destino] = nil
  pila[#pila + 1] = { volumen = volumen, carpeta = carpeta, ruta = { table.unpack(ruta) } }
  carpeta, ruta = arch.RAIZ, {}
  for _, nombre in ipairs(destino) do
    local e = arch.buscar(nombre, carpeta, volumen)
    if e and es_carpeta(e) then carpeta = e.inodo; ruta[#ruta + 1] = nombre end
  end
  releer()
end

local function cambiar_volumen(v)
  if v == volumen then return end
  volumen, carpeta, ruta, pila = v, arch.RAIZ, {}, {}
  releer()
end

-- ---- abrir ----
local function abrir(e)
  if es_carpeta(e) then entrar(e) return end
  local n = minus(e.nombre)
  local arg = tostring(carpeta) .. ":" .. e.nombre
  if n:match("%.pro$") or n:match("%.lua$") then
    nemo.syscall(S.LAUNCH, e.nombre, "", carpeta)
  elseif n:match("%.nimg$") then
    nemo.syscall(S.LAUNCH, "pintor.lua", ruta_completa(e.nombre), carpeta)
  elseif n:match("%.nb$") then
    nemo.syscall(S.LAUNCH, "ide.pro", arg, carpeta)
  elseif n:match("%.html?$") or n:match("%.md$") then
    nemo.syscall(S.LAUNCH, "visor.lua", arg, carpeta)
  else
    nemo.syscall(S.LAUNCH, "editor.pro", arg, carpeta)
  end
  estado = "Abriendo " .. e.nombre
end

-- ---- dibujo ----
local function zona() return 0, BARRA_H, ventana_w, ventana_h - BARRA_H - ESTADO_H end

local function glifo_carpeta(x, y, lado)
  local h = lado * 3 // 4
  gui.rect(x, y + lado // 6, lado, lado // 8, CARPETA_B)         -- la pestaña
  gui.rect(x, y + lado // 4, lado, h, CARPETA)
  gui.rect(x, y + lado // 4, lado, 1, CARPETA_B)
end

local function glifo_papel(x, y, lado)
  local m = lado // 6
  gui.rect(x + m, y, lado - 2 * m, lado, PAPEL)
  gui.rect(x + m, y, lado - 2 * m, 1, PAPEL_B)
  gui.rect(x + m, y + lado - 1, lado - 2 * m, 1, PAPEL_B)
  gui.rect(x + m, y, 1, lado, PAPEL_B)
  gui.rect(x + lado - m - 1, y, 1, lado, PAPEL_B)
  for k = 1, 3 do                                                -- rayas de texto
    gui.rect(x + m + 3, y + 6 + k * 5, lado - 2 * m - 6, 1, PAPEL_B)
  end
end

-- La miniatura de una entrada, o su glifo si no tiene
local function dibujar_icono(i, e, x, y, lado, mosaico, cell)
  local c = mini_de[i]
  if c and mosaico then
    local cx, cy = (c % mos_cols) * cell, (c // mos_cols) * cell
    gui.rect(x, y, lado, lado, 0xFFFFFF)
    dibujar_trozo(mosaico, x, y, cx, cy, cell, cell)
    gui.rect(x, y, lado, 1, PAPEL_B); gui.rect(x, y + lado - 1, lado, 1, PAPEL_B)
    gui.rect(x, y, 1, lado, PAPEL_B); gui.rect(x + lado - 1, y, 1, lado, PAPEL_B)
  elseif es_carpeta(e) then
    glifo_carpeta(x, y, lado)
  else
    glifo_papel(x, y, lado)
  end
end

local function recortar(s, max_px)
  local ancho = gui.medir_texto and gui.medir_texto(s) or (#s * 8)
  if ancho <= max_px then return s end
  local n = #s
  while n > 3 do
    n = n - 1
    local t = s:sub(1, n) .. ".."
    local a = gui.medir_texto and gui.medir_texto(t) or (#t * 8)
    if a <= max_px then return t end
  end
  return s:sub(1, 3)
end

local function columnas()
  local _, _, zw = zona()
  local c = zw // CELDA_W
  if c < 1 then c = 1 end
  return c
end

local function pintar_cuadricula()
  local zx, zy, zw, zh = zona()
  local cols = columnas()
  local primera = scroll * cols
  local filas_visibles = zh // CELDA_H
  for f = 0, filas_visibles do
    for c = 0, cols - 1 do
      local i = primera + f * cols + c + 1
      local e = entradas[i]
      if e then
        local x = zx + c * CELDA_W
        local y = zy + f * CELDA_H
        if y + CELDA_H <= zy + zh then
          if i == seleccion then gui.rect(x + 2, y + 2, CELDA_W - 4, CELDA_H - 4, 0xD8E4F4) end
          dibujar_icono(i, e, x + (CELDA_W - MINI_G) // 2, y + 8, MINI_G, mos_g, MINI_G)
          local t = recortar(e.nombre, CELDA_W - 8)
          local w = gui.medir_texto and gui.medir_texto(t) or (#t * 8)
          gui.texto(x + (CELDA_W - w) // 2, y + MINI_G + 14, t, TINTA)
        end
      end
    end
  end
end

local function pintar_lista()
  local zx, zy, zw, zh = zona()
  local visibles = zh // FILA_H
  for f = 0, visibles - 1 do
    local i = scroll + f + 1
    local e = entradas[i]
    if e then
      local y = zy + f * FILA_H
      if i == seleccion then gui.rect(zx, y, zw, FILA_H, 0xD8E4F4) end
      dibujar_icono(i, e, zx + 4, y + 3, MINI_P, mos_p, MINI_P)
      gui.texto(zx + 30, y + 5, recortar(e.nombre, zw - 260), TINTA)
      gui.texto(zx + zw - 240, y + 5, tipo_texto(e), SUAVE)
      if not es_carpeta(e) then
        gui.texto(zx + zw - 110, y + 5, tam_texto(e.tamano), SUAVE)
      end
    end
  end
end

-- Con palabras, no con signos: la fuente del sistema es de caja alta y
-- no trae '<' ni '^' -- los botones salian vacios.
local BOTONES = {
  { "atras", "← Atras" }, { "arriba", "↑ Arriba" },
  { "nemofs", "NemoFS" }, { "fat", "FAT" },
  { "vista", "Vista" }, { "actualizar", "Releer" }, { "fondo", "Fondo" },
}
local function ancho_boton(t)
  local w = (gui.medir_texto and gui.medir_texto(t) or (#t * 8)) + 16
  if w < 44 then w = 44 end
  return w
end
local function rect_boton(k)
  local x = 4
  for i, b in ipairs(BOTONES) do
    local w = ancho_boton(b[2])
    if i == k then return x, 4, w, 26 end
    x = x + w + 4
  end
end

local function pintar_barra()
  gui.rect(0, 0, ventana_w, BARRA_H, BARRA)
  local fin = 4
  for i, b in ipairs(BOTONES) do
    local x, y, w = rect_boton(i)
    local activo = (b[1] == "nemofs" and volumen == arch.VOL_NEMOFS)
                or (b[1] == "fat" and volumen == arch.VOL_FAT)
    gui.rect(x, y, w, h, activo and 0x8090B0 or 0xECECEC)
    gui.rect(x, y, w, 1, 0x909090)
    local tw = gui.medir_texto and gui.medir_texto(b[2]) or (#b[2] * 8)
    gui.texto(x + (w - tw) // 2, y + 6, b[2], TINTA)
    fin = x + w + 4
  end
  gui.rect(fin, 4, ventana_w - fin - 4, 26, 0xFFFFFF)
  gui.texto(fin + 6, 10, recortar(ruta_texto(), ventana_w - fin - 16), TINTA)
end

local function pintar()
  local zx, zy, zw, zh = zona()
  gui.rect(zx, zy, zw, zh, FONDO)
  if vista == "cuadricula" then pintar_cuadricula() else pintar_lista() end
  pintar_barra()
  gui.rect(0, ventana_h - ESTADO_H, ventana_w, ESTADO_H, BARRA)
  local sel = (seleccion > 0 and entradas[seleccion]) and ("   " .. entradas[seleccion].nombre) or ""
  gui.texto(6, ventana_h - ESTADO_H + 3, estado .. sel, TINTA)
end

-- ---- clics ----
local function indice_en(mx, my)
  local zx, zy, zw, zh = zona()
  if my < zy or my >= zy + zh then return 0 end
  if vista == "cuadricula" then
    local cols = columnas()
    local c = (mx - zx) // CELDA_W
    local f = (my - zy) // CELDA_H
    if c < 0 or c >= cols then return 0 end
    return scroll * cols + f * cols + c + 1
  end
  return scroll + (my - zy) // FILA_H + 1
end

local function max_scroll()
  local _, _, _, zh = zona()
  if vista == "cuadricula" then
    local cols = columnas()
    local filas = math.ceil(#entradas / cols)
    local caben = zh // CELDA_H
    return math.max(0, filas - caben)
  end
  return math.max(0, #entradas - zh // FILA_H)
end

-- ---- arranque ----
gui.crear_ventana("Navegante", 60, 60, ventana_w, ventana_h)
releer()

while true do
  if gui.tamano_cambiado() then
    ventana_w, ventana_h = gui.tamano_ventana()
    repintar = true
  end

  local ev = gui.sondear_evento()
  if ev == gui.EVENT_WINDOWCLOSE then break end

  local k = gui.siguiente_codigo()
  if k == 1 then break end                       -- Esc
  if k == 14 then atras() end                    -- Retroceso: carpeta anterior
  if k == 28 and seleccion > 0 then abrir(entradas[seleccion]) end   -- Enter

  local r = gui.rueda()
  if r ~= 0 then
    scroll = math.min(math.max(0, scroll - r), max_scroll())
    repintar = true
  end

  if gui.clic(1) then
    local mx, my = gui.raton()
    if mx then
      if my < BARRA_H then
        for i, b in ipairs(BOTONES) do
          local x, y, w, h = rect_boton(i)
          if mx >= x and mx < x + w and my >= y and my < y + h then
            if b[1] == "atras" then atras()
            elseif b[1] == "arriba" then arriba()
            elseif b[1] == "nemofs" then cambiar_volumen(arch.VOL_NEMOFS)
            elseif b[1] == "fat" then cambiar_volumen(arch.VOL_FAT)
            elseif b[1] == "vista" then
              vista = (vista == "cuadricula") and "lista" or "cuadricula"
              scroll = 0
            elseif b[1] == "actualizar" then releer()
            elseif b[1] == "fondo" then
              -- Poner la imagen elegida como fondo de pantalla. Se manda
              -- la RUTA completa, no el nombre: el kernel busca en la
              -- raiz y en DOCUMENTOS, y el archivo puede estar en otra
              -- carpeta cualquiera.
              local e = entradas[seleccion]
              if not e or es_carpeta(e) or not es_nimg(e.nombre) then
                estado = "Elige antes una imagen .nimg"
              elseif nemo.syscall(S.FONDO, ruta_completa(e.nombre), 0) == 0 then
                estado = "Fondo puesto: " .. e.nombre
              else
                estado = "No se pudo poner de fondo"
              end
            end
            repintar = true
            break
          end
        end
      else
        local i = indice_en(mx, my)
        if entradas[i] then
          -- doble clic: dos clics en la misma entrada, seguidos. Se mide
          -- con MilliSecs, que para esto sobra.
          local ahora = gui.milisegundos and gui.milisegundos() or nemo.syscall(2) * 10
          if i == ultimo_indice and (ahora - ultimo_clic) < 500 then
            abrir(entradas[i])
            ultimo_indice = -1
          else
            seleccion = i
            ultimo_indice, ultimo_clic = i, ahora
          end
          repintar = true
        end
      end
    end
  end

  if repintar then
    pintar()
    repintar = false
  end
  nemo.pump()
end

soltar_mosaicos()
