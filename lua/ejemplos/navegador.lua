-- navegador.lua -- Nemo OS
--
-- El navegador web. Se apoya en las mismas dos piezas que el visor de
-- documentos: nemo_html.lua para analizar y maquetar, y nemo_gui para
-- pintar. Lo que anade es todo lo que hace que sea un navegador y no un
-- visor: una barra con direccion y botones, historial hacia delante y
-- hacia atras, y --lo importante-- la red.
--
-- COMO LLEGA UNA PAGINA HASTA AQUI. Nemo OS no habla HTTPS ni sabe
-- descomprimir un JPEG ni tiene motor de CSS ni de JavaScript. Nada de
-- eso hace falta, porque quien baja la pagina de internet es un ayudante
-- que corre en otro ordenador de la red (herramientas/proxy):
--
--   1. Este programa le pide al proxy  /nmz?u=<direccion>
--   2. El proxy baja la pagina de verdad, le quita todo lo que aqui no
--      se sabe leer, convierte sus imagenes a .nimg ya escaladas, y
--      devuelve UN archivo con todo dentro (formato .nmz).
--   3. El kernel lo descarga (SYS_NET_BAJAR) y lo desempaqueta en una
--      carpeta (SYS_NMZ_ABRIR).
--   4. Aqui se abre el index.html resultante, que es HTML sencillo con
--      sus imagenes al lado -- exactamente lo que el visor lleva meses
--      sabiendo pintar.
--
-- Asi que el trabajo duro no esta en este archivo. Esta en haber puesto
-- la frontera en el sitio correcto.
--
-- Uso:  run navegador.lua                  abre la pagina de inicio
--       run navegador.lua es.wikipedia.org  abre esa
--
-- Controles:
--   escribir en la barra + Enter    ir a esa direccion
--   rueda, flechas arriba/abajo     desplazar
--   clic en un enlace               seguirlo
--   Retroceso                       atras (con el foco fuera de la barra)
--   Esc                             cerrar
--
-- La direccion del proxy se guarda en PROXY.CFG, en la raiz. Para
-- cambiarla, escribir en la barra:   proxy 192.168.1.40:8080

local gui = require("nemo_gui")
local archivos = require("nemo_archivos")
local html = require("nemo_html")
local web = require("nemo_web")   -- direcciones, enlaces e historial (probado aparte)

-- Solo los que se usan de verdad. Tener numeros de mas invita a
-- equivocarse: aqui habia un FREE_IMAGE = 94 que en realidad es el 88, y
-- eso no da error -- libera otra cosa, o nada.
local S = {
  GET_MOUSE = 34, GET_MOUSE_WHEEL = 45, MOUSE_HIT = 56,
  LOAD_IMAGE_EN = 253,
  NET_BAJAR = 296, NET_ESTADO = 297, NET_BYTES = 298, NET_MOTIVO = 299,
  NMZ_ABRIR = 300,
}

-- Estados que devuelve SYS_NET_ESTADO
local NET_PARADA, NET_EN_MARCHA, NET_LISTA, NET_FALLO = 0, 1, 2, 3

-- ---------------------------------------------------------------
-- Aspecto
--
-- Los colores salen de mirar Safari y Chrome: barra gris muy claro,
-- campo de direccion blanco con borde fino, y una linea de separacion
-- que es lo que hace que la barra parezca una barra y no un hueco.
-- ---------------------------------------------------------------
local BARRA_ALTO   = 44
local C_BARRA      = 0xF2F2F2
local C_SEPARADOR  = 0xD0D0D0
local C_CAMPO      = 0xFFFFFF
local C_CAMPO_BORDE= 0xC8C8C8
local C_CAMPO_FOCO = 0x4A90D9
local C_TEXTO      = 0x202020
local C_APAGADO    = 0x9A9A9A
local C_ICONO      = 0x404040
local C_PROGRESO   = 0x4A90D9

local BOTON_ANCHO  = 30
local MARGEN       = 8

-- ---------------------------------------------------------------
-- Estado
-- ---------------------------------------------------------------
local ventana_w, ventana_h = 900, 620
local documento, arbol = nil, nil
local scroll = 0
local zoom = 1

