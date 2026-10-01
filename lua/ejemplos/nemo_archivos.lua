-- nemo_archivos.lua -- lectura, escritura, listado y navegacion de
-- carpetas, en las DOS unidades (NemoFS y FAT).
--
-- BUG REAL CORREGIDO (version anterior): listar()/buscar()/es_carpeta()
-- usaban SYS_DIR_OPEN/SYS_DIR_NEXT/SYS_FIND_CHILD -- confirmado
-- leyendo syscall.c que estas tres estan CABLEADAS solo a NemoFS
-- (llaman a nemofs_list_dir/nemofs_find_child directamente, sin mirar
-- ningun volumen). Nunca iban a funcionar con FAT. La version de
-- verdad usa SYS_FILE_LIST (23), que si admite volumen, con su
-- formato binario de 40 bytes por entrada (inodo, tipo, tamaño,
-- nombre) decodificado con nemo.leer_i32/nemo.cadena_en.
local M = {}
local S = { FILE_OPEN=20, FILE_READ=21, FILE_WRITE=22, FILE_LIST=23, DIR_CREATE=24, FILE_DELETE=25, FILE_CLOSE=274 }

M.RAIZ = 0
M.VOL_NEMOFS = 0
M.VOL_FAT = 1
M.TIPO_NADA = 0
M.TIPO_ARCHIVO = 1
M.TIPO_CARPETA = 2

local ENTRY_SZ = 40
local MAX_ENTRADAS = 128

-- Lista de {nombre=, tipo=, inodo=, tamano=} de la carpeta 'padre'
-- (M.RAIZ por defecto) en 'volumen' (M.VOL_NEMOFS por defecto).
function M.listar(padre, volumen)
  padre = padre or M.RAIZ; volumen = volumen or M.VOL_NEMOFS
  local buf = nemo.buffer(MAX_ENTRADAS * ENTRY_SZ)
  local n = nemo.syscall(S.FILE_LIST, padre, nemo.direccion(buf), MAX_ENTRADAS, volumen)
  local resultado = {}
  if n < 0 then return resultado end
  for i = 0, n - 1 do
    local off = i * ENTRY_SZ
    resultado[#resultado + 1] = {
      inodo = nemo.leer_i32(buf, off),
      tipo = nemo.leer_i32(buf, off + 4),
      tamano = nemo.leer_i32(buf, off + 8),
      nombre = nemo.cadena_en(buf, off + 12),
    }
  end
  return resultado
end

-- Busca 'nombre' dentro de 'padre'/'volumen' -> la entrada
-- {nombre,tipo,inodo,tamano}, o nil si no existe. (No hay una
-- syscall de "buscar uno solo" volumen-consciente -- se lista y se
-- compara; las carpetas de un sistema como este son pequeñas.)
function M.buscar(nombre, padre, volumen)
  for _, e in ipairs(M.listar(padre, volumen)) do
    if e.nombre == nombre then return e end
  end
  return nil
end

function M.es_carpeta_entrada(entrada) return entrada ~= nil and entrada.tipo == M.TIPO_CARPETA end

function M.crear_carpeta(nombre, padre, volumen)
  volumen = volumen or M.VOL_NEMOFS
  if volumen == M.VOL_FAT then return false end -- FAT no admite carpetas todavia (mismo limite que la shell real en C)
  return nemo.syscall(S.DIR_CREATE, nombre, padre or M.RAIZ, volumen) >= 0
end

function M.borrar_en(nombre, padre, volumen)
  return nemo.syscall(S.FILE_DELETE, nombre, padre or M.RAIZ, volumen or M.VOL_NEMOFS) == 0
end

