-- nemo_gui.lua -- envoltorios comodos sobre nemo.syscall para ventanas,
-- gadgets y eventos. Todos los numeros y ordenes de argumentos salen
-- de src/syscall.h -- si dudas, ese archivo manda.
local M = {}
local S = { CREATE_WINDOW=40, POLL_EVENT=8, GET_EVENT_INFO=9,
            CREATE_BUTTON=100, CREATE_PANEL=101, CREATE_TEXTFIELD=102, CREATE_LISTBOX=103,
            GADGET_FREE=104, GADGET_SET_TEXT=105, GADGET_GET_TEXT=106, GADGET_EVENT=113,
            LISTBOX_ADD_ITEM=114, LISTBOX_CLEAR=115, LISTBOX_SELECTED=116, LISTBOX_ITEM_COUNT=118,
            CREATE_LABEL=160, SET_TITLE=130, CREATE_TIMER=127,
            DRAW_RECT=30, DRAW_TEXT=31, GET_WINDOW_SIZE=33 }
M.EVENT_GADGETACTION = 0x401
M.EVENT_WINDOWSIZE   = 0x802
M.EVENT_WINDOWCLOSE  = 0x803
M.EVENT_MENUACTION   = 0x1001
M.EVENT_TIMERTICK    = 0x4001

local function wh(ancho, alto) return (ancho << 16) | alto end

-- La ventana: cada tarea tiene una; CreateWindow la personaliza y la
-- pone en "modo evento" (la X ya no cierra sola, dispara EVENT_WINDOWCLOSE)
-- 'opciones' (opcional): { maximizar = false, minimizar = false, cerrar = false }
-- quita esos botones de la barra de titulo (un reloj no tiene nada que
-- hacer maximizado; un juego a pantalla completa puede quitarlos todos).
-- Quien quite cerrar debe ofrecer su propia salida (Esc, por ejemplo).
function M.crear_ventana(titulo, x, y, ancho, alto, opciones)
  local r = nemo.syscall(S.CREATE_WINDOW, titulo, x, y, ancho, alto)
  if opciones then M.botones_ventana(opciones.maximizar ~= false, opciones.minimizar ~= false, opciones.cerrar ~= false) end
  return r
end

-- Que botones tiene la barra de titulo (true = se ve). Sin argumento: se ve.
S.WINDOW_BOTONES = 271
function M.botones_ventana(maximizar, minimizar, cerrar)
  local function b(v) return (v == false) and 0 or 1 end
  return nemo.syscall(S.WINDOW_BOTONES, b(maximizar), b(minimizar), b(cerrar))
end
function M.titulo(texto) nemo.syscall(S.SET_TITLE, texto) end
function M.tamano_ventana()
  local v = nemo.syscall(S.GET_WINDOW_SIZE); return v >> 32, v & 0xFFFFFFFF
end

-- Gadgets: la ventana se deduce de la tarea, NO se pasa. Ancho/alto van
-- empaquetados en un solo argumento.
function M.crear_boton(texto, x, y, ancho, alto, estilo)
  return nemo.syscall(S.CREATE_BUTTON, texto, x, y, wh(ancho, alto), estilo or 0)
end
function M.crear_etiqueta(texto, x, y, ancho, alto)
  return nemo.syscall(S.CREATE_LABEL, texto, x, y, wh(ancho, alto), 0)
end
function M.crear_campo_texto(x, y, ancho, alto)  return nemo.syscall(S.CREATE_TEXTFIELD, x, y, wh(ancho, alto)) end
function M.crear_lista(x, y, ancho, alto)        return nemo.syscall(S.CREATE_LISTBOX, x, y, wh(ancho, alto)) end
function M.crear_panel(x, y, ancho, alto)        return nemo.syscall(S.CREATE_PANEL, x, y, wh(ancho, alto)) end
function M.liberar_gadget(id)                    nemo.syscall(S.GADGET_FREE, id) end
function M.poner_texto(id, texto)                nemo.syscall(S.GADGET_SET_TEXT, id, texto) end
function M.leer_texto(id)
  local buf = nemo.buffer(256)
  nemo.syscall(S.GADGET_GET_TEXT, id, nemo.direccion(buf), 256)
  return nemo.cadena(buf)