local barra_texto = ""          -- lo que se ve escrito en la barra
local barra_foco  = false
local url_actual  = nil

local historial = web.historial()
-- Un aviso a ensenar en vez de la pagina. 'mensaje_malo' distingue un
-- ERROR de una simple confirmacion: pintarlo todo de rojo hace que una
-- respuesta normal ("proxy cambiado") parezca que algo ha fallado.
local mensaje   = nil
local mensaje_malo = false

local proxy_ip, proxy_puerto = "10.0.2.2", 8080
local CARPETA_WEB = "WEB"       -- donde se desempaqueta lo que se baja
local carpeta_web = nil         -- su inodo

local bajando = false
local parpadeo = 0

-- ---------------------------------------------------------------
-- La direccion del proxy
-- ---------------------------------------------------------------
local function guardar_proxy()
  archivos.escribir_en("PROXY.CFG", proxy_ip .. ":" .. proxy_puerto,
                       archivos.RAIZ, archivos.VOL_NEMOFS)
end

local function cargar_proxy()
  local t = archivos.leer_en("PROXY.CFG", archivos.RAIZ, archivos.VOL_NEMOFS, 64)
  if not t then return end
  local ip, puerto = web.parsear_proxy(t)
  if ip then proxy_ip, proxy_puerto = ip, puerto end
end

-- ---------------------------------------------------------------
-- Fuentes e imagenes (igual que en el visor: se piden al kernel una
-- vez y se guardan, porque cada cambio de fuente es un syscall)
-- ---------------------------------------------------------------
-- Un tope: cada fuente cargada ocupa sitio en el kernel, y una pagina
-- con muchos tamanos distintos las agotaria. Cuando se llena se tira la
-- menos usada, como hace el visor.
local MAX_FUENTES = 24
local SIN_FUENTE = { handle = nil, avances = nil }

local fuentes, fallidas = {}, {}
local contador_uso = 0

local function clave_de(e)
  return string.format("%s|%d|%s|%s",
                       e.mono and "mono" or e.serif and "serif" or "sans",
                       e.tamano or 12,
                       e.negrita and "n" or "-", e.cursiva and "c" or "-")
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
    local h = gui.cargar_fuente(familia, math.floor((e.tamano or 12) * zoom + 0.5),
                                e.negrita, e.cursiva, false)
    if not h then fallidas[k] = true; return SIN_FUENTE end
    f = { handle = h, avances = gui.avances_fuente(h) }
    fuentes[k] = f
  end
  contador_uso = contador_uso + 1; f.uso = contador_uso
  return f
end

-- Metrica para el motor de maquetacion. OJO con el orden de los
-- argumentos de medir_con_avances: es (avances, texto), no al reves.
-- Cambiarlos no da un texto mal medido -- tumba el programa, porque
-- intenta recorrer la tabla de anchuras como si fuera una cadena.
local function metrica(e)
  local f = fuente(e)
  if not f.handle or not f.avances then return html.metrica_fija(e) end
  local av = f.avances
  return {
    ancho = function(t) return gui.medir_con_avances(av, t) end,
    alto_linea = av.alto_linea,
    ascent = av.ascent,
  }
end

local function liberar_fuentes()
  for _, f in pairs(fuentes) do if f.handle then gui.liberar_fuente(f.handle) end end
  fuentes, fallidas = {}, {}
end

local imagenes = {}

local function imagen(src)
  local i = imagenes[src]
  if i ~= nil then return i or nil end
  if not carpeta_web then imagenes[src] = false; return nil end
  local h = nemo.syscall(S.LOAD_IMAGE_EN, src, carpeta_web, 0, 0, 0)
  if h <= 0 then imagenes[src] = false; return nil end
  local w, hh = gui.tamano_imagen(h)
  imagenes[src] = { handle = h, w = w, h = hh }
  return imagenes[src]
end

local function liberar_imagenes()
  for _, i in pairs(imagenes) do
    if i and i.handle then gui.liberar_imagen(i.handle) end
  end
  imagenes = {}
end

