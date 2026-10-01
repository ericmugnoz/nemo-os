# Guía de GPIO de Nemo OS

Los pines GPIO del conector de 40 patillas de la Raspberry Pi 4, desde
Lua y desde Nemo Basic: encender un LED, leer un pulsador, controlar lo
que quieras conectar.

---

## 1. Antes de conectar nada

La Pi no tiene protecciones en sus pines. Un solo cable mal puesto
puede estropearla, así que tres reglas:

- **Los pines trabajan a 3,3 V.** Nunca conectes **5 V** a un pin GPIO,
  ni directamente ni a través de un sensor pensado para 5 V: se quema.
- **Poca corriente**: como mucho unos **16 mA por pin**, y unos 50 mA
  entre todos. Un LED siempre **con su resistencia** (330 Ω va bien);
  un motor o un relé, nunca directamente: con un transistor o un
  módulo de relé.
- **Conecta con la Pi apagada**, y repasa el montaje antes de encender.

---

## 2. Qué pines se pueden usar

Nemo OS usa la **numeración BCM** —la de "GPIO17"—, no el número de
patilla del conector. Se pueden usar del **GPIO 2 al GPIO 27**, salvo
el **14 y el 15, reservados**: son la terminal (la UART), y
reconfigurarlos la dejaría muda.

| Patilla | Qué es | | Patilla | Qué es |
|---|---|---|---|---|
| 1 | 3,3 V | | 2 | 5 V |
| 3 | GPIO 2 | | 4 | 5 V |
| 5 | GPIO 3 | | 6 | masa (GND) |
| 7 | **GPIO 4** | | 8 | GPIO 14 (terminal) |
| 9 | masa (GND) | | 10 | GPIO 15 (terminal) |
| 11 | **GPIO 17** | | 12 | GPIO 18 |
| 13 | GPIO 27 | | 14 | masa (GND) |
| 15 | GPIO 22 | | 16 | GPIO 23 |
| 17 | 3,3 V | | 18 | GPIO 24 |
| 19 | GPIO 10 | | 20 | masa (GND) |
| 21 | GPIO 9 | | 22 | GPIO 25 |
| 23 | GPIO 11 | | 24 | GPIO 8 |
| 25 | masa (GND) | | 26 | GPIO 7 |

(Las patillas 27 y 28 son GPIO 0 y 1, que las placas de expansión usan
para identificarse: no están disponibles. De la 29 a la 40: GPIO 5, 6,
12, 13, 16, 19, 20, 21 y 26, más masas.) La patilla 1 es la que tiene la marca
cuadrada en la placa, en la esquina más cercana a la tarjeta SD.

---

## 3. Los cuatro modos

| Modo | Número | Para qué |
|---|---|---|
| Entrada | 0 | Leer un pin. Al aire lee valores al azar: úsalo solo si lo que conectas marca 0 y 1 por sí mismo |
| Salida | 1 | Poner el pin a 1 (3,3 V) o a 0 (masa). Empieza a 0 al configurarlo |
| Entrada a positivo | 2 | Entrada con resistencia interna a 3,3 V: al aire lee **1**. Para un pulsador conectado a masa (pulsado lee 0) |
| Entrada a masa | 3 | Entrada con resistencia interna a masa: al aire lee **0**. Para un pulsador conectado a 3,3 V (pulsado lee 1) |

**Cada pin tiene dueño**: el primer programa que lo configura se lo
queda, y otro programa no puede cambiarlo ni escribir en él mientras el
primero siga abierto (leerlo, sí). **Al cerrar un programa, sus pines
vuelven a entrada**: un LED no se queda encendido cuando se cierra lo
que lo controlaba.

En **QEMU** no hay pines: se **simulan**. Cada cambio aparece en la
terminal (`gpio (simulado): pin 17 = 1`), una salida lee lo último que
escribiste y una entrada lee lo que marcaría su resistencia. Sirve para
probar un programa antes de conectar nada.

---

## 4. Un LED que parpadea

**Montaje**: GPIO 17 (patilla 11) → resistencia de 330 Ω → pata
**larga** del LED; pata corta del LED → masa (patilla 9).

**En Lua**:

