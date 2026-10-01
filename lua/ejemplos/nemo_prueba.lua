-- nemo_prueba.lua -- una ventana para programas de prueba.
--
--   local P = require("nemo_prueba")
--   local p = P.nueva("Prueba de algo")
--   p:seccion("Una parte")
--   p:ok("lo que se comprueba", condicion, "detalle opcional")
--   p:nota("algo que conviene saber")
--   p:fin()          -- resumen, y la ventana queda abierta hasta cerrarla
--
-- Cada linea va tambien a la terminal (en QEMU, la de tu Mac). La
-- ventana se desplaza sola al final; con la rueda o las flechas se
-- puede subir a ver lo anterior.
local gui = require("nemo_gui")
local M = {}

local ANCHO, ALTO, LINEA = 600, 460, 18
local FONDO   = gui.rgb(248, 248, 252)
local TINTA   = gui.rgb(30, 32, 44)
local VERDE   = gui.rgb(30, 140, 60)
local ROJO    = gui.rgb(200, 40, 40)
local GRIS    = gui.rgb(110, 112, 125)
local AZUL    = gui.rgb(40, 70, 160)
local BARRA   = gui.rgb(230, 232, 240)

local Prueba = {}
Prueba.__index = Prueba

function M.nueva(titulo)
  gui.crear_ventana(titulo, 90, 50, ANCHO, ALTO)
  gui.usar_fuente("sans", 12)   -- antes, la 5x7 en mayusculas
  local p = setmetatable({ lineas = {}, bien = 0, mal = 0, desde = 1, seguir = true, terminado = false, titulo = titulo }, Prueba)
  p:_dibujar()
  print("== " .. titulo .. " ==")
  return p
end

-- El tamaño ACTUAL de la ventana (cambia al ampliarla o redimensionarla).
-- Antes se dibujaba siempre a 600x460: al ampliar, el resto quedaba negro.
local function tamano()
  local w, h = gui.tamano_ventana()
  if not w or w < 100 or h < 100 then return ANCHO, ALTO end
  return w, h
end

