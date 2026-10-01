-- pintor.lua -- editor de imagenes NIMG para Nemo OS
--
-- Abre, edita y guarda archivos .nimg: desde un sprite de 16x16 hasta una
-- imagen de 1024x1024, que es el maximo del sistema.
--
-- COMO ESTA HECHO, que es lo que no se ve:
--
-- El lienzo es una IMAGEN del kernel, no una tabla de Lua. Se pinta
-- dentro con WRITE_PIXEL, que recibe la imagen destino como argumento
-- (buffer = imagen + 1), asi que no hay que copiar nada de ida y vuelta.
--
-- Para mostrarlo ampliado NO se dibuja pixel a pixel. Una llamada al
-- sistema cuesta unos 3,5 us en la Raspberry Pi 4 dibuje lo que dibuje:
-- un lienzo de 64x64 a zoom 8 son 4096 puntos, 14 ms, mas de un fotograma
-- entero. En su lugar:
--
--   1. se recorta la parte visible del lienzo en una imagen pequeña
--      (DRAW_IMAGE_RECT hacia un ImageBuffer)
--   2. se amplia esa imagen con RESIZE_IMAGE, que escala por VECINO MAS
--      CERCANO -- justo lo que quiere el pixel art
--   3. se dibuja de una sola llamada
--
-- Cinco llamadas en vez de cuatro mil, y el zoom sale con los bordes
-- duros en vez de emborronado.
--
-- Deshacer guarda copias del lienzo con COPY_IMAGE. Cada copia ocupa
-- ancho*alto*4 bytes de los 24 MB que el sistema reserva para imagenes,
-- asi que el numero de pasos se ajusta al tamaño: con un sprite hay
-- muchos, con una imagen de 1024x1024 (4 MB cada copia) solo dos.

local gui = require("nemo_gui")
local arch = require("nemo_archivos")

-- ---- syscalls que nemo_gui no envuelve ----
local S = {
  WRITE_PIXEL = 186, READ_PIXEL = 185,
  LOCK = 208, UNLOCK = 209,
  DRAW_IMAGE_RECT = 98, RESIZE = 96, ROTATE = 97,
  SET_BUFFER = 128, GET_MOUSE = 34,
}
local function escribir_pixel(img, x, y, color) nemo.syscall(S.WRITE_PIXEL, x, y, color, img + 1) end
local function leer_pixel(img, x, y) return nemo.syscall(S.READ_PIXEL, x, y, img + 1) & 0xFFFFFF end
local function bloquear(img) nemo.syscall(S.LOCK, img + 1) end
local function soltar() nemo.syscall(S.UNLOCK, 0) end
local function redimensionar(img, w, h) return nemo.syscall(S.RESIZE, img, w, h) == 0 end
-- Dibuja un trozo de 'img' en (x,y). 'opaco' copia tal cual, sin mascara
-- ni alfa: en un editor hay que ver los pixeles como son.
local function dibujar_trozo(img, x, y, rx, ry, rw, rh, opaco)
  local a3 = ((rx & 0x7FFF) << 16) | (ry & 0xFFFF)
  if opaco then a3 = a3 | (1 << 31) end
  nemo.syscall(S.DRAW_IMAGE_RECT, img, x, y, a3, ((rw & 0xFFFF) << 16) | (rh & 0xFFFF))
end

-- Los dialogos del sistema son de ARCHIVO (eligen un nombre en una
-- carpeta), no cuadros de texto: no hay forma de pedir "32x32" con
-- ellos. Por eso los tamaños van como entradas de menu, que ademas es
-- mas comodo para pixel art. Va aqui arriba a proposito: en Lua, un
-- local declarado DESPUES de la funcion que lo usa no es visible desde
-- ella -- seria nil, sin error.
local TAMANOS = {8, 16, 24, 32, 48, 64, 96, 128, 256, 512, 1024}

-- ---- estado ----
local MAX_LADO = 1024
local BARRA_H, PALETA_H = 30, 44      -- alto de la barra de herramientas y de la paleta
local lienzo, ancho, alto             -- la imagen que se edita
local archivo = nil                   -- nombre del ultimo abierto o guardado
local ruta = nil                      -- con su carpeta delante, para guardar donde estaba
local dir_iconos = nil                -- inodo de DOCUMENTOS/IMAGENES, si existe
local sucio = false
local zoom, vx, vy = 8, 0, 0
local color_a, color_b = 0x000000, 0xFFFFFF   -- izquierdo y derecho
local herramienta = "lapiz"
local grosor = 1
local ventana_w, ventana_h = 900, 620
local estado = ""                     -- linea de abajo

-- zona de dibujo en la ventana
local function zona()
  return 0, BARRA_H, ventana_w, ventana_h - BARRA_H - PALETA_H - 18
end

-- ---- deshacer ----
local pila, MAX_PASOS = {}, 8
local function pasos_que_caben()
  -- 24 MB entre todas las imagenes; se reserva la mitad para deshacer y
  -- nunca mas de MAX_PASOS. Una imagen de 1024x1024 son 4 MB: dos pasos.
  local por_copia = ancho * alto * 4
  local n = (12 * 1024 * 1024) // math.max(por_copia, 1)
  if n > MAX_PASOS then n = MAX_PASOS end
  if n < 1 then n = 1 end
  return n