end
function M.lista_anadir(id, texto)  nemo.syscall(S.LISTBOX_ADD_ITEM, id, texto) end
function M.lista_limpiar(id)        nemo.syscall(S.LISTBOX_CLEAR, id) end
function M.lista_seleccionado(id)   return nemo.syscall(S.LISTBOX_SELECTED, id) end   -- -1 si ninguno
function M.lista_cantidad(id)       return nemo.syscall(S.LISTBOX_ITEM_COUNT, id) end
function M.crear_temporizador(hz)   return nemo.syscall(S.CREATE_TIMER, hz) end

-- Avance de cursor por caracter de la fuente ACTIVA (distinto de
-- SYS_FONT_WIDTH, que da solo el ancho del glifo, no el hueco real
-- entre caracteres -- confirmado en la Parte V de la guia de syscalls).
S.FONT_CHAR_ADVANCE = 206
function M.avance_fuente() return nemo.syscall(S.FONT_CHAR_ADVANCE) end

-- ---------------------------------------------------------------
-- Fuentes proporcionales (fonts.c en el kernel; paquete FUENTES.NFP)
-- familia: "sans", "serif", "mono" (o "sistema" = la 5x7 de siempre).
-- alto en pixeles; se usa la cara mas cercana que exista. Devuelve un
-- handle para poner_fuente(). Hay 16 huecos: liberar los que no se usen.
-- ---------------------------------------------------------------
S.LOAD_FONT = 189; S.FREE_FONT = 190; S.SET_FONT = 191; S.FONT_WIDTH = 195; S.FONT_HEIGHT = 196
S.TEXT_WIDTH = 254; S.FONT_ADVANCES = 255
function M.cargar_fuente(familia, alto, negrita, cursiva, subrayado)
  local h = nemo.syscall(S.LOAD_FONT, familia or "sans", alto or 14, negrita and 1 or 0, cursiva and 1 or 0, subrayado and 1 or 0)
  return (h and h > 0) and h or nil
end
function M.liberar_fuente(handle) if handle then nemo.syscall(S.FREE_FONT, handle) end end
function M.poner_fuente(handle) nemo.syscall(S.SET_FONT, handle or 0) end   -- 0 = la de sistema
function M.medir_texto(texto) return nemo.syscall(S.TEXT_WIDTH, texto) end  -- con la fuente activa
function M.alto_fuente() return nemo.syscall(S.FONT_HEIGHT) end             -- alto de linea de la activa

-- Activar una fuente del sistema: se carga la primera vez y se recuerda
--. "sans" para interfaz y texto corrido; "mono" para lo que
-- se edita o se alinea en columnas -- la mono de 10 avanza 6 px por
-- caracter, como la 5x7 de siempre. Sin esto, gui.texto escribe con la
-- 5x7, en mayusculas. Devuelve el handle (o nil si no se pudo cargar).
local fuentes_usadas = {}
function M.usar_fuente(familia, alto)
  local clave = familia .. ":" .. alto
  if fuentes_usadas[clave] == nil then fuentes_usadas[clave] = M.cargar_fuente(familia, alto) or false end
  M.poner_fuente(fuentes_usadas[clave] or 0)
  return fuentes_usadas[clave] or nil
end

-- Avances de una fuente: tabla t[codepoint] = pixeles para 0..255, mas
-- t.alto_linea y t.ascent. Una sola syscall; despues medir es sumar.
function M.avances_fuente(handle)
  local buf = nemo.buffer(256)
  local v = nemo.syscall(S.FONT_ADVANCES, handle, nemo.direccion(buf))
  local t = { alto_linea = v & 0xFFFF, ascent = (v >> 16) & 0xFFFF }
  for i = 0, 63 do
    local w = nemo.leer_i32(buf, i * 4)
    t[i*4] = w & 0xFF; t[i*4+1] = (w >> 8) & 0xFF; t[i*4+2] = (w >> 16) & 0xFF; t[i*4+3] = (w >> 24) & 0xFF
  end
  return t
end

