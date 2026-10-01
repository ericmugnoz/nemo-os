-- monitor_sistema.lua -- que equipo es este y en que se le esta yendo
-- la memoria, el disco y los turnos de procesador.
--
-- Reescrito. Antes eran tres etiquetas y una lista, y solo
-- sabia hablar de los repartos que hace el propio sistema (memoria de
-- tareas, monton del nucleo) -- nada del equipo de verdad. Ahora el
-- nucleo expone el procesador (MIDR_EL1) y la RAM fisica que detecta al
-- arrancar, asi que ya se puede enseñar la maquina entera.
--
-- Se dibuja a mano en vez de con etiquetas y listas, por dos razones:
-- las barras de ocupacion dicen de un vistazo lo que una cifra tarda en
-- decir, y la lista del sistema solo enseña las filas que le caben, sin
-- desplazamiento, asi que con 16 tareas se perderian las ultimas.
local gui = require("nemo_gui")

gui.crear_ventana("Monitor del sistema", 130, 90, 580, 640)
local sistema = require("nemo_sistema")

-- Paleta: fondo claro de ventana, y una barra por cada cosa que se
-- llena. Los colores de las barras separan lo que es del SISTEMA
-- (azules) de lo que es del DISCO (verde) y de las tareas (ambar).
local C_FONDO    = gui.COLOR_VENTANA
local C_TITULO   = 0x203060
local C_TEXTO    = 0x1C1C20
local C_SUAVE    = 0x606070
local C_LINEA    = 0xA0A0A0
local C_HUECO    = 0xBCB8B0
local C_BARRA_SIS = 0x3A6EA5
local C_BARRA_DIS = 0x2E8B57
local C_BARRA_TAR = 0xC08040
local C_AVISO    = 0xCC3333

local f_normal = gui.cargar_fuente("sans", 14, false, false, false)
local f_fuerte = gui.cargar_fuente("sans", 14, true, false, false)
local f_titulo = gui.cargar_fuente("sans", 16, true, false, false)
local f_menuda = gui.cargar_fuente("sans", 12, false, false, false)

-- Donde empieza la columna de valores. 150 se quedaba corto: la
-- etiqueta mas larga ("Montón del núcleo") se comia su propio valor.
local COL_VALOR = 178

local ancho_v, alto_v = 580, 640

-- ---------------------------------------------------------------
-- Formato
-- ---------------------------------------------------------------
-- Con coma decimal: aqui se escribe "8,00 GB", no "8.00 GB".
local function coma(s) return (s:gsub("%.", ",")) end

local function bytes(n)
  if n == nil then return "--" end
  if n >= 1024 * 1024 * 1024 then return coma(string.format("%.2f GB", n / (1024 * 1024 * 1024)))
  elseif n >= 1024 * 1024 then return coma(string.format("%.1f MB", n / (1024 * 1024)))
  elseif n >= 1024 then return coma(string.format("%.1f KB", n / 1024))
  else return n .. " B" end
end

local function duracion(seg)
  local d = seg // 86400
  local h = (seg % 86400) // 3600
  local m = (seg % 3600) // 60
  local s = seg % 60
  if d > 0 then return string.format("%d d %d h %d min", d, h, m) end
  if h > 0 then return string.format("%d h %d min", h, m) end
  if m > 0 then return string.format("%d min %d s", m, s) end
  return s .. " s"
end

-- ---------------------------------------------------------------
-- Piezas de dibujo
-- ---------------------------------------------------------------
local y = 0   -- el cursor vertical del dibujado

local function titulo(texto)
  y = y + 6
  gui.poner_fuente(f_titulo)
  gui.texto(12, y, texto, C_TITULO)
  y = y + 20
  gui.rect(12, y, ancho_v - 24, 1, C_LINEA)
  y = y + 7
end

-- Una fila de "etiqueta ...... valor"
local function dato(etiqueta, valor, color)
  gui.poner_fuente(f_normal)
  gui.texto(20, y, etiqueta, C_SUAVE)
  gui.poner_fuente(f_fuerte)
  gui.texto(COL_VALOR, y, valor, color or C_TEXTO)
  y = y + 19
end

-- Barra de ocupacion con su texto encima. 'usado' y 'total' en bytes
-- (o en lo que sea, mientras sean la misma unidad).
local function barra(etiqueta, usado, total, color, extra)
  local pct = (total and total > 0) and (usado * 100 / total) or 0
  gui.poner_fuente(f_normal)
  gui.texto(20, y, etiqueta, C_SUAVE)
  gui.poner_fuente(f_fuerte)
  local texto = string.format("%s de %s", bytes(usado), bytes(total))
  if extra then texto = texto .. "   " .. extra end
  gui.texto(COL_VALOR, y, texto, C_TEXTO)
  gui.poner_fuente(f_menuda)
  local pt = string.format("%.0f%%", pct)
  gui.texto(ancho_v - 24 - gui.medir_texto(pt), y + 1, pt, pct >= 90 and C_AVISO or C_SUAVE)
  y = y + 18

  local x0, an = 20, ancho_v - 44
  gui.rect(x0, y, an, 9, C_HUECO)
  local lleno = math.floor(an * pct / 100 + 0.5)
  if lleno > an then lleno = an end
  if lleno > 0 then gui.rect(x0, y, lleno, 9, pct >= 90 and C_AVISO or color) end
  y = y + 16
