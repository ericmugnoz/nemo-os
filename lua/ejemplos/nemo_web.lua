-- nemo_web.lua -- Nemo OS
--
-- La parte del navegador que es LOGICA PURA: direcciones, enlaces,
-- historial y la configuracion del proxy. Ni pantalla, ni red, ni
-- syscalls.
--
-- Esta aparte por la misma razon que dhcp.c o nmz.c estan aparte de
-- quien los usa: asi se puede probar entero en el Mac, con el
-- interprete de Lua compilado para host (lua/lua_host), sin placa y sin
-- red. Y lo que hay aqui lo necesita: son las cosas que fallan EN
-- SILENCIO. Un porcentaje mal descodificado no da error, da un enlace
-- que lleva a otro sitio. Una IP mal empaquetada no da error, manda los
-- paquetes a ninguna parte. El historial mal llevado no da error, da un
-- boton "adelante" que va a donde no debe.

local M = {}

-- ---------------------------------------------------------------
-- Direcciones
-- ---------------------------------------------------------------

-- La IP que espera SYS_NET_BAJAR: a | b<<8 | c<<16 | d<<24.
-- nil si no es una IP.
function M.ip_empaquetada(s)
  if type(s) ~= "string" then return nil end
  local a, b, c, d = s:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
  if not a then return nil end
  a, b, c, d = tonumber(a), tonumber(b), tonumber(c), tonumber(d)
  if a > 255 or b > 255 or c > 255 or d > 255 then return nil end
  return a | (b << 8) | (c << 16) | (d << 24)
end

-- Lo que se le pide al proxy para una direccion.
function M.ruta_para(url)
  return "/nmz?u=" .. url
end

-- Deshacer el escapado por ciento ("%2F" -> "/"). El proxy escapa la
-- direccion al meterla en el enlace; aqui hay que deshacerlo para
-- volver a tener la direccion de verdad, que es lo que se ensena en la
-- barra y lo que se guarda en el historial.
function M.descifrar_por_ciento(s)
  return (s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end

-- A donde lleva un enlace de una pagina que vino del proxy. Devuelve la
-- direccion, o nil si el enlace no se puede seguir (un ancla dentro de
-- la pagina, un "javascript:", un enlace vacio).
function M.url_de_enlace(href)
  if type(href) ~= "string" or href == "" then return nil end
  local u = href:match("^/nmz%?u=(.+)$")
  if u then return M.descifrar_por_ciento(u) end
  if href:match("^https?://") then return href end
  -- "ejemplo.com/algo" sin esquema: se acepta si parece un dominio.
  if href:match("^[%w%-%.]+%.%a%a+") then return href end
  return nil
end

-- ---------------------------------------------------------------
-- El proxy
-- ---------------------------------------------------------------

-- Lee "10.0.2.2:8080" (lo que hay en PROXY.CFG). Devuelve ip, puerto.
function M.parsear_proxy(texto)
  if type(texto) ~= "string" then return nil end
  local ip, puerto = texto:match("^%s*([%d%.]+)%s*:%s*(%d+)")
  if not ip or not M.ip_empaquetada(ip) then return nil end
  puerto = tonumber(puerto)
  if puerto < 1 or puerto > 65535 then return nil end
  return ip, puerto
end

-- "proxy 192.168.1.40:8080" escrito en la barra de direcciones. Va ahi
-- y no en un menu de opciones porque es lo primero que hay que hacer en
-- una instalacion nueva, y buscarlo en un menu seria una perdida de
-- tiempo para algo que se toca una vez.
function M.orden_proxy(texto)
  if type(texto) ~= "string" then return nil end
  local resto = texto:match("^%s*proxy%s+(.+)$")
  if not resto then return nil end
  return M.parsear_proxy(resto)
end

-- ---------------------------------------------------------------
-- El historial
--
-- Una lista y una posicion, como en cualquier navegador. La regla que
-- importa --y la que es facil equivocarse-- es que ir a un sitio nuevo
-- BORRA todo lo que hubiera hacia delante: en cuanto tomas otra rama,
-- la que habias dejado atras deja de tener sentido.
-- ---------------------------------------------------------------
local H = {}
H.__index = H

function M.historial()
  return setmetatable({ lista = {}, pos = 0 }, H)
end

function H:ir(url)
  -- Recargar el mismo sitio no crea una entrada nueva: si no, pulsar
  -- "recargar" tres veces dejaria tres entradas iguales y el boton de
  -- atras no llevaria a ninguna parte.
  if self.lista[self.pos] == url then return end
  for i = #self.lista, self.pos + 1, -1 do self.lista[i] = nil end
  self.lista[#self.lista + 1] = url
  self.pos = #self.lista
end

function H:puede_atras()    return self.pos > 1 end
function H:puede_adelante() return self.pos < #self.lista end
function H:actual()         return self.lista[self.pos] end

function H:atras()
  if not self:puede_atras() then return nil end
  self.pos = self.pos - 1
  return self.lista[self.pos]
end

function H:adelante()
  if not self:puede_adelante() then return nil end
  self.pos = self.pos + 1
  return self.lista[self.pos]
end

return M