-- Ancho de una cadena UTF-8 con una tabla de avances (sin syscalls).
-- Codepoints fuera de 0..255 (p.ej. el euro) se estiman con el de '?'.
function M.medir_con_avances(t, s)
  local w, i, n = 0, 1, #s
  while i <= n do
    local c = s:byte(i)
    local cp
    if c < 0x80 then cp = c; i = i + 1
    elseif c >= 0xC0 and c < 0xE0 and i + 1 <= n then cp = ((c & 0x1F) << 6) | (s:byte(i+1) & 0x3F); i = i + 2
    elseif c >= 0xE0 and c < 0xF0 and i + 2 <= n then cp = ((c & 0x0F) << 12) | ((s:byte(i+1) & 0x3F) << 6) | (s:byte(i+2) & 0x3F); i = i + 3
    elseif c >= 0xF0 and i + 3 <= n then cp = 63; i = i + 4
    else cp = c; i = i + 1 end   -- byte suelto: Latin-1
    w = w + (t[cp] or t[63] or 0)
  end
  return w
end

-- Menu de verdad (barra superior, MENUBAR_H=16 px -- se resta sola de
-- las coordenadas de dibujo en cuanto existe al menos una entrada de
-- primer nivel, confirmado en gadgets_menubar_height()).
M.MENUBAR_H = 16
S.WINDOW_MENU = 120
S.CREATE_MENU = 121
S.MENU_GET_TAG = 124
function M.menu_raiz() return nemo.syscall(S.WINDOW_MENU) end
function M.crear_menu(texto, tag, padre) return nemo.syscall(S.CREATE_MENU, texto, tag, padre) end
function M.menu_tag(id) return nemo.syscall(S.MENU_GET_TAG, id) end
function M.marcar_menu(id, si) nemo.syscall(122, id, (si == false) and 0 or 1) end      -- CheckMenu / UncheckMenu
function M.activar_menu(id, si) nemo.syscall(123, id, (si == false) and 0 or 1) end     -- EnableMenu / DisableMenu
-- Una entrada con texto "" es una linea de separacion.

-- ---- Todos los controles del kernel ----
-- Lo mismo que ofrece Nemo Basic, con nombres en español. Los ids que
-- devuelven valen para poner_texto, lista_anadir, ocultar, etc.
local function b01(v) return (v == false) and 0 or 1 end
function M.crear_casilla(texto, x, y, ancho, alto) return nemo.syscall(100, texto, x, y, wh(ancho, alto), 2) end
function M.crear_opcion(texto, x, y, ancho, alto)  return nemo.syscall(100, texto, x, y, wh(ancho, alto), 3) end   -- al marcar una, se desmarcan las demas
function M.marcada(id) return nemo.syscall(141, id) == 1 end                                 -- casilla u opcion
function M.marcar(id, si) nemo.syscall(142, id, b01(si)) end
-- Deslizador: de 0 a maximo (por defecto 100); 'vertical' opcional
function M.crear_deslizador(x, y, ancho, alto, maximo, vertical)
  local id = nemo.syscall(163, x, y, wh(ancho, alto), vertical and 2 or 1)
  nemo.syscall(164, id, 1, (maximo or 100) + 1)                -- el kernel da de 0 a total - visible
  return id