-- Contenido entero de 'nombre' dentro de 'padre'/'volumen'.
-- SYS_FILE_OPEN crea el archivo si no existe -- usa M.buscar()
-- primero si eso importa.
-- 'maximo' opcional: bytes a leer como mucho (por defecto 64KB; el
-- visor de HTML/Markdown pide mas para guias largas). NemoFS admite
-- archivos de hasta 8MB, pero el pool de Lua de una tarea son 12MB:
-- no pidas mas de un par de MB.
function M.leer_en(nombre, padre, volumen, maximo)
  volumen = volumen or M.VOL_NEMOFS
  maximo = maximo or 65536
  local id = nemo.syscall(S.FILE_OPEN, nombre, padre or M.RAIZ, volumen)
  if id < 0 then return nil end
  -- Reservar lo que ocupa el archivo, no el maximo permitido. Antes se
  -- reservaba 'maximo' entero (y puesto a cero): el visor pide hasta 1 MB
  -- para las guias largas, y con los 4 MB de memoria de una app de Lua
  -- eso era un cuarto de toda su memoria solo para leer un archivo de
  -- 17 KB. Si no se sabe el tamano, se usa el maximo, como antes.
  local e = M.buscar(nombre, padre or M.RAIZ, volumen)
  if e and e.tamano and e.tamano >= 0 and e.tamano < maximo then maximo = e.tamano + 1 end
  local buf = nemo.buffer(maximo)
  local n = nemo.syscall(S.FILE_READ, id, nemo.direccion(buf), maximo, volumen)
  -- Cerrar SIEMPRE, tambien cuando la lectura falla. En FAT los huecos de
  -- archivo abierto son ocho en todo el sistema y no se sueltan hasta que
  -- muere el programa que los pidio: una app de Lua que viva un rato
  -- leyendo archivos de la tarjeta los agota sin darse cuenta.
  nemo.syscall(S.FILE_CLOSE, id, volumen)
  if n < 0 then return nil end
  return nemo.cadena(buf):sub(1, n)
end