end

-- ---------------------------------------------------------------
-- Las tres secciones y las tareas
-- ---------------------------------------------------------------
local function pintar_equipo()
  titulo("Equipo")
  local nucleos, mhz = sistema.cpu_datos()
  dato("Procesador", sistema.cpu_nombre())
  dato("Núcleos", string.format("%d en marcha", nucleos))
  -- 0 MHz no significa "parado": significa que no hay a quien
  -- preguntarselo. En QEMU no existe el mailbox del firmware.
  dato("Frecuencia", (mhz and mhz > 0) and (mhz .. " MHz") or "desconocida")
  dato("RAM física", bytes(sistema.ram_fisica()))
  local pw, ph = sistema.pantalla()
  dato("Pantalla", string.format("%d x %d", pw, ph))
  dato("Encendido", duracion(sistema.encendido_segundos()))
  local t = gui.fecha_hora()
  dato("Fecha y hora", string.format("%02d/%02d/%d  %02d:%02d:%02d",
    t.dia, t.mes, t.anio, t.hora, t.minuto, t.segundo))
end

local function pintar_memoria()
  titulo("Memoria")
  local ram = sistema.ram_fisica()
  local nucleo = sistema.ram_nucleo()
  local usado_t, total_t = sistema.memoria_de_tareas()
  local usado_h, total_h = sistema.heap_kernel()

  barra("Tareas", usado_t, total_t, C_BARRA_SIS)
  barra("Montón del núcleo", usado_h, total_h, C_BARRA_SIS)
  dato("Núcleo en RAM", bytes(nucleo))
  -- Lo que queda es RAM que existe y que el sistema todavia no reparte
  -- a nadie. Se dice asi, y no "libre", porque no hay un asignador
  -- general que la vaya a usar: hoy sobra, sin mas.
  local repartida = (nucleo or 0) + (total_t or 0)
  local resto = (ram or 0) - repartida
  if resto < 0 then resto = 0 end
  dato("Sin repartir", bytes(resto))
end

local function pintar_disco()
  titulo("Disco")
  local total_b, usado_b = sistema.uso_disco()
  local libre = (total_b - usado_b) * 512
  barra("NemoFS (C:)", usado_b * 512, total_b * 512, C_BARRA_DIS, bytes(libre) .. " libres")
  dato("Tarjeta", bytes(sistema.tarjeta_bytes()))
  local parts = sistema.particiones()
  local resumen = {}
  for _, p in ipairs(parts) do
    resumen[#resumen + 1] = string.format("%d: %s", p.indice + 1, bytes(p.sectores * 512))
  end
  dato("Particiones", #resumen > 0 and table.concat(resumen, "   ") or "sin tabla utilizable")
end

local function pintar_tareas()
  local tareas = sistema.listar_tareas()
  local vivas, huecos = sistema.bloques_de_tarea()
  titulo(string.format("Tareas (%d de %d huecos)", vivas, huecos))

  local suma = 0
  for _, t in ipairs(tareas) do suma = suma + t.turnos end

  gui.poner_fuente(f_menuda)
  gui.texto(20, y, "El reparto es de TURNOS del planificador, no de procesador: una tarea", C_SUAVE)
  y = y + 14
  gui.texto(20, y, "que sólo espera acumula turnos igual que una que trabaja.", C_SUAVE)
  y = y + 18

  local x0, an = COL_VALOR, ancho_v - COL_VALOR - 44
  for _, t in ipairs(tareas) do
    -- El aviso de que faltan tareas tiene que caber ENTERO, asi que se
    -- reserva su propia linea: si no, quedaba cortado por abajo justo
    -- cuando es cuando hace falta leerlo.
    if y > alto_v - 34 then
      gui.poner_fuente(f_menuda)
      gui.texto(20, y, "(hay más tareas: agranda la ventana para verlas)", C_SUAVE)
      break
    end
    local pct = (suma > 0) and (t.turnos * 100 / suma) or 0
    gui.poner_fuente(f_normal)
    gui.texto(20, y, string.format("[%d] %s", t.slot, t.nombre), C_TEXTO)
    gui.rect(x0, y + 4, an, 8, C_HUECO)
    local lleno = math.floor(an * pct / 100 + 0.5)
    if lleno > 0 then gui.rect(x0, y + 4, lleno, 8, C_BARRA_TAR) end
    gui.poner_fuente(f_menuda)
    local pt = string.format("%.0f%%", pct)
    gui.texto(ancho_v - 24 - gui.medir_texto(pt), y + 1, pt, C_SUAVE)
    y = y + 18
  end
end

local function pintar()
  gui.limpiar(C_FONDO)
  y = 4
  pintar_equipo()
  pintar_memoria()
  pintar_disco()
  pintar_tareas()
  gui.poner_fuente(0)
end

gui.crear_temporizador(1)   -- 1 Hz
pintar()

gui.bucle(function(ev)
  local cambia, w, h = gui.tamano_cambiado()
  if cambia and w and w >= 200 then
    ancho_v, alto_v = w, h
    pintar()
  end
  if ev == gui.EVENT_TIMERTICK then pintar() end
end)