end
function M.valor_deslizador(id) return nemo.syscall(166, id) end
function M.poner_deslizador(id, v) nemo.syscall(165, id, v) end
-- Barra de progreso: fraccion de 0.0 a 1.0
function M.crear_progreso(x, y, ancho, alto) return nemo.syscall(161, x, y, wh(ancho, alto)) end
function M.poner_progreso(id, fraccion) nemo.syscall(162, id, math.floor((fraccion or 0) * 1000 + 0.5)) end   -- el kernel cuenta por mil
function M.crear_desplegable(x, y, ancho, alto) return nemo.syscall(167, x, y, wh(ancho, alto)) end   -- sus lineas: lista_anadir
function M.crear_pestanas(x, y, ancho, alto) return nemo.syscall(168, x, y, wh(ancho, alto)) end      -- sus nombres: lista_anadir
function M.crear_caja_texto(x, y, ancho, alto) return nemo.syscall(125, x, y, wh(ancho, alto)) end
function M.poner_caja_texto(id, texto) nemo.syscall(126, id, texto) end
function M.anadir_caja_texto(id, texto) nemo.syscall(145, id, texto) end
function M.lista_insertar(id, n, texto) nemo.syscall(157, id, n, texto) end
function M.lista_cambiar(id, n, texto) nemo.syscall(159, id, n, texto) end
function M.lista_quitar(id, n) nemo.syscall(158, id, n) end
function M.mostrar(id, si) nemo.syscall(110, id, b01(si)) end        -- mostrar(id, false) lo oculta
function M.activar(id, si) nemo.syscall(111, id, b01(si)) end        -- activar(id, false) lo desactiva
function M.dar_foco(id) nemo.syscall(112, id) end
function M.color_panel(id, r, g, b) nemo.syscall(207, id, (r << 16) | (g << 8) | b) end
function M.imagen_panel(id, archivo) nemo.syscall(222, id, archivo) end   -- NIMG, en la raiz, en DOCUMENTOS o en DOCUMENTOS/IMAGENES
-- Barra de herramientas: una tira de iconos NIMG (cuadrados, en fila; el
-- primer pixel es el color transparente). En DOCUMENTOS/IMAGENES hay tiras hechas:
-- tb_basica16/24.nimg, tb_archivo, tb_edicion, tb_ejecutar, tb_navegar,
-- tb_nautilus, tb_avisos, tb_varios. Al pulsar un boton llega un evento
-- EVENT_GADGETACTION cuyo dato (info_evento) es su numero, desde 0.
function M.crear_barra(archivo, x, y, ancho, alto) return nemo.syscall(172, archivo, x or 0, y or 0, wh(ancho or 0, alto or 0)) end
function M.ayudas_barra(id, textos)                                    -- "Nuevo,Abrir,..." o una tabla
  if type(textos) == "table" then textos = table.concat(textos, ",") end
  nemo.syscall(174, id, textos)
end
function M.activar_boton_barra(id, n, si) nemo.syscall(173, id, n, b01(si)) end
-- El temporizador (crear_temporizador(hz)): evento EVENT_TIMERTICK
function M.liberar_temporizador(t) nemo.syscall(131, t) end
function M.pausar_temporizador(t) nemo.syscall(216, t) end
function M.reanudar_temporizador(t) nemo.syscall(217, t) end
function M.reiniciar_temporizador(t) nemo.syscall(218, t) end
function M.ticks_temporizador(t) return nemo.syscall(219, t) end
-- El arbol: cada nodo es un control; su texto, con leer_texto.
-- Al elegir un nodo llega EVENT_GADGETACTION del arbol: arbol_elegido(a).
function M.crear_arbol(x, y, ancho, alto) return nemo.syscall(175, x, y, wh(ancho, alto)) end
function M.arbol_raiz(a) return nemo.syscall(176, a) end
function M.arbol_anadir(texto, padre) return nemo.syscall(177, texto, padre) end
function M.arbol_insertar(indice, texto, padre) return nemo.syscall(178, indice, texto, padre) end
function M.arbol_cambiar(nodo, texto) nemo.syscall(179, nodo, texto) end
function M.arbol_quitar(nodo) nemo.syscall(180, nodo) end
function M.arbol_abrir(nodo, si) nemo.syscall(181, nodo, b01(si)) end      -- arbol_abrir(n, false) la cierra
function M.arbol_contar(padre) return nemo.syscall(182, padre) end
function M.arbol_elegido(a) return nemo.syscall(183, a) end
function M.arbol_elegir(nodo) nemo.syscall(184, nodo) end
-- El lienzo: una superficie para dibujar. Tras dibujar_en_lienzo(l), todo el
-- dibujo (rect, texto, linea, ovalo, punto, dibujar_imagen) va al lienzo, con
-- sus coordenadas y recortado a el; dibujar_en_ventana() vuelve a la ventana.
function M.crear_lienzo(x, y, ancho, alto) return nemo.syscall(188, x, y, wh(ancho, alto)) end
function M.dibujar_en_lienzo(l) nemo.syscall(128, l + 100000) end        -- el kernel reconoce un lienzo por el +100000
function M.dibujar_en_imagen(img) nemo.syscall(128, img) end
function M.dibujar_en_ventana() nemo.syscall(128, -1) end