-- ---------------------------------------------------------------
-- Maquetar y pintar
-- ---------------------------------------------------------------
local function alto_pagina() return ventana_h - BARRA_ALTO end

local function maquetar()
  if not arbol then documento = nil; return end
  fallidas = {}
  documento = html.maquetar(arbol, {
    ancho = ventana_w - 10,
    zoom = zoom,
    metrica = metrica,
    tamano_imagen = function(src) local i = imagen(src); if i then return i.w, i.h end end,
  })
  local max_scroll = math.max(0, documento.alto - alto_pagina())
  if scroll > max_scroll then scroll = max_scroll end
  if scroll < 0 then scroll = 0 end
  -- Solo el titulo de la pagina, como Chrome y Safari. El gestor de
  -- ventanas corta a 23 caracteres (wm.c), asi que gastar seis en
  -- " - Navegador" seria tirar un cuarto del sitio para decir algo que
  -- la ventana ya dice ella sola.
  gui.titulo(documento.titulo or "Navegador")
end

-- Una flecha dibujada con lineas. Sale mejor que un caracter: las
-- fuentes del sistema no tienen por que traer flechas, y un
-- interrogante en un boton queda fatal.
--
-- 'hacia' es -1 para la izquierda y 1 para la derecha, y la PUNTA va en
-- ese extremo. La primera version la ponia en el centro, y el resultado
-- eran dos botones que apuntaban justo al reves de lo que hacian --
-- algo que en el codigo no se ve y en la pantalla canta a la primera.
local function flecha(cx, cy, hacia, color)
  -- La punta en cx + hacia*5, ensanchando hacia el centro.
  for i = 0, 5 do
    local x = cx + hacia * (5 - i)
    gui.linea(x, cy - i, x, cy + i, color)
  end
  -- El astil, saliendo de la base hacia el otro lado.
  gui.linea(cx, cy, cx - hacia * 4, cy, color)
  gui.linea(cx, cy + 1, cx - hacia * 4, cy + 1, color)
end

local function circulo_recarga(cx, cy, color)
  -- Un aro abierto con una punta de flecha, como el de recargar de toda
  -- la vida. No hay primitiva de circulo, asi que se pone punto a punto:
  -- con paso de 12 grados el aro sale continuo a este tamano.
  local r = 7
  for a = -60, 260, 12 do
    local rad = a * math.pi / 180
    local x = cx + math.floor(r * math.cos(rad) + 0.5)
    local y = cy + math.floor(r * math.sin(rad) + 0.5)
    gui.rect(x, y, 2, 2, color)
  end
  -- La punta, en el extremo abierto de arriba a la derecha.
  local px, py = cx + math.floor(r * 0.5 + 0.5), cy - math.floor(r * 0.87 + 0.5)
  gui.linea(px, py, px - 4, py - 1, color)
  gui.linea(px, py, px + 1, py + 4, color)
end

-- Un rectangulo con las esquinas comidas. No hay primitiva de esquina
-- redonda; quitar cuatro pixeles del color del fondo basta para que el
-- campo de direccion deje de parecer una caja de 1990.
local function rect_redondeado(x, y, w, h, color, fondo)
  gui.rect(x, y, w, h, color)
  for i = 0, 1 do
    gui.rect(x + i,         y + 1 - i,     1, 1, fondo)
    gui.rect(x + w - 1 - i, y + 1 - i,     1, 1, fondo)
    gui.rect(x + i,         y + h - 2 + i, 1, 1, fondo)
    gui.rect(x + w - 1 - i, y + h - 2 + i, 1, 1, fondo)
  end
end