```lua
local gpio = require("nemo_gpio")
local sistema = require("nemo_sistema")

gpio.modo(17, gpio.SALIDA)
for i = 1, 10 do
  gpio.escribir(17, 1)
  sistema.esperar(500)
  gpio.escribir(17, 0)
  sistema.esperar(500)
end
```

**En Nemo Basic**:

```basic
Console
GpioMode 17, 1
For i = 1 To 10
  GpioWrite 17, 1
  Delay 500
  GpioWrite 17, 0
  Delay 500
Next
Print "hecho"
```

---

## 5. Un pulsador

**Montaje**: un pulsador entre GPIO 4 (patilla 7) y masa (patilla 9).
Nada más: la resistencia la pone la propia Pi, con el modo "entrada a
positivo". Sin pulsar lee 1; pulsado, 0.

**En Lua**, encendiendo el LED de antes mientras se pulsa:

```lua
local gpio = require("nemo_gpio")
local sistema = require("nemo_sistema")

gpio.modo(4, gpio.ARRIBA)
gpio.modo(17, gpio.SALIDA)
while true do
  gpio.escribir(17, gpio.leer(4) == 0 and 1 or 0)
  sistema.esperar(20)          -- ceder el turno: 50 lecturas por segundo
end
```

**En Nemo Basic**:

```basic
Console
GpioMode 4, 2
GpioMode 17, 1
While True
  If GpioRead(4) = 0 Then GpioWrite 17, 1 Else GpioWrite 17, 0
  Delay 20
Wend
```

Un bucle así necesita siempre su `esperar`/`Delay` (o `Pump`): sin él,
el programa no cede nunca el turno.

---

## 6. PWM: brillo, velocidad y servos

Con **PWM** un pin se enciende y se apaga muy deprisa, siempre al mismo
ritmo: lo que cambia es **qué parte del tiempo** está encendido. Un LED
al 25 % brilla poco; un servo lee la duración de cada pulso como una
posición.

Nemo OS usa el **PWM por hardware** de la Pi: la señal la genera el
chip, con precisión de un microsegundo, aunque el programa esté ocupado.
Solo existe en cuatro pines, con **dos canales**: el **GPIO 12 y el 18**
comparten uno, y el **13 y el 19**, el otro. Se pueden usar dos a la
vez, uno de cada pareja.

> **El sonido usa el mismo reloj.** El jack de 3,5 mm de la Pi 4 también
> va por PWM, en otro bloque del chip pero **colgando del mismo reloj**
> que estos pines. Por eso Nemo OS deja ese reloj fijo en **27 MHz** y
> calcula el resto a partir de ahí: así el PWM de un pin y el sonido
> conviven sin pisarse. Si tocas el divisor del reloj en `gpio.c`,
> desafinas el audio — en Linux ese mismo fallo se oye como un siseo
> agudo en el jack en cuanto alguien activa el PWM de un pin.

### Un LED que se enciende y se apaga suave

**Montaje**: el de la sección 4, pero en el GPIO 18 (patilla 12).

```lua
local gpio = require("nemo_gpio")
local sistema = require("nemo_sistema")

for vuelta = 1, 3 do
  for p = 0, 100, 2 do gpio.pwm(18, 1000, p); sistema.esperar(20) end
  for p = 100, 0, -2 do gpio.pwm(18, 1000, p); sistema.esperar(20) end
end
gpio.modo(18, gpio.ENTRADA)      -- parar el PWM
```

### Un servo

**Montaje**: el cable de señal (amarillo o naranja) al GPIO 18
(patilla 12); el rojo a 5 V (patilla 2); el marrón o negro a masa
(patilla 6). Un servo pequeño (tipo SG90) se puede alimentar así; uno
más grande necesita **su propia fuente**, con la masa unida a la de la
Pi, o hará que la Pi se reinicie al moverse.

```lua
local gpio = require("nemo_gpio")
local sistema = require("nemo_sistema")

for _, grados in ipairs({ 0, 90, 180, 90 }) do
  gpio.servo(18, grados)
  sistema.esperar(1000)
end
```

`gpio.servo(pin, grados)` usa pulsos de 0,5 ms (0°) a 2,5 ms (180°) a
50 Hz, lo más habitual. Si tu servo no llega a los extremos o se fuerza
en ellos, usa sus límites reales: `gpio.servo(18, 90, 1, 2)` para uno
de 1 a 2 ms.