-- Dialogos de abrir/guardar -- BLOQUEAN hasta que el usuario elige o
-- cancela (dibujan su propia pantalla encima de la ventana, hay que
-- redibujar el contenido propio despues). Firma real confirmada en
-- src/dialog.c: a0=carpeta de inicio (inodo), a1=buffer de salida
-- para el NOMBRE elegido, a2=tamaño max -> (carpeta<<32 | inodo del
-- archivo), o -1 si se cancelo.
S.OPEN_FILE_DIALOG = 38
S.SAVE_FILE_DIALOG = 39
local function dialogo(syscall_num, carpeta_inicio)
  local buf = nemo.buffer(64)
  local r = nemo.syscall(syscall_num, carpeta_inicio or 0, nemo.direccion(buf), 64)
  if r == -1 then return nil end
  return nemo.cadena(buf), r >> 32   -- nombre, carpeta
end
function M.dialogo_abrir(carpeta_inicio) return dialogo(S.OPEN_FILE_DIALOG, carpeta_inicio) end
function M.dialogo_guardar(carpeta_inicio) return dialogo(S.SAVE_FILE_DIALOG, carpeta_inicio) end

-- Reloj -- SYS_GET_CIVIL_TIME (134) da la fecha/hora entera
-- empaquetada en un solo entero (ver la Parte V de la guia). No hay
-- zona horaria ni configuracion -- es la hora que lleve el reloj del
-- sistema (RTC), tal cual.
S.GET_CIVIL_TIME = 134
function M.fecha_hora()
  local v = nemo.syscall(S.GET_CIVIL_TIME)
  return {
    anio    = (v >> 48) & 0xFFFF,
    mes     = (v >> 40) & 0xFF,
    dia     = (v >> 32) & 0xFF,
    hora    = (v >> 24) & 0xFF,
    minuto  = (v >> 16) & 0xFF,
    segundo = v & 0xFF,
  }
end

-- Imagenes -- SOLO el formato propio NIMG (cabecera "NIMG" + ancho +
-- alto + pixeles RGBA en crudo). Nada de PNG/JPEG reales -- para
-- convertir una foto normal hace falta nimg_convert.py en el host
-- (fuera de Nemo OS). Ver la nota grande junto a SYS_LOAD_IMAGE en
-- syscall.c si hace falta el detalle exacto del formato.
S.LOAD_IMAGE = 49
S.DRAW_IMAGE = 50
S.IMAGE_SIZE = 51
S.FREE_IMAGE = 88
S.LOAD_IMAGE_EN = 253
-- Sin 'carpeta', busca en raiz+DOCUMENTOS como siempre. Con
-- 'carpeta' (un inodo, ver nemo_archivos.buscar), busca AHI dentro
-- -- para imagenes en subcarpetas como DOCUMENTOS/IMAGENES.
--
-- Con 'volumen' se puede cargar de la TARJETA. Va por una
-- syscall aparte (293) y no por un argumento de la 49, porque la 49
-- tambien la llama Nemo Basic y alli el generador solo pone x0: el
-- volumen llegaria con lo que hubiera en el registro.
S.LOAD_IMAGE_VOL = 293
function M.cargar_imagen(nombre, carpeta, volumen)
  local h
  if volumen and volumen ~= 0 then
    h = nemo.syscall(S.LOAD_IMAGE_VOL, nombre, carpeta or 0, volumen)
  elseif carpeta then h = nemo.syscall(S.LOAD_IMAGE_EN, nombre, carpeta)
  else h = nemo.syscall(S.LOAD_IMAGE, nombre) end
  if h < 0 then return nil end
  return h
end
function M.liberar_imagen(handle) nemo.syscall(S.FREE_IMAGE, handle) end
-- Un color que pasa a transparente, para dibujar un icono sobre
-- lo que sea: los iconos tr_*.nimg de DOCUMENTOS/IMAGENES usan el magenta.
M.MAGENTA = 0xFF00FF
function M.mascara_imagen(handle, color) nemo.syscall(92, handle, color or M.MAGENTA) end
-- Su punto de referencia (por defecto la esquina de arriba a la izquierda)
function M.centro_imagen(handle, x, y) nemo.syscall(89, handle, x, y) end
function M.copiar_imagen(handle) return nemo.syscall(93, handle) end
function M.tamano_imagen(handle)
  local v = nemo.syscall(S.IMAGE_SIZE, handle)
  if v == 0 then return nil end
  return (v >> 32), (v & 0xFFFFFFFF) -- ancho, alto
