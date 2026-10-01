-- nemo_sistema.lua -- gestor de tareas y monitor de sistema, sobre
-- las syscalls 248-252 y 285-288. Ver las notas junto a cada #define
-- en syscall.h para las limitaciones reales: no hay medida de CPU en
-- el sentido tradicional, por como es el planificador.
--
-- La cabecera decia tambien "no hay deteccion de RAM
-- fisica". Eso dejo de ser cierto, cuando memoria.c
-- empezo a detectar la RAM para poder mapearla entera; lo que faltaba
-- era una syscall que lo contara. Ya existe: ram_fisica().
local M = {}
local S = { TASK_LIST=248, TASK_KILL=249, DISK_USAGE=250, RAM_TASKS=251, RAM_HEAP=252, RAM_POOL=260,
            CPU_NOMBRE=285, CPU_DATOS=286, RAM_FISICA=287, RAM_NUCLEO=288,
            TICKS=2, DISK_FISICO=279, DISK_PARTICION=280, PANTALLA=35 }

-- Lista de tareas vivas: {slot=, ventana=, turnos=, nombre=}. 'slot'
-- es el numero a pasar a matar_tarea(); 'turnos' es cuantas veces el
-- planificador le ha dado la CPU desde que arranco el sistema --
-- relativo entre tareas, no un porcentaje de ocupacion real.
function M.listar_tareas()
  local MAX = 16
  local buf = nemo.buffer(MAX * 48)
  local n = nemo.syscall(S.TASK_LIST, nemo.direccion(buf), MAX)
  local resultado = {}
  for i = 0, n - 1 do
    local off = i * 48
    resultado[#resultado + 1] = {
      slot = nemo.leer_i32(buf, off),
      ventana = nemo.leer_i32(buf, off + 4),
      turnos = nemo.leer_i32(buf, off + 8),
      nombre = nemo.cadena_en(buf, off + 16),
    }
  end
  return resultado
end

-- Mata una tarea por su 'slot' -- cierra tambien su ventana. true si
-- se encontro y estaba viva.
function M.matar_tarea(slot) return nemo.syscall(S.TASK_KILL, slot) == 1 end

-- Espacio en disco de NemoFS, en BLOQUES de 512 bytes cada uno.
function M.uso_disco()
  local v = nemo.syscall(S.DISK_USAGE)
  return (v >> 32), (v & 0xFFFFFFFF) -- total, usado
end

-- NO es memoria RAM del sistema -- Nemo OS no detecta memoria fisica.
-- Cuantas TAREAS hay vivas y cuantas caben como maximo (huecos).
-- Ojo: esto ya NO dice nada de la memoria. Desde que cada tarea recibe
-- una region de su propio tamaño, pocas tareas grandes pueden ocupar
-- mas que muchas pequeñas. Para la memoria, memoria_de_tareas().
function M.bloques_de_tarea()
  local v = nemo.syscall(S.RAM_TASKS)
  return (v >> 32), (v & 0xFFFFFFFF) -- en_uso, total
end

-- Memoria de la reserva de tareas, en bytes: cuanta ocupan entre todas
-- las tareas vivas, y cuanta hay en total. (El kernel la da en KB desde
-- que la reserva puede pasar de 4 GB.)
function M.memoria_de_tareas()
  local v = nemo.syscall(S.RAM_POOL)
  return (v >> 32) * 1024, (v & 0xFFFFFFFF) * 1024 -- usado, total
end

-- Heap del KERNEL (16MB fijos), en bytes -- distinto de la memoria de
-- las tareas, y distinto tambien de la memoria propia de este mismo
-- interprete Lua (esa la da nemo.memoria()).
function M.heap_kernel()
  local v = nemo.syscall(S.RAM_HEAP)
  return (v >> 32), (v & 0xFFFFFFFF) -- usado, total
end

-- =====================================================================
-- Tiempo y consola
-- =====================================================================
S.SLEEP = 1; S.READ_LINE = 258

-- Esperar unos milisegundos, DURMIENDO: el programa no gasta nada
-- mientras tanto y el nucleo queda libre. El kernel cuenta en latidos de
-- 10 ms, asi que se redondea hacia arriba: esperar(1) espera un latido.
function M.esperar(ms)
  nemo.syscall(S.SLEEP, (ms + 9) // 10)
end

-- Milisegundos desde que arranco el sistema (de 10 en 10: es la
-- resolucion del reloj). Para medir tiempos o marcar el ritmo de un
-- juego sin depender de lo rapido que vaya el programa.
function M.milisegundos() return nemo.ticks() * 10 end

-- Leer una linea del teclado (con eco y retroceso), para scripts de
-- CONSOLA -- los que se lanzan desde la shell. Devuelve el texto sin el
-- salto de linea, o nil si no se pudo (por ejemplo, si el script no
-- tiene consola). Mientras espera, la shell esconde su indicador.
function M.leer_linea(max)
  max = max or 256
  local b = nemo.buffer(max + 1)          -- un byte de mas: siempre queda terminada en 0
  local n = nemo.syscall(S.READ_LINE, nemo.direccion(b), max)
  if n < 0 then return nil end
  return nemo.cadena(b)
end

-- ---------------------------------------------------------------
-- Datos del equipo
-- ---------------------------------------------------------------

-- Nombre del procesador, leido de MIDR_EL1 por el nucleo.
function M.cpu_nombre()
  local buf = nemo.buffer(64)
  local n = nemo.syscall(S.CPU_NOMBRE, nemo.direccion(buf), 64)
  if n <= 0 then return "desconocido" end
  return nemo.cadena_en(buf, 0)
end

-- Nucleos en marcha y frecuencia en MHz. Ojo: mhz vale 0 cuando no se
-- puede saber (QEMU), que NO es lo mismo que ir a 0 MHz -- quien lo
-- enseñe deberia escribir "desconocida" en ese caso, no "0 MHz".
function M.cpu_datos()
  local v = nemo.syscall(S.CPU_DATOS)
  return (v >> 32), (v & 0xFFFFFFFF)   -- nucleos, mhz
end

-- RAM FISICA de la placa, en bytes. La de verdad: no confundir con la
-- reserva de tareas ni con el monton del nucleo, que son repartos que
-- el sistema hace DENTRO de esta.
function M.ram_fisica() return nemo.syscall(S.RAM_FISICA) end

-- Lo que ocupa el nucleo cargado en memoria (codigo + datos + bss).
function M.ram_nucleo() return nemo.syscall(S.RAM_NUCLEO) end

-- Segundos encendido. El contador del planificador va a 100 Hz.
function M.encendido_segundos() return nemo.syscall(S.TICKS) // 100 end

-- Tamaño de la tarjeta, en bytes (sectores de 512).
function M.tarjeta_bytes() return nemo.syscall(S.DISK_FISICO) * 512 end

-- Las cuatro entradas de la tabla de particiones: {tipo=, inicio=,
-- sectores=} por cada una que no este vacia.
function M.particiones()
  local r = {}
  for i = 0, 3 do
    local v = nemo.syscall(S.DISK_PARTICION, i)
    if v ~= 0 then
      r[#r + 1] = { indice = i, tipo = (v >> 56) & 0xFF,
                    inicio = (v >> 28) & 0xFFFFFFF, sectores = v & 0xFFFFFFF }
    end
  end
  return r
end

-- Resolucion de pantalla activa ahora mismo.
function M.pantalla()
  local v = nemo.syscall(S.PANTALLA)
  return (v >> 32), (v & 0xFFFFFFFF)
end

return M