### En Nemo Basic

`GpioPwm pin, hz, ciclo`, con el ciclo en **diezmilésimas**: 0 apagado,
10000 siempre encendido, 2500 el 25 %. Para un servo a 50 Hz, 250 son
0,5 ms (0°), 750 son 1,5 ms (90°) y 1250 son 2,5 ms (180°).

```basic
Console
For b = 0 To 10000 Step 200
  GpioPwm 18, 1000, b
  Delay 20
Next
GpioPwm 18, 50, 750      ; un servo en el 18, a 90 grados
Delay 1000
GpioMode 18, 0           ; parar el PWM
```

---

## 7. I²C: sensores y pantallas con dos cables

**I²C** es un bus: varios aparatos comparten los mismos dos cables, y
cada uno tiene una **dirección** (un número de 7 bits, como `0x76`).
Casi todos los sensores pequeños (temperatura, humedad, presión, luz,
acelerómetros) y las pantallas OLED lo usan.

**Montaje**: cuatro cables, los mismos para todos los aparatos:

| Del aparato | A la Pi |
|---|---|
| VCC (o VIN) | 3,3 V (patilla 1) — **no a 5 V**, salvo que el módulo diga que los admite y baje él la tensión |
| GND | masa (patilla 9) |
| SDA | GPIO 2 (patilla 3) |
| SCL | GPIO 3 (patilla 5) |

Las resistencias que necesita el bus ya las trae la Pi en esos dos
pines. El bus va a **100 kHz**, la velocidad estándar. El primer
programa que lo usa se queda los pines 2 y 3 hasta que termina.

### Qué hay conectado

Lo primero, siempre: comprobar que el aparato contesta, y en qué
dirección.

```lua
local gpio = require("nemo_gpio")
local hay, e = gpio.i2c_buscar()
if not hay then print(e) else
  for _, d in ipairs(hay) do print(string.format("hay algo en 0x%02x", d)) end
end
```

Si no aparece nada, revisa los cables (SDA y SCL cruzados es lo más
habitual) y la alimentación.

### Leer y escribir registros

Casi todos los aparatos I²C funcionan igual: tienen **registros**
numerados; se escribe el número de registro, y luego se lee su valor o
se escribe uno nuevo. Un BMP280 o BME280 (presión y temperatura; el BME
también humedad) guarda su identificador en el registro `0xD0`:

```lua
local gpio = require("nemo_gpio")
local id, e = gpio.i2c_leer_registro(0x76, 0xD0)   -- 0x77 en algunos modulos
if not id then print(e)
elseif id[1] == 0x58 then print("es un BMP280")
elseif id[1] == 0x60 then print("es un BME280")
else print(string.format("identificador 0x%02x", id[1])) end
```

Para leer la temperatura o la presión de verdad hay que seguir la hoja
de datos de cada sensor (qué registros, en qué orden, qué cuentas):
cada aparato es distinto, pero todos se leen con estas funciones.

### Probarlo en QEMU: la EEPROM simulada

En QEMU no hay bus, pero Nemo OS simula una **memoria EEPROM** de 256
bytes en la dirección `0x50`, como las 24C02: el primer byte de una
escritura fija la posición, los siguientes se guardan; una lectura
devuelve desde la posición. Cualquier otra dirección no contesta, como
en un bus vacío. Cada transferencia aparece en la terminal.

```lua
local gpio = require("nemo_gpio")
gpio.i2c_escribir_registro(0x50, 16, { 72, 111, 108, 97 })   -- "Hola" en la posicion 16
local b = gpio.i2c_leer_registro(0x50, 16, 4)
print(string.char(table.unpack(b)))                          -- Hola
```

### En Nemo Basic

Los bytes van en **cadenas**: `Chr$(n)` para construirlas y
`Asc(Mid$(s$, i, 1))` para sacarlos. `I2cWrite dir, datos$` envía;
`I2cRead$(dir, n)` devuelve `n` bytes (o una cadena vacía si algo
falla). Las direcciones, en decimal: `0x50` es 80, `0x76` es 118.