end
function M.dibujar_imagen(handle, x, y) nemo.syscall(S.DRAW_IMAGE, handle, x, y, 0) end

-- ---- Filas de pixeles ----
--
-- Una llamada al sistema cuesta 3,5 us medidos en la Pi 4; un pixel, 4 ns.
-- Trabajar pixel a pixel (leer_pixel/escribir_pixel) es pagar casi 900
-- veces mas peaje que trabajo. Estas cuatro trabajan por FILAS y el
-- trabajo por pixel se queda en el kernel, que es donde es barato.
--
-- El buffer se pide con nemo.buffer(n * 4) y cada pixel es un entero
-- 0xRRGGBB, que se saca con nemo.leer_i32(buf, i * 4) y se mete con
-- nemo.escribir_i32(buf, i * 4, color).
--
-- Solo valen para IMAGENES, no para la ventana: ahi ya hay caminos
-- rapidos de verdad (dibujar_trozo / DrawBlock).
S.FILA_LEER = 289; S.FILA_ESCRIBIR = 290
S.FILA_RELLENAR = 291; S.FILA_TIRA = 292

-- Las cuatro devuelven cuantos pixeles se han tratado de verdad (ya
-- recortados a la imagen), o nil si los argumentos no valen -- un 0 es
-- "no quedaba nada dentro de la imagen", que no es lo mismo.
local function fila_ret(r) if r == -1 then return nil end return r end

-- Lee n pixeles desde (x, y) en 'buf'.
function M.leer_fila(handle, x, y, n, buf)
  return fila_ret(nemo.syscall(S.FILA_LEER, handle + 1, x, y, n, nemo.direccion(buf)))
end
-- Escribe n pixeles de 'buf' en (x, y).
function M.escribir_fila(handle, x, y, n, buf)
  return fila_ret(nemo.syscall(S.FILA_ESCRIBIR, handle + 1, x, y, n, nemo.direccion(buf)))
end
-- Pinta n pixeles de un solo color desde (x, y). Sin buffer.
function M.rellenar_fila(handle, x, y, n, color)
  return fila_ret(nemo.syscall(S.FILA_RELLENAR, handle + 1, x, y, n, color))
end
-- Longitud de la tira de pixeles que desde (x, y) SON de 'color' (o que
-- NO lo son, con distinto = true), avanzando hacia la derecha o, con
-- 'max' negativo, hacia la izquierda. El pixel de partida cuenta si
-- cumple. Es lo que hace barato un relleno por inundacion.
function M.tira_fila(handle, x, y, max, color, distinto)
  return fila_ret(nemo.syscall(S.FILA_TIRA, handle + 1, x, y, max, color, distinto and 1 or 0))
end

-- Dibujo directo en la ventana propia
function M.rect(x, y, ancho, alto, color) nemo.syscall(S.DRAW_RECT, x, y, ancho, alto, color) end

-- ---- Ventanas que cambian de tamaño ----
-- Al maximizar o restaurar una ventana, el programa tiene que recolocar
-- lo suyo: con estas funciones, en cada vuelta del bucle:
--
--   if gui.tamano_cambiado() then colocar() end
--
-- El color de fondo de las ventanas (el de src/wm.c)
M.COLOR_VENTANA = 0xD4D0C8

local ultimo_w, ultimo_h = nil, nil
-- true la primera vez que se llama y cada vez que el tamaño de la zona de
-- dibujo cambia; tambien devuelve el tamaño nuevo.
function M.tamano_cambiado()
  local w, h = M.tamano_ventana()
  if w ~= ultimo_w or h ~= ultimo_h then ultimo_w, ultimo_h = w, h; return true, w, h end
  return false, w, h
end

S.GADGET_RECT = 107; S.GADGET_MOVE = 108; S.GADGET_RESIZE = 109
-- Posicion y tamaño de un control: x, y, ancho, alto (o nil)
function M.rect_gadget(id)
  local r = nemo.syscall(S.GADGET_RECT, id)
  if r == -1 then return nil end
  local function s16(v) v = v & 0xFFFF; return v >= 0x8000 and v - 0x10000 or v end
  return s16(r >> 48), s16(r >> 32), (r >> 16) & 0xFFFF, r & 0xFFFF