-- Parte un texto en lineas que quepan en 'max_px' pixeles, por palabras,
-- MIDIENDO con la fuente de la ventana (una sans: cada letra mide lo suyo).
-- La continuacion va sangrada. (Antes se cortaba por la derecha.)
local SANGRIA = "      "
local function cabe(t, max_px) return gui.medir_texto(t) <= max_px end
local function partir(texto, max_px)
  if cabe(texto, max_px) then return { texto } end
  local inicio = texto:match("^(%s*)")
  local lineas, actual = {}, inicio
  for p in texto:gmatch("%S+") do
    local palabra = p                                            -- (la variable del bucle es constante en Lua 5.5)
    local candidata = (actual:match("^%s*$")) and (actual .. palabra) or (actual .. " " .. palabra)
    if cabe(candidata, max_px) then
      actual = candidata
    else
      if not actual:match("^%s*$") then lineas[#lineas + 1] = actual end
      while not cabe(SANGRIA .. palabra, max_px) and #palabra > 1 do    -- una palabra mas larga que la linea
        local n = #palabra
        while n > 1 and not cabe(SANGRIA .. palabra:sub(1, n), max_px) do n = n - 1 end
        lineas[#lineas + 1] = SANGRIA .. palabra:sub(1, n)
        palabra = palabra:sub(n + 1)
      end
      actual = SANGRIA .. palabra
    end
  end
  if not actual:match("^%s*$") then lineas[#lineas + 1] = actual end
  return lineas
end

-- Las lineas tal como se ven: partidas al ancho actual.
function Prueba:_visibles(w)
  local caben = math.max(100, w - 20)                     -- pixeles utiles
  if self.cache_caben == caben and self.cache_n == #self.lineas then return self.cache end
  local v = {}
  for _, l in ipairs(self.lineas) do
    for _, trozo in ipairs(partir(l.texto, caben)) do v[#v + 1] = { texto = trozo, color = l.color } end
  end
  self.cache, self.cache_caben, self.cache_n = v, caben, #self.lineas
  return v
end

function Prueba:_dibujar()
  local w, h = tamano()
  self.w, self.h = w, h
  local v = self:_visibles(w)
  local caben = math.max(1, (h - 40) // LINEA)            -- lineas que caben (40 px abajo: el resumen)
  if self.seguir then self.desde = math.max(1, #v - caben + 1) end
  if self.desde > math.max(1, #v - caben + 1) then self.desde = math.max(1, #v - caben + 1) end
  if self.desde < 1 then self.desde = 1 end
  self.caben, self.nvis = caben, #v
  gui.limpiar(FONDO)
  for i = self.desde, math.min(#v, self.desde + caben - 1) do
    gui.texto(10, 6 + (i - self.desde) * LINEA, v[i].texto, v[i].color)
  end
  gui.rect(0, h - 34, w, 34, BARRA)
  local resumen
  if self.terminado then
    resumen = (self.mal == 0) and string.format("TODO BIEN: %d comprobaciones", self.bien)
                              or string.format("%d bien, %d MAL (en rojo, arriba)", self.bien, self.mal)
  else
    resumen = string.format("Comprobando... %d bien, %d mal", self.bien, self.mal)
  end
  gui.texto(10, h - 24, resumen, (self.mal > 0) and ROJO or (self.terminado and VERDE or TINTA))
  if #v > caben then gui.texto(w - 170, h - 24, "rueda / flechas", GRIS) end
end

function Prueba:_linea(texto, color)
  self.lineas[#self.lineas + 1] = { texto = texto, color = color }
  print(texto)
  self.seguir = true                                   -- mientras se comprueba, siempre se ve lo ultimo
  self:_dibujar()
  nemo.pump()                                          -- que se vea mientras se comprueba
end

function Prueba:seccion(texto) self:_linea("", TINTA); self:_linea("-- " .. texto .. " --", AZUL) end
function Prueba:nota(texto) self:_linea("   " .. texto, GRIS) end

function Prueba:ok(nombre, condicion, detalle)
  if condicion then self.bien = self.bien + 1 else self.mal = self.mal + 1 end
  local t = (condicion and "bien  " or "MAL   ") .. nombre
  if detalle and detalle ~= "" then t = t .. "  (" .. tostring(detalle) .. ")" end
  self:_linea(t, condicion and VERDE or ROJO)
  return condicion
end

function Prueba:fin()
  self.terminado = true
  print(self.mal == 0 and ("TODO BIEN: " .. self.bien .. " comprobaciones") or (self.bien .. " bien, " .. self.mal .. " MAL"))
  self:_dibujar()
  gui.bucle(function(ev)
    -- la ventana ha cambiado de tamaño: repintar a la medida nueva
    if ev == gui.EVENT_WINDOWSIZE then self:_dibujar() return end
    local w, h = tamano()
    if w ~= self.w or h ~= self.h then self:_dibujar() end
    local arriba, abajo = false, false
    local rueda = nemo.syscall(45)                     -- SYS_GET_MOUSE_WHEEL
    if rueda > 0 then arriba = true elseif rueda < 0 then abajo = true end
    local c = gui.siguiente_codigo and gui.siguiente_codigo() or 0
    if c == 103 then arriba = true elseif c == 108 then abajo = true end   -- flechas (codigos evdev)
    if arriba and self.desde > 1 then self.seguir = false; self.desde = math.max(1, self.desde - 3); self:_dibujar() end
    if abajo and self.desde + self.caben <= self.nvis then
      self.desde = self.desde + 3
      self.seguir = (self.desde + self.caben > self.nvis)   -- al llegar al final, vuelve a seguirlo
      self:_dibujar()
    end
  end)
end

-- ¿Estamos en QEMU? La simulacion tiene una huella inconfundible: una
-- EEPROM en la direccion I2C 0x50 y un conversor en el SPI que da
-- exactamente 37 y 165 en sus canales 0 y 1. Una Pi con las dos cosas a
-- la vez es practicamente imposible. (Se queda los pines del I2C y del
-- SPI para este programa, hasta que termine.)
local qemu = nil
function M.es_qemu()
  if qemu ~= nil then return qemu end
  local gpio = require("nemo_gpio")
  local eeprom = gpio.i2c_leer(0x50, 1)
  local c0, c1 = gpio.mcp3008(0), gpio.mcp3008(1)
  qemu = (eeprom ~= nil) and c0 == 37 and c1 == 165
  return qemu
end

return M