local function pintar_barra()
  gui.rect(0, 0, ventana_w, BARRA_ALTO, C_BARRA)
  gui.linea(0, BARRA_ALTO - 1, ventana_w, BARRA_ALTO - 1, C_SEPARADOR)

  local cy = BARRA_ALTO // 2
  local x = MARGEN

  -- atras / adelante: apagados cuando no hay a donde ir, que es la
  -- forma de decirlo sin escribir nada
  flecha(x + BOTON_ANCHO // 2, cy, -1, historial:puede_atras() and C_ICONO or C_APAGADO)
  x = x + BOTON_ANCHO
  flecha(x + BOTON_ANCHO // 2, cy, 1, historial:puede_adelante() and C_ICONO or C_APAGADO)
  x = x + BOTON_ANCHO
  circulo_recarga(x + BOTON_ANCHO // 2, cy, url_actual and C_ICONO or C_APAGADO)
  x = x + BOTON_ANCHO + MARGEN

  -- campo de direccion
  local cw = ventana_w - x - MARGEN
  local ch = BARRA_ALTO - 16
  local cyy = (BARRA_ALTO - ch) // 2
  local borde = barra_foco and C_CAMPO_FOCO or C_CAMPO_BORDE
  rect_redondeado(x - 1, cyy - 1, cw + 2, ch + 2, borde, C_BARRA)
  rect_redondeado(x, cyy, cw, ch, C_CAMPO, borde)

  gui.poner_fuente(fuente({ tamano = 13 }).handle or 0)
  local t = barra_texto
  if t == "" and not barra_foco then t = "escribe una direccion" end
  local color = (barra_texto == "" and not barra_foco) and C_APAGADO or C_TEXTO
  -- Si el texto no cabe se ensena el FINAL, no el principio: lo que
  -- estas escribiendo esta al final, y ver el principio mientras
  -- escribes a ciegas es inutil.
  local ancho_t = gui.medir_texto(t)
  local tx = x + 8
  if ancho_t > cw - 16 then tx = x + 8 - (ancho_t - (cw - 16)) end
  gui.texto(tx, cyy + (ch - 14) // 2, t, color)
  if barra_foco and (parpadeo // 15) % 2 == 0 then
    local cx = tx + ancho_t + 1
    if cx < x + cw - 3 then gui.linea(cx, cyy + 4, cx, cyy + ch - 4, C_TEXTO) end
  end
  gui.poner_fuente(0)

  -- barra de progreso, pegada a la linea de separacion, como en Safari
  if bajando then
    local recibido, total = nemo.syscall(S.NET_BYTES), 0
    total = recibido >> 32
    recibido = recibido & 0xFFFFFFFF
    local frac = (total > 0) and (recibido / total) or 0.15
    if frac > 1 then frac = 1 end
    gui.rect(0, BARRA_ALTO - 3, math.floor(ventana_w * frac), 2, C_PROGRESO)
  end
end

local function pintar_pagina()
  local fondo = documento and documento.fondo or 0xFFFFFF
  gui.rect(0, BARRA_ALTO, ventana_w, alto_pagina(), fondo)

  if mensaje then
    gui.poner_fuente(fuente({ tamano = 14, negrita = mensaje_malo }).handle or 0)
    gui.texto(16, BARRA_ALTO + 24, mensaje, mensaje_malo and 0xCC0000 or 0x606060)
    gui.poner_fuente(0)
    return
  end
  if not documento then return end

  local fuente_actual = nil
  for _, o in ipairs(documento.ops) do
    local y = o.y - scroll + BARRA_ALTO
    local h = o.h or (o.tamano and math.floor(o.tamano * 1.3)) or 0
    -- Recortado a la zona de pagina: sin esto, una linea de texto medio
    -- desplazada se dibujaria ENCIMA de la barra de direcciones.
    if y + h >= BARRA_ALTO and y < ventana_h then
      if o.tipo == "rect" then
        gui.rect(o.x, y, o.w, o.h, o.color)
      elseif o.tipo == "texto" then
        local f = fuente(o).handle or 0
        if f ~= fuente_actual then gui.poner_fuente(f); fuente_actual = f end
        gui.texto(o.x, y, o.texto, o.color)
      elseif o.tipo == "imagen" then
        local i = imagen(o.src)
        if i then gui.dibujar_imagen(i.handle, o.x, y) end
      end
    end
  end
  if fuente_actual then gui.poner_fuente(0) end
end

local function redibujar()
  pintar_pagina()
  pintar_barra()
end

local function desplazar(d)
  if not documento then return end
  local max_scroll = math.max(0, documento.alto - alto_pagina())
  local antes = scroll
  scroll = math.max(0, math.min(max_scroll, scroll + d))
  if scroll ~= antes then redibujar() end
end

-- ---------------------------------------------------------------
-- Navegar
-- ---------------------------------------------------------------

local function abrir_descargado()
  bajando = false
  liberar_imagenes()

  local buf = nemo.buffer(64)
  local n = nemo.syscall(S.NMZ_ABRIR, "PAGINA.NMZ", CARPETA_WEB,
                         nemo.direccion(buf), 64)
  if n < 0 then
    mensaje = "El paquete llego mal y no se pudo abrir."; mensaje_malo = true
    arbol, documento = nil, nil
    redibujar()
    return
  end
  local primero = nemo.cadena(buf)

  local d = archivos.buscar(CARPETA_WEB, archivos.RAIZ, archivos.VOL_NEMOFS)
  carpeta_web = d and d.inodo or nil
  if not carpeta_web then
    mensaje = "No se encuentra la carpeta " .. CARPETA_WEB; mensaje_malo = true
    redibujar()
    return
  end

  local texto = archivos.leer_en(primero, carpeta_web, archivos.VOL_NEMOFS, 512 * 1024)
  if not texto then
    mensaje = "No se pudo leer " .. primero; mensaje_malo = true
    redibujar()
    return
  end

  mensaje = nil
  arbol = html.parsear(texto)
  scroll = 0
  maquetar()
  redibujar()
end

local function navegar(url, guardar)
  if url == "" then return end

  -- "proxy 192.168.1.40:8080" cambia el ayudante y no navega. Va aqui y
  -- no en un menu de opciones porque es lo primero que hay que hacer en
  -- una instalacion nueva, y buscarlo en un menu seria una perdida de
  -- tiempo para algo que se toca una vez.
  local ip, puerto = web.orden_proxy(url)
  if ip then
    proxy_ip, proxy_puerto = ip, puerto
    guardar_proxy()
    mensaje = "Proxy: " .. proxy_ip .. ":" .. proxy_puerto .. "   (guardado en PROXY.CFG)"
    mensaje_malo = false
    barra_texto = ""
    arbol, documento = nil, nil
    redibujar()
    return
  end

  local ipn = web.ip_empaquetada(proxy_ip)
  if not ipn then
    mensaje = "La direccion del proxy no vale: " .. proxy_ip; mensaje_malo = true
    redibujar()
    return
  end

  if nemo.syscall(S.NET_BAJAR, web.ruta_para(url), ipn, proxy_puerto, "PAGINA.NMZ") ~= 0 then
    mensaje = "No se pudo empezar la descarga (hay otra en marcha?)"; mensaje_malo = true
    redibujar()
    return
  end

  url_actual = url
  barra_texto = url
  bajando = true
  mensaje = nil

  if guardar ~= false then historial:ir(url) end
  redibujar()
end

local function atras()
  local u = historial:atras()
  if u then navegar(u, false) end
end

local function adelante()
  local u = historial:adelante()
  if u then navegar(u, false) end
end

-- Un enlace de una pagina del proxy viene reescrito como
-- "/nmz?u=<direccion>". Se saca la direccion de dentro y se navega a
-- ella -- asi el historial guarda direcciones de verdad y no rutas del
-- proxy, que es lo que hay que ensenar en la barra.
local function seguir_enlace(href)
  local u = web.url_de_enlace(href)
  if not u then return false end
  navegar(u, true)
  return true
end

-- ---------------------------------------------------------------
-- Teclado en la barra de direcciones
-- ---------------------------------------------------------------
local function tecla_en_barra(t)
  if t == gui.TECLA_ENTER then
    barra_foco = false
    navegar(barra_texto, true)
    return true
  elseif t == gui.TECLA_BACKSPACE then
    barra_texto = barra_texto:sub(1, -2)
    return true
  elseif t == gui.TECLA_ESC then
    barra_foco = false
    barra_texto = url_actual or ""
    return true
  elseif t >= 32 and t < 127 then
    barra_texto = barra_texto .. string.char(t)
    return true
  end
  return false
end

-- ---------------------------------------------------------------
-- Arranque
-- ---------------------------------------------------------------
gui.crear_ventana("Navegador", 40, 30, ventana_w, ventana_h)
ventana_w, ventana_h = gui.tamano_ventana()
cargar_proxy()

-- La carpeta donde se desempaquetan las paginas. Se crea una vez.
do
  local d = archivos.buscar(CARPETA_WEB, archivos.RAIZ, archivos.VOL_NEMOFS)
  if not d then archivos.crear_carpeta(CARPETA_WEB, archivos.RAIZ, archivos.VOL_NEMOFS) end
end

if arg and arg[1] then
  navegar(arg[1], true)
else
  mensaje = "Escribe una direccion arriba y pulsa Enter."
  barra_foco = true
end
redibujar()

-- ---------------------------------------------------------------
-- Bucle
-- ---------------------------------------------------------------
gui.bucle(function(evento)
  if evento == gui.EVENT_WINDOWSIZE then
    ventana_w, ventana_h = gui.tamano_ventana()
    maquetar(); redibujar()
    return true
  end

  -- La descarga avanza en el kernel; aqui solo se mira como va. Cuando
  -- termina, se desempaqueta y se pinta.
  if bajando then
    local e = nemo.syscall(S.NET_ESTADO) & 0xFF
    if e == NET_LISTA then
      abrir_descargado()
    elseif e == NET_FALLO then
      bajando = false
      local buf = nemo.buffer(128)
      nemo.syscall(S.NET_MOTIVO, nemo.direccion(buf), 128)
      mensaje = "No se pudo cargar: " .. nemo.cadena(buf); mensaje_malo = true
      arbol, documento = nil, nil
      redibujar()
    else
      -- repintar la barra para que la de progreso avance
      parpadeo = parpadeo + 1
      pintar_barra()
    end
  elseif barra_foco then
    parpadeo = parpadeo + 1
    if parpadeo % 15 == 0 then pintar_barra() end
  end

  local rueda = nemo.syscall(S.GET_MOUSE_WHEEL)
  if rueda ~= 0 then desplazar(-rueda * 3 * 18 * zoom) end

  if nemo.syscall(S.MOUSE_HIT, 1) == 1 then
    local m = nemo.syscall(S.GET_MOUSE)
    if m ~= -1 then
      local mx, my = (m >> 32) & 0xFFFF, (m >> 16) & 0xFFFF
      if my < BARRA_ALTO then
        local x = MARGEN
        if mx >= x and mx < x + BOTON_ANCHO then atras()
        elseif mx >= x + BOTON_ANCHO and mx < x + 2 * BOTON_ANCHO then adelante()
        elseif mx >= x + 2 * BOTON_ANCHO and mx < x + 3 * BOTON_ANCHO then
          if url_actual then navegar(url_actual, false) end
        else
          barra_foco = true
          redibujar()
        end
      else
        if barra_foco then barra_foco = false; redibujar() end
        if documento then
          local href = html.enlace_en(documento, mx, my - BARRA_ALTO + scroll)
          if href and not seguir_enlace(href) then
            mensaje = "Enlace que no se puede seguir: " .. href; mensaje_malo = true
            redibujar()
          end
        end
      end
    end
  end

  local tecla = gui.leer_tecla()
  if tecla ~= 0 then
    if barra_foco then
      if tecla_en_barra(tecla) then redibujar() end
    elseif tecla == 0x11 then desplazar(-18 * 3 * zoom)
    elseif tecla == 0x12 then desplazar(18 * 3 * zoom)
    elseif tecla == gui.TECLA_BACKSPACE then atras()
    elseif tecla == 43 or tecla == 61 then
      if zoom < 3 then zoom = zoom + 1; liberar_fuentes(); maquetar(); redibujar() end
    elseif tecla == 45 then
      if zoom > 1 then zoom = zoom - 1; liberar_fuentes(); maquetar(); redibujar() end
    elseif tecla == gui.TECLA_ESC then
      return false
    elseif tecla >= 32 and tecla < 127 then
      -- empezar a escribir una direccion sin tener que pinchar antes
      barra_foco = true
      barra_texto = string.char(tecla)
      redibujar()
    end
  end
  return true
end)

liberar_imagenes()
liberar_fuentes()