end
-- Mueve (y, si se da ancho y alto, redimensiona) un control. Los
-- controles se pintan DENTRO del dibujo de la ventana: su imagen vieja
-- se borra primero, o se quedaria como un fantasma.
-- El kernel ya borra donde estaba el control al moverlo o
-- redimensionarlo, asi que aqui no hace falta pintar encima: antes se hacia a
-- mano y ahora seria pintar dos veces.
function M.mover_gadget(id, x, y, ancho, alto)
  nemo.syscall(S.GADGET_MOVE, id, x, y)
  if ancho and alto then nemo.syscall(S.GADGET_RESIZE, id, ancho, alto) end
end
function M.texto(x, y, s, color)          nemo.syscall(S.DRAW_TEXT, x, y, s, color) end
function M.rgb(r, g, b)                   return (r << 16) | (g << 8) | b end

-- Eventos
--
-- IMPORTANTE, confirmado leyendo gadgets.c: SYS_GADGET_EVENT (113) y
-- SYS_POLL_EVENT (8) leen de la MISMA cola de un solo hueco por
-- debajo (gadgets_poll_raw_event) -- NO son independientes. Llamar a
-- los dos en la misma vuelta hace que el primero se coma el evento
-- del segundo. Para una ventana en modo evento (CreateWindow), que
-- necesita detectar TANTO el cierre como los gadgets, la unica forma
-- correcta es leer con POLL_EVENT una vez por vuelta y repartir segun
-- el codigo que devuelva -- nunca los dos.
function M.sondear_evento() return nemo.syscall(S.POLL_EVENT) end
function M.info_evento()
  local v = nemo.syscall(S.GET_EVENT_INFO)
  return (v >> 32) & 0xFFFFFFFF, v & 0xFFFFFFFF   -- fuente (gadget/ventana), datos
end
-- Sondeo alternativo, SOLO para programas que NO llamen a
-- crear_ventana() (sin modo evento) -- ver gadgetdemo.c real. Si tu
-- programa ya usa crear_ventana()+bucle(), no combines esto con ellos.
function M.evento_gadget() return nemo.syscall(S.GADGET_EVENT) end   -- id, o 0

-- Bucle principal: llama a manejador(evento, fuente, datos) en CADA
-- vuelta, tenga o no evento pendiente (evento=0 si no lo hay) -- asi
-- un programa que solo necesita mirar el teclado (sin gadgets, como
-- nota_rapida.lua) tambien recibe su turno cada vez. Termina al
-- cerrar la ventana o si el manejador devuelve false.
function M.bucle(manejador)
  while true do
    nemo.pump()
    local ev = M.sondear_evento()
    if ev == M.EVENT_WINDOWCLOSE then return end
    local fuente, datos = 0, 0
    if ev ~= 0 then
      fuente, datos = M.info_evento()
    end
    if manejador(ev, fuente, datos) == false then return end
  end
end

-- Teclado
--
-- IMPORTANTE: no existe ningun evento de teclado -- el catalogo real
-- (src/gadgets.h) solo tiene $401/$802/$803/$1001/$4001, ninguno de
-- tecla. El teclado se sondea APARTE, cada vuelta del bucle, con estas
-- funciones -- nunca a traves de sondear_evento()/info_evento().
S.READ_CHAR = 12
S.KEY_DOWN  = 48
S.KEY_HIT   = 53
S.GET_KEY   = 54
S.FLUSH_KEYS = 55

-- Siguiente caracter ASCII pulsado, o 0 si no hay ninguno pendiente.
-- Para texto normal (nombres, contenido de un editor) es la que hace
-- falta: ya viene traducido con mayusculas/simbolos segun el teclado.
function M.leer_tecla() return nemo.syscall(S.READ_CHAR) end

-- Siguiente CODIGO de tecla (scancode estilo evdev, no ASCII) -- para
-- teclas sin caracter propio: flechas, Supr, F1-F12. Los codigos
-- exactos estan en el driver de teclado (src/input.c).
function M.siguiente_codigo() return nemo.syscall(S.GET_KEY) end

-- Si una tecla (por codigo) esta pulsada AHORA MISMO -- para juegos o
-- controles continuos (moverse mientras se mantiene pulsada).
function M.tecla_pulsada(codigo) return nemo.syscall(S.KEY_DOWN, codigo) == 1 end