-- Escribe 'contenido' entero en 'nombre' dentro de 'padre'/'volumen'.
-- NOTA real del kernel: en FAT esto solo funciona para archivos
-- NUEVOS (los que SYS_FILE_OPEN no encontro ya existentes).
function M.escribir_en(nombre, contenido, padre, volumen)
  volumen = volumen or M.VOL_NEMOFS
  local id = nemo.syscall(S.FILE_OPEN, nombre, padre or M.RAIZ, volumen)
  if id < 0 then return false end
  -- SYS_FILE_WRITE devuelve (uint64_t)-1 en el kernel al fallar, que
  -- llega a Lua como el entero -1 (con signo, 64 bits).
  local ok = nemo.syscall(S.FILE_WRITE, id, contenido, #contenido, volumen)
  nemo.syscall(S.FILE_CLOSE, id, volumen)
  return ok ~= -1
end

-- ---- atajos para archivos sueltos en la raiz de NemoFS (el caso
-- mas comun, sin navegar carpetas ni pensar en FAT) ----
-- =====================================================================
-- Leer POR PARTES (fase 5 de la memoria,)
-- =====================================================================
-- Para archivos grandes: en vez de cargarlos enteros, se leen trozos
-- desde cualquier posicion. Sirve en los dos volumenes, pero los archivos
-- grandes de verdad estan en FAT (M.VOL_FAT: en la Pi, la particion de
-- arranque, la que ve el Mac); en NemoFS ninguno pasa de ~8 MB.
S.FILE_READ_AT = 262
M.TROZO_MAX = 256 * 1024     -- lo que el kernel lee como mucho por llamada

-- Abre un archivo existente -> identificador, o nil.
function M.abrir(nombre, padre, volumen)
  local id = nemo.syscall(S.FILE_OPEN, nombre, padre or M.RAIZ, volumen or M.VOL_NEMOFS)
  if id < 0 then return nil end
  return id
end

-- Suelta el identificador que devolvio M.abrir. Obligatorio en FAT (ocho
-- huecos en todo el sistema); en NemoFS no hace nada, pero llamarla
-- siempre evita tener que acordarse de en que volumen se estaba.
function M.cerrar(id, volumen)
  if not id then return end
  nemo.syscall(S.FILE_CLOSE, id, volumen or M.VOL_NEMOFS)
end

-- Lee hasta 'n' bytes desde 'posicion' EN el buffer 'buf' (de nemo.buffer,
-- de al menos n bytes) -> cuantos leyo (0 en el final), o nil si falla.
-- Para no crear un buffer por llamada al leer mucho, se reutiliza uno.
function M.leer_desde(id, posicion, n, volumen, buf)
  if n > M.TROZO_MAX then n = M.TROZO_MAX end
  local r = nemo.syscall(S.FILE_READ_AT, id, nemo.direccion(buf), n, posicion, volumen or M.VOL_NEMOFS)
  if r < 0 then return nil end
  return r
end

-- Recorre un archivo entero a trozos: llama a cada_trozo(datos, posicion)
-- con cada uno (datos es una cadena, ceros incluidos). Si cada_trozo
-- devuelve false, se para. Devuelve el total leido, o nil si falla.
function M.leer_por_partes(nombre, padre, volumen, cada_trozo, tam_trozo)
  volumen = volumen or M.VOL_NEMOFS
  local id = M.abrir(nombre, padre, volumen)
  if not id then return nil end
  tam_trozo = math.min(tam_trozo or M.TROZO_MAX, M.TROZO_MAX)
  local buf = nemo.buffer(tam_trozo)
  local pos = 0
  while true do
    local n = M.leer_desde(id, pos, tam_trozo, volumen, buf)
    if n == nil then M.cerrar(id, volumen); return nil end
    if n == 0 then break end
    if cada_trozo(nemo.bytes(buf, 0, n), pos) == false then break end
    pos = pos + n
  end
  M.cerrar(id, volumen)
  return pos
end

-- =====================================================================
-- Escribir POR PARTES (fase 5b): añadir al final, solo en FAT
-- =====================================================================
S.FILE_APPEND = 263
M.TROZO_ESCRITURA = 64 * 1024   -- lo que el kernel escribe como mucho por llamada

-- Añade 'datos' (una cadena, de cualquier tamaño) al final del archivo
-- abierto 'id' del volumen FAT, en trozos de 64 KB. Devuelve cuantos
-- bytes añadio (si el disco se llena, menos que #datos), o nil si falla.
function M.anadir(id, datos, volumen)
  volumen = volumen or M.VOL_FAT
  local total, pos = 0, 1
  while pos <= #datos do
    local trozo = datos:sub(pos, pos + M.TROZO_ESCRITURA - 1)
    local r = nemo.syscall(S.FILE_APPEND, id, trozo, #trozo, volumen)
    if r < 0 then return total > 0 and total or nil end
    total = total + r
    if r < #trozo then break end               -- disco lleno
    pos = pos + #trozo
  end
  return total
end

-- Crea (o rehace desde cero) un archivo del volumen FAT pidiendo los
-- trozos uno a uno: siguiente(n) devuelve el trozo numero n (desde 0), o
-- nil para terminar. Nunca hace falta tener el archivo entero en memoria.
-- 'cada' (opcional) se llama tras cada trozo con el total escrito.
-- Devuelve el total escrito, o nil si falla.
function M.crear_por_partes(nombre, padre, siguiente, cada)
  padre = padre or M.RAIZ
  M.borrar_en(nombre, padre, M.VOL_FAT)        -- empezar de cero (si no existia, no pasa nada)
  local id = M.abrir(nombre, padre, M.VOL_FAT)
  if not id then return nil end
  local total, n = 0, 0
  while true do
    local trozo = siguiente(n)
    if trozo == nil then break end
    local r = M.anadir(id, trozo, M.VOL_FAT)
    if r == nil then return nil end
    total = total + r
    if r < #trozo then return nil end          -- disco lleno
    if cada then cada(total) end
    n = n + 1
  end
  -- archivo de 0 bytes: que exista igualmente
  if total == 0 then nemo.syscall(S.FILE_APPEND, id, "", 0, M.VOL_FAT) end
  return total
end

function M.leer(ruta) return nemo.leer_archivo(ruta) end
function M.escribir(ruta, contenido) return M.escribir_en(ruta, contenido, M.RAIZ, M.VOL_NEMOFS) end
function M.existe(ruta) local e = M.buscar(ruta, M.RAIZ, M.VOL_NEMOFS); return e ~= nil end
function M.borrar(ruta) return M.borrar_en(ruta, M.RAIZ, M.VOL_NEMOFS) end

return M