```basic
Console
r = I2cWrite(80, Chr$(16) + "Hola")     ; "Hola" en la posicion 16 de la EEPROM
I2cWrite 80, Chr$(16)                   ; volver a la posicion 16...
s$ = I2cRead$(80, 4)                    ; ...y leer 4 bytes
Print "leido: " + s$
For i = 1 To Len(s$)
  Print Str$(Asc(Mid$(s$, i, 1)))
Next
```

---

## 8. SPI: más rápido, y entradas analógicas

**SPI** es otro bus, más rápido que I²C (varios MHz) y en las dos
direcciones a la vez: por cada byte que sale, entra otro. En lugar de
direcciones, cada aparato tiene su propio cable de **selección**. Lo
usan pantallas a color, lectores de tarjetas y **conversores
analógicos**.

| Señal | GPIO | Patilla |
|---|---|---|
| Reloj (SCLK) | 11 | 23 |
| MOSI (sale de la Pi) | 10 | 19 |
| MISO (entra a la Pi) | 9 | 21 |
| Selector CE0 (aparato 0) | 8 | 24 |
| Selector CE1 (aparato 1) | 7 | 26 |

Cada aparato funciona en uno de **cuatro modos** (cómo se comporta el
reloj) y hasta una **velocidad** máxima: los dos los dice su hoja de
datos. Nemo OS admite de 100 kHz a 50 MHz, y hasta 4096 bytes por
transferencia.

### Leer una tensión: potenciómetros y sensores analógicos

La Pi **no puede leer tensiones**, solo 0 y 1. Para un potenciómetro, un
sensor de luz (LDR) o cualquier sensor analógico hace falta un
conversor. El más usado es el **MCP3008**: 8 entradas, de 0 a 1023.

**Montaje** (patillas del MCP3008, que es un chip de 16 patillas; la 1
está junto a la muesca):

| MCP3008 | A |
|---|---|
| 16 (VDD) y 15 (VREF) | 3,3 V (patilla 1 de la Pi) |
| 14 (AGND) y 9 (DGND) | masa (patilla 6) |
| 13 (CLK) | GPIO 11 (patilla 23) |
| 12 (DOUT) | GPIO 9, MISO (patilla 21) |
| 11 (DIN) | GPIO 10, MOSI (patilla 19) |
| 10 (CS) | GPIO 8, CE0 (patilla 24) |
| 1 (CH0) | la pata central de un potenciómetro; las otras dos, a 3,3 V y a masa |

**En Lua**, `gpio.mcp3008(canal)` hace todo:

```lua
local gpio = require("nemo_gpio")
local sistema = require("nemo_sistema")
for i = 1, 20 do
  local v, e = gpio.mcp3008(0)
  if not v then print(e) break end
  print(string.format("canal 0: %4d  (%.2f V)", v, v * 3.3 / 1023))
  sistema.esperar(500)
end
```

Por dentro es una transferencia de tres bytes: `gpio.spi({1, 0x80 |
canal << 4, 0})`, y el valor llega en los dos últimos. Para otros
aparatos, `gpio.spi(bytes, { chip = 0, hz = 1000000, modo = 0 })`
envía y devuelve lo recibido.

**En QEMU** se simulan dos aparatos: un MCP3008 en CE0, cuyos canales
dan valores fijos (canal × 128 + 37: 37, 165, 293…) para comprobar un
programa, y un "espejo" en CE1, que devuelve lo mismo que recibe.

### En Nemo Basic

`SpiTransfer$(datos$, chip, hz, modo)` envía los bytes de la cadena y
devuelve los recibidos, en otra cadena de la misma longitud (vacía si
algo falla). Para el canal `n` del MCP3008 se envía `Chr$(1) +
Chr$(128 + n * 16) + Chr$(0)`:

```basic
Console
For i = 1 To 20
  r$ = SpiTransfer$(Chr$(1) + Chr$(128) + Chr$(0), 0, 1000000, 0)
  v = (Asc(Mid$(r$, 2, 1)) Mod 4) * 256 + Asc(Mid$(r$, 3, 1))
  Print "canal 0: " + Str$(v)
  Delay 500
Next
```

---

## 9. Referencia

**Lua** (`nemo_gpio`, en SISTEMA). Cada función devuelve su resultado,
o `nil` y un mensaje si falla:

