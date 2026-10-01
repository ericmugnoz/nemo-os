-- prueba_sonido.lua -- Nemo OS
--
-- Comprueba el sonido de punta a punta: fabrica un WAV, lo guarda, se lo
-- da al kernel y lo reproduce. En la Pi 4 sale por el jack de 3,5 mm
-- (PWM1, pines 40 y 41); en QEMU, por virtio-sound.
--
-- Va paso a paso y dice en cual se queda, porque "no suena nada" puede ser
-- el archivo, el cargador, el driver o el cable, y son arreglos distintos.
--
--   run prueba_sonido.lua

local SYS_FILE_OPEN   = 20
local SYS_FILE_WRITE  = 22
local SYS_LOAD_SOUND  = 226
local SYS_FREE_SOUND  = 227
local SYS_PLAY_SOUND  = 228
local VOL_NEMOFS      = 0
local RAIZ            = 0

local ARCHIVO = "TONO.WAV"
local HZ      = 11025      -- el kernel lo remuestrea a 44100 al cargarlo
local SEGUNDOS = 0.6

-- Una cadena de Lua no es un puntero que una syscall pueda leer: hay que
-- copiarla a un buffer de verdad, de cuatro en cuatro bytes.
local function buffer_de(s)
  local b = nemo.buffer(#s + 4)              -- +4: el ultimo entero y el cero final
  for i = 1, #s, 4 do
    local v = 0
    for k = 0, 3 do
      local c = s:byte(i + k) or 0
      v = v | (c << (k * 8))
    end
    nemo.escribir_i32(b, i - 1, v)
  end
  return b
end

-- Un arpegio corto: tres notas, con una entrada y una salida suaves para
-- que no empiece ni acabe con un chasquido.
local function muestras()
  local notas = { 440, 554, 659 }            -- la, do#, mi
  local total = math.floor(HZ * SEGUNDOS)
  local por_nota = total // #notas
  local t = {}
  for i = 0, total - 1 do
    local nota = notas[math.min(#notas, i // por_nota + 1)]
    local dentro = i % por_nota
    local v = math.sin(2 * math.pi * nota * i / HZ)
    -- rampa de 3 ms a la entrada y a la salida de cada nota
    local rampa = math.floor(HZ * 0.003)
    if dentro < rampa then v = v * (dentro / rampa) end
    -- el -1 hace que la ultima muestra caiga en cero exacto, no cerca
    if dentro > por_nota - rampa then v = v * ((por_nota - dentro - 1) / rampa) end
    t[#t + 1] = math.floor(v * 12000)        -- ~37% de la escala: sitio de sobra
  end
  return t
end

-- Cabecera WAV de 44 bytes, PCM de 16 bits, un canal.
local function wav(pcm)
  local datos = #pcm * 2
  local function le32(v) return string.char(v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >> 24) & 255) end
  local function le16(v) return string.char(v & 255, (v >> 8) & 255) end
  local trozos = {
    "RIFF", le32(36 + datos), "WAVE",
    "fmt ", le32(16), le16(1), le16(1), le32(HZ), le32(HZ * 2), le16(2), le16(16),
    "data", le32(datos),
  }
  for i = 1, #pcm do
    local s = pcm[i]
    if s < 0 then s = s + 65536 end
    trozos[#trozos + 1] = le16(s)
  end
  return table.concat(trozos)
end

print("1. fabricando el tono...")
local bytes = wav(muestras())
print("   " .. #bytes .. " bytes, " .. HZ .. " Hz, un canal")

print("2. guardando " .. ARCHIVO .. "...")
local nombre = buffer_de(ARCHIVO)
local id = nemo.syscall(SYS_FILE_OPEN, nemo.direccion(nombre), RAIZ, VOL_NEMOFS)
if id < 0 then print("   FALLO: no se pudo crear el archivo"); return end

local datos = nemo.buffer(#bytes + 4)
for i = 1, #bytes, 4 do
  local v = 0
  for k = 0, 3 do
    local c = bytes:byte(i + k) or 0
    v = v | (c << (k * 8))
  end
  nemo.escribir_i32(datos, i - 1, v)
end
local escrito = nemo.syscall(SYS_FILE_WRITE, id, nemo.direccion(datos), #bytes, VOL_NEMOFS)
if escrito < 0 then print("   FALLO: no se pudo escribir"); return end
print("   guardado")

print("3. cargandolo en el kernel...")
local h = nemo.syscall(SYS_LOAD_SOUND, nemo.direccion(nombre))
if h < 0 then
  print("   FALLO: LoadSound devolvio -1.")
  print("   El archivo esta, asi que o el WAV no le gusta al cargador,")
  print("   o no quedan huecos de sonido libres.")
  return
end
print("   cargado, hueco " .. h)

print("4. sonando (bloquea hasta que termina)...")
nemo.syscall(SYS_PLAY_SOUND, h)
nemo.syscall(SYS_FREE_SOUND, h)
print("   listo.")
print("")
print("Si no has oido nada pero han salido los cuatro pasos, el problema")
print("esta en el driver o en el cable, no en el camino del archivo.")
print("En la Pi 4: los auriculares van al jack de 3,5 mm de la placa.")