-- Cuantas veces se pulso una tecla desde la ultima vez (consume el
-- contador) -- para "por flanco", sin repetirse mientras se mantiene.
function M.tecla_flanco(codigo) return nemo.syscall(S.KEY_HIT, codigo) end

function M.vaciar_teclado() nemo.syscall(S.FLUSH_KEYS) end

-- Codigos ASCII de control mas comunes, para no repetir numeros
-- magicos. TECLA_ENTER=10 confirmado en src/input.c (el driver
-- traduce KEY_ENTER a '\n', NO a '\r'/13 como en otros sistemas).
M.TECLA_ENTER = 10
M.TECLA_BACKSPACE = 8
M.TECLA_ESC = 27
M.TECLA_TAB = 9

-- =====================================================================
-- Raton, lineas, elipses e imagenes para sprites
--
-- El kernel ya tenia todo esto -- Nemo Basic lo usa --, pero Lua no lo
-- exponia. Numeros y formatos sacados de src/syscall.c (el codigo manda
-- sobre los comentarios).
-- =====================================================================
S.GET_MOUSE = 34; S.MOUSE_HIT = 56
S.DRAW_OVAL = 47; S.DRAW_LINE = 257
S.GET_EVENT_XY = 256
S.CREATE_IMAGE = 52; S.SET_IMAGE_HANDLE = 89
S.MASK_IMAGE = 92; S.COPY_IMAGE = 93; S.SAVE_IMAGE = 94

-- Raton: posicion DENTRO de la ventana y botones pulsados ahora mismo
-- (1 = izquierdo, 2 = derecho, 4 = central; se suman). Devuelve nil si
-- el raton esta fuera de la ventana o si la ventana no tiene el foco.
function M.raton()
  local v = nemo.syscall(S.GET_MOUSE)
  if v == -1 then return nil end
  return (v >> 32) & 0xFFFF, (v >> 16) & 0xFFFF, v & 0xFFFF
end
-- Si se pulso un boton desde la ultima vez que se pregunto (1 izquierdo
-- por defecto, 2 derecho). Consume el aviso: un clic se ve una vez.
function M.clic(boton) return nemo.syscall(S.MOUSE_HIT, boton or 1) == 1 end
-- Cuanto se ha girado la rueda desde la ultima vez (positivo: hacia arriba).
-- Solo la ventana que tiene el foco la recibe.
function M.rueda() return nemo.syscall(45) end

-- Dibujo
function M.linea(x0, y0, x1, y1, color) nemo.syscall(S.DRAW_LINE, x0, y0, x1, y1, color) end
function M.ovalo(x, y, ancho, alto, color) nemo.syscall(S.DRAW_OVAL, x, y, ancho, alto, color) end
function M.punto(x, y, color)            nemo.syscall(S.DRAW_RECT, x, y, 1, 1, color) end
-- Rellenar la ventana entera de un color (negro si no se da)
function M.limpiar(color)
  local ancho, alto = M.tamano_ventana()
  nemo.syscall(S.DRAW_RECT, 0, 0, ancho, alto, color or 0)
end

-- Donde ocurrio el ultimo evento leido (por ejemplo, un clic en un
-- panel): companero de info_evento().
function M.posicion_evento()
  local v = nemo.syscall(S.GET_EVENT_XY)
  return (v >> 32) & 0xFFFFFFFF, v & 0xFFFFFFFF
end

-- Imagenes para sprites (ademas de cargar/dibujar/liberar, de arriba)
function M.crear_imagen(ancho, alto)   return nemo.syscall(S.CREATE_IMAGE, ancho, alto) end
-- Ese color de la imagen pasa a ser transparente (el magenta 0xFF00FF,
-- por costumbre en los sprites)
function M.transparente(img, color)    nemo.syscall(S.MASK_IMAGE, img, color) end
-- Que punto de la imagen cae en las coordenadas de dibujar_imagen: con
-- el centro, el sprite queda centrado donde se dibuja
function M.punto_agarre(img, x, y)     nemo.syscall(S.SET_IMAGE_HANDLE, img, x, y) end
function M.copiar_imagen(img)          return nemo.syscall(S.COPY_IMAGE, img) end
function M.guardar_imagen(img, nombre) return nemo.syscall(S.SAVE_IMAGE, img, nombre) == 0 end

return M