| Función | Qué hace |
|---|---|
| `gpio.modo(pin, modo)` | Configura el pin: `gpio.ENTRADA`, `gpio.SALIDA`, `gpio.ARRIBA`, `gpio.ABAJO` |
| `gpio.escribir(pin, valor)` | Pone una salida a 1 o a 0 (valen `true`/`false`) |
| `gpio.leer(pin)` | 1 o 0. Cualquier programa puede leer cualquier pin |
| `gpio.alternar(pin)` | Invierte una salida; devuelve el valor nuevo |
| `gpio.pwm(pin, hz, porcentaje)` | PWM por hardware (pines 12, 13, 18, 19), de 10 Hz a 100 kHz; porcentaje de 0 a 100, con decimales |
| `gpio.servo(pin, grados, [min_ms, max_ms])` | Un servo: de 0 a 180 grados (pulsos de 0,5 a 2,5 ms si no se dice otra cosa) |
| `gpio.i2c_buscar()` | Lista de direcciones I²C donde hay algo conectado |
| `gpio.i2c_escribir(dir, bytes)` | Envía bytes (tabla de números o cadena); devuelve cuántos |
| `gpio.i2c_leer(dir, n)` | Lee `n` bytes: tabla de números |
| `gpio.i2c_leer_registro(dir, reg, [n])` | Escribe el número de registro y lee `n` bytes (1 si no se dice) |
| `gpio.i2c_escribir_registro(dir, reg, valores)` | Escribe un número o una tabla de números a partir de un registro |
| `gpio.spi(bytes, [opciones])` | Transferencia SPI: envía los bytes y devuelve los recibidos. Opciones: `chip` (0/1), `hz`, `modo` (0-3) |
| `gpio.mcp3008(canal, [chip])` | Lee una entrada de un conversor MCP3008: de 0 a 1023 |

**Nemo Basic**:

| Orden | Qué hace |
|---|---|
| `GpioMode pin, modo` | Configura el pin (0 entrada, 1 salida, 2 entrada a positivo, 3 entrada a masa) |
| `GpioWrite pin, valor` | Pone una salida a 1 o a 0 |
| `GpioRead(pin)` | Devuelve 1 o 0 |
| `GpioPwm pin, hz, ciclo` | PWM por hardware (pines 12, 13, 18, 19); ciclo en diezmilésimas (0 a 10000) |
| `I2cWrite dir, datos$` | Envía los bytes de la cadena por I²C; devuelve cuántos |
| `I2cRead$(dir, n)` | Lee `n` bytes por I²C, en una cadena (vacía si falla) |
| `SpiTransfer$(datos$, chip, hz, modo)` | Transferencia SPI: devuelve los bytes recibidos (vacía si falla) |

Las tres devuelven un número negativo si algo falla: **−1** pin fuera
del 2-27 o modo desconocido, **−2** pin reservado (14 o 15), **−3** el
pin es de otro programa (o no lo has configurado antes de escribir),
**−4** el pin no es una salida, **−5** el canal de PWM ya lo usa el otro
pin de su pareja, **−6** nadie contesta en esa dirección I²C, **−7** el
bus I²C no terminó (un aparato lo retiene, o está mal conectado).

**Syscalls** (para cualquier lenguaje): `264` modo (pin, modo), `265`
escribir (pin, valor), `266` leer (pin), `267` PWM (pin, hz, ciclo en
diezmilésimas), `268` I²C escribir y `269` I²C leer (dirección,
búfer, bytes: de 1 a 256), y `270` SPI (aparato, bytes a enviar,
cuántos, hz, dónde recibir o 0, modo: seis argumentos, `x0` a `x5`).
Para parar un PWM, `264` con modo 0.

**El panel GPIO** (`gpio.lua`, en Accesorios) muestra los 26 pines:
un clic en el modo lo va cambiando, un clic en el círculo de una salida
la enciende o la apaga, y las entradas se leen en directo. Es la forma
más rápida de comprobar un montaje.

---

## 10. Lo que viene

I²C con parada repetida (algún sensor la exige) y a 400 kHz, SPI con
transferencias más largas para pantallas, y avisos por cambio de pin,
para no tener que leer en bucle.
