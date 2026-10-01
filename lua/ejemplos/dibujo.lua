-- dibujo.lua -- un cuaderno de dibujo con el raton
--
-- Arrastra con el boton izquierdo para dibujar. Boton derecho: borrar
-- todo. Teclas 1 a 4: cambiar de color. Usa las funciones de raton,
-- lineas y elipses de nemo_gui.
local gui = require("nemo_gui")

local ANCHO, ALTO = 480, 360
local FONDO = gui.rgb(250, 250, 245)
local colores = {
  gui.rgb(20, 20, 20),    -- 1: negro
  gui.rgb(200, 30, 30),   -- 2: rojo
  gui.rgb(30, 120, 200),  -- 3: azul
  gui.rgb(40, 160, 60),   -- 4: verde
}
local TECLA_1 = 2          -- codigos de tecla: 1 = 2, 2 = 3, 3 = 4, 4 = 5

gui.crear_ventana("Dibujo", 80, 80, ANCHO, ALTO)

local color = colores[1]
local ax, ay = nil, nil   -- ultimo punto mientras se arrastra

-- El tamaño ACTUAL de la zona de dibujo: cambia al maximizar
-- o restaurar. Antes era fijo, y al ampliar, lo nuevo se veia negro.
local ancho_c, alto_c = ANCHO, ALTO
local function medir()
  local w, h = gui.tamano_ventana()
  if not w or w < 100 or h < 60 then return ANCHO, ALTO end
  return w, h
end

local PALETA_W, PALETA_H = 10 + #colores * 24, 24
local function muestra()
  -- la paleta, abajo a la izquierda: el color activo, mas grande. Se
  -- limpia antes (si no, el circulo grande del color anterior dejaba un
  -- anillo alrededor del pequeño).
  gui.rect(0, alto_c - 34, PALETA_W, PALETA_H, FONDO)
  for i, c in ipairs(colores) do
    local tam = (c == color) and 18 or 12
    gui.ovalo(10 + (i - 1) * 24, alto_c - 30, tam, tam, c)
  end
end

-- La ventana ha cambiado de tamaño: la zona NUEVA, del color del papel (lo
-- ya dibujado se conserva), y la paleta, a su sitio nuevo.
local function redimensionar(w, h)
  gui.rect(0, alto_c - 34, PALETA_W, PALETA_H, FONDO)      -- la paleta vieja fuera
  if w > ancho_c then gui.rect(ancho_c, 0, w - ancho_c, h, FONDO) end
  if h > alto_c then gui.rect(0, alto_c, w, h - alto_c, FONDO) end
  ancho_c, alto_c = w, h
  muestra()
end

ancho_c, alto_c = medir()
gui.limpiar(FONDO)
muestra()

gui.bucle(function(evento)
  local w, h = medir()
  if w ~= ancho_c or h ~= alto_c then redimensionar(w, h) end
  -- cambiar de color con las teclas 1 a 4
  for i = 1, #colores do
    if gui.tecla_flanco(TECLA_1 + i - 1) > 0 then color = colores[i]; muestra() end
  end

  -- boton derecho: borrar
  if gui.clic(2) then gui.limpiar(FONDO); muestra() end

  -- boton izquierdo pulsado: una linea desde el punto anterior
  local x, y, botones = gui.raton()
  if x and (botones & 1) ~= 0 then
    if ax then gui.linea(ax, ay, x, y, color) else gui.ovalo(x - 1, y - 1, 3, 3, color) end
    ax, ay = x, y
  else
    ax, ay = nil, nil
  end
end)