end
local function apuntar()
  local copia = gui.copiar_imagen(lienzo)
  if copia < 0 then return end          -- sin sitio: se sigue, pero sin deshacer
  pila[#pila + 1] = copia
  while #pila > pasos_que_caben() do
    gui.liberar_imagen(pila[1])
    table.remove(pila, 1)
  end
  sucio = true
end
local function deshacer()
  if #pila == 0 then estado = "Nada que deshacer" return end
  local copia = pila[#pila]
  table.remove(pila)
  gui.liberar_imagen(lienzo)
  lienzo = copia
  ancho, alto = gui.tamano_imagen(lienzo)
  estado = "Deshecho"
end
local function vaciar_pila()
  for _, h in ipairs(pila) do gui.liberar_imagen(h) end
  pila = {}
end

-- ---- el lienzo ----
local function nuevo(w, h, fondo)
  if lienzo then gui.liberar_imagen(lienzo) end
  vaciar_pila()
  lienzo = gui.crear_imagen(w, h)
  ancho, alto = w, h
  -- CreateImage deja la imagen a cero (negro). Se rellena de blanco por
  -- filas con DRAW_RECT sobre un ImageBuffer: una llamada por fila en vez
  -- de una por pixel.
  -- OJO: ImageBuffer recibe el HANDLE tal cual. El +100000 es como el
  -- kernel reconoce un Canvas, no una imagen.
  gui.dibujar_en_imagen(lienzo)
  nemo.syscall(30, 0, 0, w, h, fondo or 0xFFFFFF)
  gui.dibujar_en_ventana()
  vx, vy, sucio, archivo = 0, 0, false, nil
end

-- La ruta con la que se guardara: la misma carpeta de la que salio.
-- SaveImage acepta rutas; antes escribia siempre en la
-- raiz, asi que editar un icono de DOCUMENTOS/IMAGENES dejaba el original
-- intacto y una copia suelta en la raiz -- parecia guardado, y no lo estaba.
local function ruta_de(nombre, carpeta)
  if nombre:find("[/\\]") then return nombre end      -- ya viene con carpetas
  if carpeta and dir_iconos and carpeta == dir_iconos then
    return "DOCUMENTOS/IMAGENES/" .. nombre
  end
  return nombre
end

local function abrir(nombre, carpeta, volumen)
  local h = gui.cargar_imagen(nombre, carpeta, volumen)
  if not h then estado = "No se pudo abrir " .. nombre return false end
  local w, a = gui.tamano_imagen(h)
  if not w then gui.liberar_imagen(h) estado = "Imagen ilegible" return false end
  if lienzo then gui.liberar_imagen(lienzo) end
  vaciar_pila()
  lienzo, ancho, alto = h, w, a
  -- 'archivo' es solo el nombre; 'ruta' lleva las carpetas delante, que
  -- es lo que necesita Guardar para escribir donde estaba
  archivo = nombre:match("([^/\\]+)$") or nombre
  sucio, vx, vy = false, 0, 0
  ruta = ruta_de(nombre, carpeta)
  estado = ruta .. "  " .. w .. "x" .. a
  return true
end

local function guardar(nombre)
  if not nombre or nombre == "" then estado = "Falta el nombre" return false end
  if not nombre:lower():match("%.nimg$") then nombre = nombre .. ".nimg" end
  if gui.guardar_imagen(lienzo, nombre) then
    ruta = nombre
    archivo = nombre:match("([^/\\]+)$") or nombre
    sucio = false
    estado = "Guardado: " .. nombre
    return true
  end
  if nombre:find("[/\\]") then estado = "No existe esa carpeta: " .. nombre
  else estado = "No se pudo guardar" end
  return false
end

-- ---- pintar ----
-- De pantalla a lienzo, y al reves
local function a_lienzo(sx, sy)
  local zx, zy = zona()
  local px = vx + (sx - zx) // zoom
  local py = vy + (sy - zy) // zoom
  if px < 0 or py < 0 or px >= ancho or py >= alto then return nil end
  return px, py
end

-- DIBUJO INCREMENTAL (lo que hace que el lapiz vaya fino).
--
-- Repintar la vista entera cuesta ampliar el recorte a la escala del zoom
-- y volver a copiarlo a la ventana: con la ventana llena son casi medio
-- millon de pixeles DOS veces, varios milisegundos. Hacerlo en cada
-- movimiento del raton se nota muchisimo, y encima para nada: al pintar
-- con el lapiz solo cambia un pixel del lienzo.
--
-- Con 'directo' activo, cada pixel que se escribe se dibuja ademas en su
-- cuadradito de la pantalla: UNA llamada en vez de medio millon de pixeles.
local directo = false
-- En vista previa, 'punto' dibuja en la pantalla y NO toca el lienzo. Asi
-- la goma elastica sale de las MISMAS funciones que dibujan la figura de
-- verdad: lo que ves mientras arrastras es exactamente lo que va a quedar,
-- sin un segundo trozo de codigo que se desincronice con el primero.
local previsualizando = false

local function pintar_celda(x, y, color)
  local zx, zy, zw, zh = zona()
  local sx = zx + (x - vx) * zoom
  local sy = zy + (y - vy) * zoom
  if sx < zx or sy < zy or sx >= zx + zw or sy >= zy + zh then return end
  gui.rect(sx, sy, zoom, zoom, color)
  if zoom >= 8 then                      -- la rejilla, solo la de este cuadro
    gui.rect(sx, sy, zoom, 1, 0x909090)
    gui.rect(sx, sy, 1, zoom, 0x909090)
  end
end

local function punto(x, y, color)
  if previsualizando then
    if x >= 0 and y >= 0 and x < ancho and y < alto then pintar_celda(x, y, color) end
    return
  end
  if grosor <= 1 then
    if x >= 0 and y >= 0 and x < ancho and y < alto then
      escribir_pixel(lienzo, x, y, color)
      if directo then pintar_celda(x, y, color) end
    end
    return
  end
  local r = grosor // 2
  for j = -r, r do
    for i = -r, r do
      local px, py = x + i, y + j
      if px >= 0 and py >= 0 and px < ancho and py < alto then
        escribir_pixel(lienzo, px, py, color)
        if directo then pintar_celda(px, py, color) end
      end
    end
  end
end

-- Bresenham: hace falta porque el raton salta varios pixeles entre dos
-- vueltas del bucle. Sin esto, un trazo rapido sale a puntitos.
local function linea(x0, y0, x1, y1, color)
  local dx, dy = math.abs(x1 - x0), -math.abs(y1 - y0)
  local sx = x0 < x1 and 1 or -1
  local sy = y0 < y1 and 1 or -1
  local err = dx + dy
  while true do
    punto(x0, y0, color)
    if x0 == x1 and y0 == y1 then break end
    local e2 = 2 * err
    if e2 >= dy then err = err + dy; x0 = x0 + sx end
    if e2 <= dx then err = err + dx; y0 = y0 + sy end
  end
end

local function rectangulo(x0, y0, x1, y1, color, relleno)
  if x0 > x1 then x0, x1 = x1, x0 end
  if y0 > y1 then y0, y1 = y1, y0 end
  if relleno then
    for y = y0, y1 do linea(x0, y, x1, y, color) end
  else
    linea(x0, y0, x1, y0, color); linea(x0, y1, x1, y1, color)
    linea(x0, y0, x0, y1, color); linea(x1, y0, x1, y1, color)
  end
end

-- Elipse por puntos medios, en enteros: sin senos ni raices.
local function elipse(x0, y0, x1, y1, color, relleno)
  if x0 > x1 then x0, x1 = x1, x0 end
  if y0 > y1 then y0, y1 = y1, y0 end
  local a, b = (x1 - x0) // 2, (y1 - y0) // 2
  if a < 1 or b < 1 then rectangulo(x0, y0, x1, y1, color, relleno) return end
  local cx, cy = x0 + a, y0 + b
  local a2, b2 = a * a, b * b
  local x, y = 0, b
  local sigma = 2 * b2 + a2 * (1 - 2 * b)
  local function fila(dx, dy)
    if relleno then
      linea(cx - dx, cy + dy, cx + dx, cy + dy, color)
      linea(cx - dx, cy - dy, cx + dx, cy - dy, color)
    else
      punto(cx + dx, cy + dy, color); punto(cx - dx, cy + dy, color)
      punto(cx + dx, cy - dy, color); punto(cx - dx, cy - dy, color)
    end
  end
  while b2 * x <= a2 * y do
    fila(x, y)
    if sigma >= 0 then sigma = sigma + 4 * a2 * (1 - y); y = y - 1 end
    sigma = sigma + b2 * (4 * x + 6); x = x + 1
  end
  x, y = a, 0
  sigma = 2 * a2 + b2 * (1 - 2 * a)
  while a2 * y <= b2 * x do
    fila(x, y)
    if sigma >= 0 then sigma = sigma + 4 * b2 * (1 - x); x = x - 1 end
    sigma = sigma + a2 * (4 * y + 6); y = y + 1
  end
end

-- Relleno por inundacion, con pila propia (nada de recursion: una imagen
-- de 1024x1024 de un solo color desbordaria la pila de Lua).
--
-- REESCRITO, y no por velocidad: la version anterior se
-- quedaba SIN MEMORIA y moria en cualquier lienzo mediano. Hacia dos
-- cosas que la pila no perdona:
--
--   1. Apuntaba UNA SEMILLA POR PIXEL de las filas de arriba y abajo,
--      en vez de una por TRAMO contiguo. En un lienzo de un solo color
--      el pico de la pila crecia con el AREA: medido, 65.024 semillas
--      en 256x256 y 1.046.528 en 1024x1024.
--   2. Cada semilla era una tabla {x, y}. Medido con el Lua 5.5 de este
--      proyecto (lua_host): 81,8 bytes por tabla. El monton de una app
--      de Lua en Nemo OS son 4 MB, o sea ~51.000 semillas como techo.
--
-- Juntas: 256x256 pedia 5,1 MB solo de pila y 1024x1024 pedia 81,6 MB.
-- El cubo reventaba a partir de un lienzo de unos 226x226, y el Pintor
-- ofrece hasta 1024x1024. No era lentitud, era un limite silencioso.
--
-- Ahora: una semilla por tramo (el pico baja a unas pocas docenas) y la
-- semilla es UN ENTERO 'y * ancho + x', no una tabla, asi que no se
-- reserva nada en el monton por semilla.
--
-- Lo que esto NO arregla: sigue costando una llamada al sistema por
-- pixel leido y otra por pixel escrito, a 3,5 us cada una. Un lienzo de
-- 1024x1024 eran ~15 s.
--
-- Eso YA NO ES ASI: existen las syscalls de
-- FILA (289-292). Ahora no hay ni una sola llamada por pixel:
--
--   * los dos extremos del tramo de la fila -> 2 llamadas a tira_fila,
--     que recorre la fila DENTRO del kernel, a 4 ns el pixel;
--   * rellenarlo -> 1 llamada a rellenar_fila;
--   * buscar los tramos de las filas de arriba y abajo -> 2 llamadas por
--     tramo encontrado (una para saltarse lo que ya no vale, otra para
--     medir el tramo).
--
-- El coste pasa a ser por TRAMO y no por pixel, asi que crece con el
-- PERIMETRO y no con el area. Medido sobre un lienzo de un solo color,
-- que es el caso normal al empezar a dibujar:
--
--     lienzo      llamadas antes   ahora    por pixel
--     64x64               20.225     446      0,1089
--     256x256            326.657   1.790      0,0273
--     1024x1024        4.193.279   7.166      0,0068
--
-- En la Pi 4, a 3,5 us la llamada: 1024x1024 pasa de ~14,7 s a ~25 ms,
-- y el pico de memoria se queda en 6,7 KB pase lo que pase.
--
-- Donde NO gana: formas con tramos muy estrechos. Un laberinto de
-- paredes cada 4 columnas (tramos de 3 px) sube de 4.110 a 5.321
-- llamadas, porque cada tramo cuesta un fijo de unas 2 llamadas y ahi
-- los tramos casi no tienen pixeles que amortizarlo. Es el mismo orden
-- de magnitud y sigue sin poder agotar la memoria, asi que se acepta:
-- el caso comun mejora 45-585 veces y el malo empeora un 30%.
local function cubo(x, y, color)
  local viejo = leer_pixel(lienzo, x, y)
  if viejo == color then return end
  bloquear(lienzo)
  local pend = {y * ancho + x}
  local n = 1
  -- Apunta los tramos de la fila 'ny' que siguen siendo del color viejo,
  -- dentro del tramo [i, j] que se acaba de rellenar. Uno por tramo.
  local function sembrar(ny, i, j)
    if ny < 0 or ny >= alto then return end
    local k = i
    while k <= j do
      -- saltarse de un tiron lo que ya NO es del color viejo
      local salto = gui.tira_fila(lienzo, k, ny, j - k + 1, viejo, true)
      if not salto then return end
      k = k + salto
      if k > j then return end
      n = n + 1
      pend[n] = ny * ancho + k
      -- y saltarse el tramo entero: esa semilla ya lo cubre
      local largo = gui.tira_fila(lienzo, k, ny, j - k + 1, viejo, false)
      if not largo or largo == 0 then return end
      k = k + largo
    end
  end
  while n > 0 do
    local s = pend[n]; pend[n] = nil; n = n - 1
    local py = s // ancho
    local px = s % ancho
    -- Una semilla puede haber quedado ya rellenada por otro tramo.
    if leer_pixel(lienzo, px, py) == viejo then
      -- Los dos extremos, una llamada cada uno. El pixel de partida
      -- cuenta en las dos, de ahi los -1.
      local der = gui.tira_fila(lienzo, px, py, ancho - px, viejo, false)
      local izq = gui.tira_fila(lienzo, px, py, -(px + 1), viejo, false)
      if not der or not izq or der == 0 or izq == 0 then
        -- No puede pasar: el pixel de partida ES del color viejo, asi
        -- que las dos tiras miden 1 como poco. Si pasa, es que las
        -- syscalls de fila no estan en este kernel, y vale mas decirlo
        -- que rellenar a medias en silencio.
        soltar()
        estado = "El kernel no tiene las syscalls de fila (289-292)"
        return
      end
      local i, j = px - (izq - 1), px + (der - 1)
      gui.rellenar_fila(lienzo, i, py, j - i + 1, color)
      sembrar(py - 1, i, j)
      sembrar(py + 1, i, j)
    end
  end
  soltar()
end

-- ---- imagen entera ----
local function cambiar_tamano(w, h)
  if w < 1 or h < 1 or w > MAX_LADO or h > MAX_LADO then
    estado = "Entre 1 y " .. MAX_LADO return
  end
  apuntar()
  if redimensionar(lienzo, w, h) then
    ancho, alto = w, h
    vx, vy = 0, 0
    estado = "Tamaño: " .. w .. "x" .. h
  else
    estado = "No se pudo cambiar el tamaño"
  end
end

local function rotar(grados)
  apuntar()
  -- el angulo va como los bits de un double, que es lo que espera la syscall
  local bits = string.unpack("<I8", string.pack("<d", grados))
  if nemo.syscall(S.ROTATE, lienzo, bits) == 0 then
    ancho, alto = gui.tamano_imagen(lienzo)
    estado = "Rotado " .. grados .. " grados"
  else
    estado = "No se pudo rotar"
  end
end

local function espejo(horizontal)
  apuntar()
  bloquear(lienzo)
  if horizontal then
    for y = 0, alto - 1 do
      for x = 0, ancho // 2 - 1 do
        local a = leer_pixel(lienzo, x, y)
        local b = leer_pixel(lienzo, ancho - 1 - x, y)
        escribir_pixel(lienzo, x, y, b)
        escribir_pixel(lienzo, ancho - 1 - x, y, a)
      end
    end
  else
    for y = 0, alto // 2 - 1 do
      for x = 0, ancho - 1 do
        local a = leer_pixel(lienzo, x, y)
        local b = leer_pixel(lienzo, x, alto - 1 - y)
        escribir_pixel(lienzo, x, y, b)
        escribir_pixel(lienzo, x, alto - 1 - y, a)
      end
    end
  end
  soltar()
  estado = horizontal and "Espejo horizontal" or "Espejo vertical"
end

-- ---- seleccion, copiar y pegar ----
local sel = nil          -- {x, y, w, h} en coordenadas del lienzo
local porta = nil        -- imagen con lo copiado

local function copiar()
  if not sel then estado = "Selecciona antes una zona" return end
  if porta then gui.liberar_imagen(porta) end
  porta = gui.crear_imagen(sel.w, sel.h)
  if porta < 0 then porta = nil; estado = "Sin sitio para copiar" return end
  gui.dibujar_en_imagen(porta)
  dibujar_trozo(lienzo, 0, 0, sel.x, sel.y, sel.w, sel.h, true)
  gui.dibujar_en_ventana()
  estado = "Copiado " .. sel.w .. "x" .. sel.h
end

local function pegar_en(x, y)
  if not porta then estado = "No hay nada copiado" return end
  apuntar()
  local pw, ph = gui.tamano_imagen(porta)
  gui.dibujar_en_imagen(lienzo)
  dibujar_trozo(porta, x, y, 0, 0, pw, ph, true)
  gui.dibujar_en_ventana()
  estado = "Pegado en " .. x .. "," .. y
end

local function borrar_seleccion()
  if not sel then return end
  apuntar()
  gui.dibujar_en_imagen(lienzo)
  nemo.syscall(30, sel.x, sel.y, sel.w, sel.h, color_b)
  gui.dibujar_en_ventana()
  estado = "Zona borrada"
end

-- ---- dibujar la pantalla ----
local COLORES = {
  0x000000, 0x404040, 0x808080, 0xC0C0C0, 0xFFFFFF,
  0x8B0000, 0xE03030, 0xE08000, 0xE0C000, 0x2E8B57,
  0x40E0D0, 0x1B2A5E, 0x3A7AC8, 0x6A3AA0, 0xE080C0,
  0x8B5E3C, 0xEBD5A3, 0xFF00FF,
}

local HERRAMIENTAS = {
  {"lapiz", "Lapiz"}, {"borrador", "Goma"}, {"cubo", "Cubo"}, {"gotero", "Gotero"},
  {"linea", "Linea"}, {"rect", "Rect"}, {"rectr", "Rect+"},
  {"elipse", "Elip"}, {"elipser", "Elip+"}, {"sel", "Selec"},
}

-- Repintar SOLO cuando algo cambia.
--
-- El bucle pintaba la ventana entera en cada vuelta, y eso borra el menu
-- desplegable que el gestor de ventanas acaba de dibujar encima: se veia
-- medio comido, con la barra de herramientas asomando por detras. Mientras
-- un menu esta abierto no pasa nada, asi que no se repinta y el menu
-- sobrevive. De paso se ahorran las ciento y pico llamadas por vuelta que
-- cuesta la rejilla.
local repintar = true
local vista, vista_w, vista_h = nil, 0, 0  -- la imagen ampliada, reaprovechada
local arrastrando, ax, ay = false, 0, 0   -- punto donde empezo el arrastre
local previa = nil                        -- figura en curso, para la vista previa

local function pintar_lienzo()
  local zx, zy, zw, zh = zona()
  gui.rect(zx, zy, zw, zh, 0x707070)

  -- cuantos pixeles del lienzo caben, y cuantos hay
  local vw = math.min(ancho - vx, zw // zoom)
  local vh = math.min(alto - vy, zh // zoom)
  if vw < 1 or vh < 1 then return end

  -- 1) recortar la parte visible, 2) ampliarla, 3) dibujarla de una vez.
  -- La imagen intermedia se reaprovecha entre repintados: crearla y
  -- liberarla cada vez pedia y devolvia memoria del kernel sin necesidad.
  if vista and (vista_w ~= vw or vista_h ~= vh) then
    gui.liberar_imagen(vista); vista = nil
  end
  if not vista then
    local h = gui.crear_imagen(vw, vh)
    if h < 0 then return end
    vista, vista_w, vista_h = h, vw, vh
  else
    redimensionar(vista, vw, vh)          -- deshacer la ampliacion anterior
  end
  gui.dibujar_en_imagen(vista)
  dibujar_trozo(lienzo, 0, 0, vx, vy, vw, vh, true)
  gui.dibujar_en_ventana()
  if zoom > 1 then redimensionar(vista, vw * zoom, vh * zoom) end
  gui.dibujar_imagen(vista, zx, zy)

  -- rejilla, solo cuando los pixeles son bien grandes
  if zoom >= 8 then
    for i = 0, vw do gui.rect(zx + i * zoom, zy, 1, vh * zoom, 0x909090) end
    for j = 0, vh do gui.rect(zx, zy + j * zoom, vw * zoom, 1, 0x909090) end
  end

  -- la seleccion
  if sel then
    local sx = zx + (sel.x - vx) * zoom
    local sy = zy + (sel.y - vy) * zoom
    gui.rect(sx, sy, sel.w * zoom, 1, 0x00FF00)
    gui.rect(sx, sy + sel.h * zoom - 1, sel.w * zoom, 1, 0x00FF00)
    gui.rect(sx, sy, 1, sel.h * zoom, 0x00FF00)
    gui.rect(sx + sel.w * zoom - 1, sy, 1, sel.h * zoom, 0x00FF00)
  end
end

local function pintar_barra()
  gui.rect(0, 0, ventana_w, BARRA_H, 0xD4D0C8)
  local x = 4
  for _, h in ipairs(HERRAMIENTAS) do
    local fondo = (herramienta == h[1]) and 0x8090B0 or 0xE8E8E8
    gui.rect(x, 3, 52, 24, fondo)
    gui.texto(x + 6, 9, h[2], 0x000000)
    x = x + 54
  end
  gui.texto(x + 8, 9, "Zoom " .. zoom .. "x   Grosor " .. grosor, 0x000000)
end

local function pintar_paleta()
  local y = ventana_h - PALETA_H - 18
  gui.rect(0, y, ventana_w, PALETA_H, 0xD4D0C8)
  -- los dos colores activos
  gui.rect(6, y + 6, 26, 26, color_a)
  gui.rect(24, y + 16, 22, 22, color_b)
  gui.rect(6, y + 6, 26, 1, 0x000000)
  local x = 60
  for _, c in ipairs(COLORES) do
    gui.rect(x, y + 8, 22, 22, c)
    x = x + 24
  end
end

-- La figura en curso, encima de la vista ya repintada
local function pintar_previa()
  if not previa then return end
  local p = previa
  previsualizando = true
  if herramienta == "linea" then linea(p.x0, p.y0, p.x1, p.y1, p.color)
  elseif herramienta == "rect" or herramienta == "rectr" then
    -- en la vista previa siempre el CONTORNO, aunque la herramienta sea
    -- la rellena: pintar el relleno serian miles de llamadas por cada
    -- movimiento del raton, y el contorno ya dice donde va a quedar
    rectangulo(p.x0, p.y0, p.x1, p.y1, p.color, false)
  elseif herramienta == "elipse" or herramienta == "elipser" then
    elipse(p.x0, p.y0, p.x1, p.y1, p.color, false)
  end
  previsualizando = false
end

local function pintar()
  pintar_barra()
  pintar_lienzo()
  pintar_previa()
  pintar_paleta()
  local info = (ruta or "(sin nombre)") .. (sucio and " *" or "") ..
               "   " .. ancho .. "x" .. alto ..
               "   [" .. herramienta .. "]  zoom " .. zoom .. "x   " .. estado
  gui.rect(0, ventana_h - 18, ventana_w, 18, 0xE8E8E8)
  gui.texto(6, ventana_h - 16, info, 0x202020)
end

-- ---- clics fuera del lienzo ----
local function clic_barra(mx)
  local x = 4
  for _, h in ipairs(HERRAMIENTAS) do
    if mx >= x and mx < x + 52 then herramienta = h[1]; sel = nil; return true end
    x = x + 54
  end
  return false
end

local function clic_paleta(mx, boton)
  local x = 60
  for _, c in ipairs(COLORES) do
    if mx >= x and mx < x + 22 then
      if boton == 2 then color_b = c else color_a = c end
      return true
    end
    x = x + 24
  end
  return false
end

-- ---- menus ----
-- OJO con la firma: crear_menu(TEXTO, tag, padre). El padre va el
-- TERCERO. Llamarla como (padre, texto) no da ningun error: el kernel
-- recibe un numero donde espera un puntero a texto, devuelve -1, y
-- simplemente no aparece ninguna barra de menus.
local m = {}
local tag = 0
local function entrada(texto, padre)
  tag = tag + 1
  return gui.crear_menu(texto, tag, padre)
end
local function menus()
  local raiz = gui.menu_raiz()
  local arc = entrada("Archivo", raiz)
  local nue = entrada("Nuevo", arc)
  m.nuevos = {}
  for _, t in ipairs(TAMANOS) do
    m.nuevos[entrada(t .. " x " .. t, nue)] = t
  end
  m.abrir   = entrada("Abrir...", arc)
  m.guardar = entrada("Guardar", arc)
  m.guardar_como = entrada("Guardar como...", arc)
  m.salir   = entrada("Salir", arc)
  local ed = entrada("Edicion", raiz)
  m.deshacer = entrada("Deshacer", ed)
  m.copiar   = entrada("Copiar", ed)
  m.pegar    = entrada("Pegar", ed)
  m.borrar   = entrada("Borrar zona", ed)
  local im = entrada("Imagen", raiz)
  local tam = entrada("Cambiar tamaño", im)
  m.tamanos = {}
  for _, t in ipairs(TAMANOS) do
    m.tamanos[entrada(t .. " x " .. t, tam)] = t
  end
  m.doble = entrada("Doble de grande", im)
  m.mitad = entrada("La mitad", im)
  m.rot90  = entrada("Rotar 90", im)
  m.rot270 = entrada("Rotar -90", im)
  m.esph   = entrada("Espejo horizontal", im)
  m.espv   = entrada("Espejo vertical", im)
  -- El GROSOR funcionaba desde que existe 'punto', pero no
  -- habia forma de cambiarlo: 'grosor' se ponia a 1 al arrancar y no se
  -- le asignaba en ningun sitio, asi que el pincel era siempre de 1 px
  -- y la linea de estado enseñaba un numero que nunca cambiaba.
  --
  -- Los valores son IMPARES a proposito. 'punto' dibuja un cuadrado de
  -- lado 2*(grosor//2)+1, asi que 2 y 3 dan los mismos 3x3, y 4 y 5 los
  -- mismos 5x5: ofrecer los pares seria ofrecer entradas que no hacen
  -- nada. Con solo impares, el numero del menu ES el lado que se pinta,
  -- y el "Grosor N" de la linea de estado dice la verdad.
  local GROSORES = {1, 3, 5, 7, 9, 13, 17}
  local pi = entrada("Pincel", raiz)
  m.grosores = {}
  for _, gr in ipairs(GROSORES) do
    m.grosores[entrada(gr .. " px", pi)] = gr
  end
  local ve = entrada("Ver", raiz)
  m.mas   = entrada("Ampliar", ve)
  m.menos = entrada("Reducir", ve)
  m.ajustar = entrada("Ajustar a la ventana", ve)
end

local function ajustar_zoom()
  local _, _, zw, zh = zona()
  local z = math.min(zw // ancho, zh // alto)
  if z < 1 then z = 1 end
  if z > 32 then z = 32 end
  zoom, vx, vy = z, 0, 0
end

-- ---- arranque ----
gui.crear_ventana("Pintor", 40, 40, ventana_w, ventana_h)
menus()

-- DOCUMENTOS/IMAGENES: donde viven los iconos del sistema. Se busca al
-- arrancar para que el dialogo de abrir empiece ahi.
do
  local docs = arch.buscar("DOCUMENTOS", arch.RAIZ)
  if docs and arch.es_carpeta_entrada(docs) then
    local ims = arch.buscar("IMAGENES", docs.inodo)
    if ims and arch.es_carpeta_entrada(ims) then dir_iconos = ims.inodo end
  end
end
nuevo(64, 64, 0xFFFFFF)
ajustar_zoom()
estado = "Listo"

-- Si viene un archivo por la linea de ordenes, se abre. El explorador
-- lanza con el formato "CARPETA:nombre" -- el numero es el inodo de la
-- carpeta, que hace falta porque cargar_imagen sin carpeta solo mira en
-- la raiz y en DOCUMENTOS. (Es el mismo formato que usa visor.lua.)
--
--   run pintor.lua tr_mina.nimg
--   run pintor.lua 7:tr_mina.nimg
do
  local a = arg and arg[1]
  if a and a ~= "" then
    -- El explorador pone "F:" delante cuando el archivo esta
    -- en la TARJETA. Ese prefijo lo generaba desde hace tiempo y NADIE lo
    -- leia: un .nimg de la tarjeta llegaba aqui como el nombre literal
    -- "F:7:dibujo.nimg" y no se encontraba. Ahora se separa y se carga del
    -- volumen que toca, con la syscall 293.
    local fat = false
    if a:sub(1, 2) == "F:" then fat = true; a = a:sub(3) end
    local c, nombre = a:match("^(%d+):(.+)$")
    if fat then
      -- en FAT la carpeta es un cluster desplazado, y 0 es la raiz
      if abrir(nombre or a, c and tonumber(c) or 0, 1) then ajustar_zoom() end
    elseif c then
      if abrir(nombre, tonumber(c)) then ajustar_zoom() end
    elseif a:find("[/\\]") then
      -- una ruta con carpetas: el kernel la resuelve, y
      -- Guardar la reutiliza tal cual para escribir donde estaba
      if abrir(a) then ajustar_zoom() end
    else
      -- sin carpeta: se prueba la raiz y luego DOCUMENTOS/IMAGENES
      if abrir(a) then ajustar_zoom()
      elseif dir_iconos and abrir(a, dir_iconos) then ajustar_zoom() end
    end
  end
end

local anterior_px, anterior_py = nil, nil

while true do
  if gui.tamano_cambiado() then
    ventana_w, ventana_h = gui.tamano_ventana()
    repintar = true
  end

  -- ---- menus ----
  -- Un menu NO llega por evento_gadget: llega como EVENT_MENUACTION
  -- (0x1001) y la entrada elegida viene en la 'fuente' del evento.
  local ev = gui.sondear_evento()
  if ev == gui.EVENT_WINDOWCLOSE then break end
  local g = 0
  if ev ~= 0 then
    local fuente = gui.info_evento()
    if ev == gui.EVENT_MENUACTION then g = fuente end
  end
  if g ~= 0 then
    repintar = true
    if g == m.salir then break
    elseif m.nuevos[g] then
      local t = m.nuevos[g]
      nuevo(t, t, 0xFFFFFF); ajustar_zoom(); estado = "Nuevo " .. t .. "x" .. t
    elseif g == m.abrir then
      -- devuelve el NOMBRE y la CARPETA donde se eligio; hay que pasar
      -- las dos, porque cargar_imagen sin carpeta solo mira en la raiz
      -- y en DOCUMENTOS
      -- empieza en DOCUMENTOS/IMAGENES, que es donde viven los iconos
      -- el dialogo dibuja encima de toda la ventana
      local nombre, carpeta = gui.dialogo_abrir(dir_iconos or 0)
      if nombre then if abrir(nombre, carpeta) then ajustar_zoom() end end
      repintar = true
    elseif g == m.guardar then
      if ruta then guardar(ruta)
      else
        local nombre, carpeta = gui.dialogo_guardar(dir_iconos or 0)
        if nombre then guardar(ruta_de(nombre, carpeta)) end
      end
    elseif g == m.guardar_como then
      local nombre, carpeta = gui.dialogo_guardar(dir_iconos or 0)
      if nombre then guardar(ruta_de(nombre, carpeta)) end
    elseif g == m.deshacer then deshacer()
    elseif g == m.copiar then copiar()
    elseif g == m.pegar then pegar_en(vx, vy)
    elseif g == m.borrar then borrar_seleccion()
    elseif m.tamanos[g] then
      local t = m.tamanos[g]; cambiar_tamano(t, t); ajustar_zoom()
    elseif g == m.doble then cambiar_tamano(ancho * 2, alto * 2); ajustar_zoom()
    elseif g == m.mitad then cambiar_tamano(ancho // 2, alto // 2); ajustar_zoom()
    elseif g == m.rot90 then rotar(90)
    elseif g == m.rot270 then rotar(270)
    elseif g == m.esph then espejo(true)
    elseif g == m.espv then espejo(false)
    elseif g == m.mas then if zoom < 32 then zoom = zoom * 2 end
    elseif g == m.menos then if zoom > 1 then zoom = zoom // 2 end
    elseif g == m.ajustar then ajustar_zoom()
    elseif m.grosores[g] then
      grosor = m.grosores[g]
      estado = "Grosor " .. grosor .. " px"
    end
  end

  -- ---- teclado ----
  local k = gui.siguiente_codigo and gui.siguiente_codigo() or 0
  if k == 1 then break end                                  -- Esc

  -- ---- rueda: zoom ----
  local r = gui.rueda()
  if r > 0 and zoom < 32 then zoom = zoom * 2; repintar = true end
  if r < 0 and zoom > 1 then zoom = zoom // 2; repintar = true end

  -- ---- raton ----
  local mx, my, botones = gui.raton()
  if mx then
    local zx, zy, zw, zh = zona()
    local dentro = my >= zy and my < zy + zh
    local pulsado = (botones or 0) ~= 0
    local boton = ((botones or 0) & 2) ~= 0 and 2 or 1
    local color = (boton == 2) and color_b or color_a

    if pulsado and not arrastrando then
      -- empieza un gesto
      arrastrando = true
      repintar = true
      if my < BARRA_H then clic_barra(mx)
      elseif my >= ventana_h - PALETA_H - 18 then clic_paleta(mx, boton)
      elseif dentro then
        local px, py = a_lienzo(mx, my)
        if px then
          ax, ay = px, py
          if herramienta == "gotero" then
            local c = leer_pixel(lienzo, px, py)
            if boton == 2 then color_b = c else color_a = c end
            estado = string.format("Color %06X", c)
          elseif herramienta == "cubo" then
            apuntar(); cubo(px, py, color)
          elseif herramienta == "sel" then
            sel = {x = px, y = py, w = 1, h = 1}
          elseif herramienta == "lapiz" or herramienta == "borrador" then
            apuntar()
            directo = true
            punto(px, py, herramienta == "borrador" and color_b or color)
            directo = false
            repintar = false          -- ya esta pintado: no hace falta mas
            anterior_px, anterior_py = px, py
          else
            apuntar()                     -- figura: se apunta al empezar
            previa = {x0 = px, y0 = py, x1 = px, y1 = py, color = color}
          end
        end
      end
    elseif pulsado and arrastrando and dentro then
      -- sigue el gesto
      repintar = true
      local px, py = a_lienzo(mx, my)
      if px then
        if herramienta == "lapiz" or herramienta == "borrador" then
          local c = herramienta == "borrador" and color_b or color
          directo = true
          if anterior_px then linea(anterior_px, anterior_py, px, py, c) else punto(px, py, c) end
          directo = false
          repintar = false          -- cada pixel ya se ha dibujado solo
          anterior_px, anterior_py = px, py
        elseif herramienta == "sel" then
          sel.w = math.abs(px - ax) + 1
          sel.h = math.abs(py - ay) + 1
          sel.x = math.min(px, ax)
          sel.y = math.min(py, ay)
        elseif previa then
          -- solo si de verdad ha cambiado: mover el raton dentro del
          -- mismo pixel del lienzo no obliga a repintar nada
          if previa.x1 ~= px or previa.y1 ~= py then
            previa.x1, previa.y1 = px, py
            repintar = true
          end
        end
      end
    elseif not pulsado and arrastrando then
      -- termina el gesto: las figuras se pintan AQUI, una sola vez
      arrastrando = false
      repintar = true
      anterior_px, anterior_py = nil, nil
      if previa then
        local p = previa
        if herramienta == "linea" then linea(p.x0, p.y0, p.x1, p.y1, p.color)
        elseif herramienta == "rect" then rectangulo(p.x0, p.y0, p.x1, p.y1, p.color, false)
        elseif herramienta == "rectr" then rectangulo(p.x0, p.y0, p.x1, p.y1, p.color, true)
        elseif herramienta == "elipse" then elipse(p.x0, p.y0, p.x1, p.y1, p.color, false)
        elseif herramienta == "elipser" then elipse(p.x0, p.y0, p.x1, p.y1, p.color, true)
        end
        previa = nil
      end
      sucio = true
    end
  end

  if repintar then
    pintar()
    repintar = false
  end
  nemo.pump()
end

if vista then gui.liberar_imagen(vista) end
if porta then gui.liberar_imagen(porta) end
vaciar_pila()
if lienzo then gui.liberar_imagen(lienzo) end
