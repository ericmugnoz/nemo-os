# Guía de red de Nemo OS — Raspberry Pi 4 y QEMU

Esta guía cubre qué hay en la pila de red, cómo conectarse desde otro
ordenador, cómo probarlo en QEMU sin la placa, qué hace la shell remota,
dónde está cada cosa en el código, y qué hacer cuando algo no conecta.

---

## 1. Qué hay (y qué no)

La pila de red de Nemo OS es deliberadamente mínima: lo que hace falta
para administrar la placa desde otro ordenador, documentado, y nada
más. Es la misma filosofía que Lua en vez de Python o NIMG en vez de
PNG.

**Lo que hay:**

| Capa | Qué hace | Archivo |
|---|---|---|
| Ethernet (Pi 4) | Driver del controlador GENET integrado, a 10/100/1000 Mbps | `src/pi4/genet_pi4.c` |
| Ethernet (QEMU) | Driver de tarjeta virtio-net | `src/virtio_net.c` |
| ARP | Responde "¿quién tiene esta IP?" con la MAC de la placa | `src/net.c` |
| IPv4 | Recibe paquetes dirigidos a la IP de la placa, checksum incluido | `src/net.c` |
| ICMP | Responde a `ping` | `src/net.c` |
| TCP | Conexiones entrantes, una a la vez | `src/tcp.c` |
| Shell remota | Comandos de disco, sistema y lanzamiento de programas, por TCP | `src/netshell.c` |

**Lo que NO hay, a propósito:**

- **DHCP.** La IP es fija (ver §2). Un cliente DHCP es otro protocolo.
- **Retransmisión TCP.** Es un cable directo, sin pérdida esperable. Si un
  paquete se pierde, la conexión queda colgada y hay que reconectar.
- **Varias conexiones TCP a la vez.** Una sola. De sobra para una shell.
- **Conexiones salientes.** Nemo OS solo responde; no inicia nada (salvo
  un anuncio ARP al arrancar).
- **UDP, DNS, HTTP, HTTPS.** Navegar internet queda fuera de alcance.
- **Cifrado.** La shell remota es texto plano, para la red de casa. No es
  SSH ni telnet (no negocia opciones).
- **Interrupciones.** Todo es sondeo, una vez por vuelta del sistema,
  igual que el teclado y el ratón.

---

## 2. Direcciones

Nemo OS pide su dirección por DHCP. Cuando nadie la reparte —un cable
directo entre dos máquinas, sin router— usa una **dirección de reserva**:

| Dónde | Dirección de reserva | Por qué esa |
|---|---|---|
| Raspberry Pi 4 | `169.254.x.y`, calculada a partir de la MAC | Rango *link-local* (RFC 3927): el que macOS se autoasigna en una interfaz Ethernet sin DHCP. Funciona con un cable directo Pi–Mac. Se calcula en vez de ser fija para que **dos placas unidas por un cable no cojan la misma**. |
| QEMU | `10.0.2.15` | La subred de la red de usuario de QEMU (slirp) es `10.0.2.0/24` y espera al invitado en esa dirección; ahí reenvía `hostfwd`. Aquí es fija por eso mismo. |

El arranque dice cuál le ha tocado:

```
net: reserva = 169.254.103.188 (sacada de la MAC b8:27:eb:11:22:33)
```

Esa es la dirección a la que hay que llamar desde el otro ordenador. Con
router no aparece esta línea: el DHCP da la dirección y el arranque la
anuncia igual.

El puerto de la shell remota es **2323** en los dos casos (`NET_PUERTO_SHELL`
en `src/net.h`). No es el 23 de telnet a propósito: no es telnet, y así
no choca con nada del Mac.

La **MAC** se lee de fábrica: en la Pi, del firmware por el mailbox
(`mailbox_get_mac_pi4()`); en QEMU, del espacio de configuración de la
tarjeta virtio.

Para cambiar la IP, el único sitio es `mi_ip` en `src/net.c`.

---

## 3. Conectarse a la Raspberry Pi 4

### 3.1 Montaje

Cable Ethernet directo entre la Pi 4 y el Mac (o a través de un switch;
las dos cosas funcionan). No hace falta router ni internet.

### 3.2 Configurar el Mac (una sola vez)

macOS se autoasigna una IP link-local si no encuentra DHCP, pero tarda
un rato en hacerlo cada vez que el enlace se levanta — y el enlace se
levanta cada vez que la Pi arranca. Con una IP fija la ruta es
inmediata y determinista:

1. **Ajustes del Sistema → Red** → el adaptador Ethernet que va a la Pi.
2. **Detalles → TCP/IP → Configurar IPv4: Manualmente**
3. Dirección IP `169.254.100.1`, máscara `255.255.0.0`, router **vacío**.
4. Aceptar.

### 3.3 Comprobar

Con la Pi arrancada (escritorio visible):

```bash
ping 169.254.103.188      # la que diga el arranque
```

Debería responder en 5–20 ms. Si no, ver §7.

### 3.4 Abrir la shell remota

```bash
nc -v 169.254.103.188 2323      # la que diga el arranque
```

Siempre con `-v`: sin él, `nc` no dice si conectó o no. Debería aparecer:

```
Connection to 169.254.103.188 port 2323 [tcp/*] succeeded!

Nemo OS -- shell remota (Raspberry Pi 4)
'help' para ver los comandos.

nemo:/>
```

> **Tras cada arranque de la Pi, el primer `nc` puede tardar un par de
> segundos** en decir `succeeded!` — o parecer que no conecta si se
> corta antes. Es normal (ver §7.2). Esperar, o cortar y repetir.

Para salir: `exit` (cierra limpiamente) o Ctrl+C.

---

## 4. Probar en QEMU (sin la placa)

La misma pila corre en QEMU sobre una tarjeta virtio-net. Es la forma
de probar cambios de red sin encender la Pi.

```bash
make run
```

`make run` ya arranca QEMU con la tarjeta y el puerto 2323 redirigido
al host. En la UART debe aparecer:

```
virtio-net: tarjeta lista, MAC = 52:54:00:12:34:56
net: listo, IP = 10.0.2.15, MAC = 52:54:00:12:34:56
tcp: escuchando en el puerto 2323
```

Y en otra terminal del Mac:

```bash
nc -v localhost 2323
```

Sale el mismo banner y el mismo prompt. Todo lo de §5 funciona igual.

**Limitación de QEMU:** su red de usuario (slirp) no reenvía ICMP. El
`ping` al invitado **no funciona** en QEMU; TCP sí. No es un fallo de
Nemo OS.

Si QEMU arranca sin la tarjeta (`virtio-net: no hay tarjeta de red en la
maquina virtual`), falta `-device virtio-net-device` en el comando —
compruébalo en el objetivo `run` del `Makefile`.

---

## 5. La shell remota

Comandos disponibles (`help` los lista):

| Comando | Qué hace |
|---|---|
| `ls` (o `dir`) | Lista la carpeta actual: `[DIR]` para carpetas, tamaño para archivos. Hasta 64 entradas. |
| `cd <carpeta>` | Entra en una carpeta. `cd ..` sube, `cd /` vuelve a la raíz. Hasta 8 niveles. |
| `pwd` | Muestra la carpeta actual. |
| `cat <archivo>` (o `type`) | Muestra un archivo de texto. Se corta a 900 bytes y lo avisa. Los bytes no imprimibles salen como `.`. |
| `run <programa> [argumento]` | Lanza un programa (`.pro` o `.lua`) **en el escritorio de la Pi**. Busca en la carpeta actual, luego en `PROGRAMAS` y `ACCESORIOS`. Ejemplo: `run reloj.lua`, `run editor.pro notas.txt`. |
| `disco` (o `df`) | Uso del disco NemoFS: usado, total, libre. |
| `mem` (o `free`) | Heap del kernel y bloques de tarea en uso. |
| `tareas` (o `ps`) | Tareas en marcha: hueco, turnos, nombre. |
| `exit` (o `quit`, `salir`) | Cierra la conexión. |

Detalles de comportamiento:

- Acepta `\n` (como manda `nc`) y `\r\n` (como manda `telnet`).
- Acepta retroceso.
- Acepta clientes que mandan byte a byte: no responde hasta completar
  la línea.
- Cada conexión nueva empieza en la raíz, aunque la anterior estuviera
  en otra carpeta.
- **Toda la salida de un comando cabe en un segmento TCP (1024 bytes).**
  Por eso `cat` y `ls` se cortan. Es una consecuencia del diseño "un
  segmento entra, uno sale" (§6.3).

La shell corre **dentro del kernel**, en C. Es distinta de `shell.lua`
(la shell del escritorio, que corre como tarea) y tiene menos comandos
a propósito: es para administrar la placa desde fuera, no para trabajar
en ella.

---

## 6. Cómo está construido

### 6.1 Mapa de archivos

```
src/nic.h           La tarjeta de red vista desde arriba: nic_init, nic_link_up,
                    nic_get_mac, nic_send, nic_recv. Según el build resuelve a
                    genet_pi4.c (Pi 4) o virtio_net.c (QEMU, con -DNEMO_QEMU).
src/pi4/genet_pi4.c Driver del GENET (Ethernet integrado de la Pi 4).
src/pi4/genet_pi4.h
src/virtio_net.c    Driver virtio-net (QEMU).
src/virtio_net.h
src/net.c           ARP + IPv4 + ICMP. Despacha TCP a tcp.c. Anuncio ARP al arrancar.
src/net.h           NET_PUERTO_SHELL, net_init(), net_poll().
src/tcp.c           TCP mínimo. La aplicación son tres ganchos externos.
src/tcp.h
src/netshell.c      La shell remota: implementa los ganchos de tcp.h.
```

`net.c`, `tcp.c` y `netshell.c` no tocan ningún registro de hardware.
Por eso la misma pila corre en la Pi y en QEMU, y por eso se puede
probar entera en el host con un `nic` simulado (ver §8).

### 6.2 Dónde se engancha al kernel

- `kernel.c`: tras la entrada USB, `nic_init()`; si el enlace subió,
  `net_init()` (que envía el ARP gratuito y abre el puerto 2323).
- `tasks.c`, en `task_yield()`: `net_poll()` justo después de
  `input_poll()`. Es el "tick" del sistema: cada vez que una tarea cede
  el control, se sondea la red. Una trama por llamada.

Si no hay cable, o el enlace no sube, `nic_init()` devuelve `false`,
`net_init()` no se llama, `net_poll()` no hace nada, y el sistema
arranca igual. **La red nunca es un requisito para llegar al
escritorio.**

### 6.3 Un segmento entra, uno sale

`net_poll()` recibe como mucho una trama y envía como mucho una. Todo lo
de arriba está diseñado alrededor de eso:

- Una petición ARP → una respuesta ARP.
- Un ping → un pong.
- Un segmento TCP con datos → un segmento con el ACK **y** la respuesta
  de la aplicación en el mismo paquete (*piggyback*, como cualquier TCP
  real).
- El FIN del cliente → FIN+ACK en un solo segmento (nos saltamos
  CLOSE_WAIT: nunca hay nada pendiente que decir).
- Al establecerse la conexión (tercer paso del saludo) → el banner de la
  shell, como datos con ACK.

Es lo que limita la salida de un comando a una ventana TCP. Levantar
esa limitación exigiría una cola de envío — trabajo para otra fase.

### 6.4 El driver del GENET (Pi 4)

Transcrito registro a registro del driver de U-Boot para esta misma
placa (`drivers/net/bcmgenet.c`), que es la referencia mínima probada
que existe: sondeo, un solo anillo (el 16), sin bloques de estado. No
hay documentación oficial completa del GENET; inventar offsets no era
una opción.

Cosas que conviene saber si se toca:

- **Los descriptores DMA viven dentro del espacio de registros del
  chip**, no en RAM. Ya están mapeados como memoria de dispositivo por
  la MMU (sin caché). Solo los búferes de paquetes están en RAM normal y
  necesitan `dc cvac` antes de transmitir y `dc ivac` antes de leer —
  el mismo patrón que `xhci_pi4.c`.
- `CMD_SW_RESET` resetea el lado MAC del GENET, **no el PHY externo**
  (BCM54213PE). Sin un reset explícito del PHY (bit 15 de BMCR) y un
  anuncio explícito de capacidades (incluido Gigabit en `MII_CTRL1000`),
  el PHY se queda con la configuración del arranque anterior y no
  negocia con un ordenador conectado en directo. Esto costó una vuelta
  de pruebas.
- Los 16 bits altos de `RDMA_PROD_INDEX` son un contador de descartes,
  no parte del índice. Hay que enmascarar a 16 bits.
- La velocidad negociada se lee del registro 0x19 del PHY, específico de
  Broadcom. Con un PHY de otro fabricante, esa lectura es lo primero a
  revisar.

### 6.5 El driver virtio-net (QEMU)

Mismo patrón que `disk.c` (virtio-mmio moderno, `VERSION_1`): escaneo de
los 32 slots MMIO, negociación de features, colas virtqueue en RAM. Dos
colas (0 recepción, 1 transmisión). Cada trama va precedida de una
cabecera virtio-net de 12 bytes que aquí es toda ceros: se rechazan a
propósito todas las descargas (checksum, GSO, búferes fusionados) —
la pila lo calcula todo por software, igual que en la Pi. Dieciséis
búferes de recepción colgados de antemano. Sin mantenimiento de caché
(en QEMU el DMA es coherente con la CPU emulada).

### 6.6 TCP: simplificaciones conscientes

Están escritas en la cabecera de `src/tcp.c`. Resumen:

- Una conexión. Puerto cerrado o segunda conexión simultánea → RST.
- Sin retransmisión.
- Ventana fija de 1024 bytes. MSS 1460 anunciado en el SYN-ACK.
- Sin reordenación: un segmento fuera de secuencia se descarta y se
  vuelve a confirmar lo último recibido; el otro extremo (que sí tiene
  una pila completa) reenvía lo que falte.
- Un SYN nuevo con una conexión abierta la sustituye (un `nc` que se
  reinicia sin cerrar el anterior no deja el puerto colgado).
- Cierre pasivo (el cliente cierra) y activo (`exit`), este último
  saltándose TIME_WAIT.
- El checksum incluye la **pseudo-cabecera** (IPs, protocolo,
  longitud). Sin ella, el otro extremo descarta cada segmento en
  silencio. Es el error más traicionero de una pila TCP casera.

### 6.7 Separación protocolo / aplicación

`tcp.c` no sabe que sirve una shell. La aplicación son tres funciones
declaradas en `tcp.h` e implementadas fuera:

```c
uint32_t tcp_app_on_connect(uint8_t *salida, uint32_t max);
uint32_t tcp_app_on_data(const uint8_t *datos, uint32_t len,
                         uint8_t *salida, uint32_t max, bool *cerrar);
```

En el kernel las implementa `netshell.c`. En la batería de pruebas del
host, un eco. Para servir otra cosa por TCP (en vez de la shell), basta
con implementar esas dos funciones.

---

## 7. Cuando no conecta

### 7.1 Lo primero: la UART

La UART de la Pi (o la consola de QEMU) cuenta todo lo que pasa en la
red. Las líneas que importan:

```
genet: enlace arriba, 0x000003e8 Mbps        ← el cable negoció (0x3e8 = 1000)
net: listo, IP = 169.254.103.188, MAC = ...    ← la pila está en marcha
tcp: escuchando en el puerto 2323
net: ARP -- quien tiene 169.254.103.188? ...   ← el Mac nos ha encontrado
tcp: SYN de 169.254.100.1:49684 -- SYN-ACK    ← el Mac quiere conectar
tcp: conexion ESTABLECIDA
tcp: datos recibidos, 3 bytes (seq=...)       ← llegó un comando
```

### 7.2 El primer `nc` tras arrancar la Pi no conecta

**Síntoma:** `nc -v` no dice `succeeded!` al momento. La UART muestra:

```
tcp: checksum invalido, descartado. seg_len=44 doff=44 src=192.168.1.104 ...
tcp: bytes: ... d0 73 ...
```

y ningún `tcp: SYN de` detrás — o uno un segundo después.

**Qué es:** cada arranque de la Pi tira y levanta el enlace. Justo
después, macOS pasa por una ventana en la que envía el primer SYN con
la IP de origen equivocada (la del WiFi) y el checksum sin completar
por el adaptador (`0xd073` es exactamente la suma parcial de la
pseudo-cabecera — la "semilla" del *checksum offload*). Nemo OS lo
descarta, como debe: es un paquete corrupto de verdad. macOS reintenta
el SYN bien formado al cabo de ~1 segundo.

**Qué hacer:** esperar dos segundos antes de cortar. O cortar y repetir.
La IP fija en el Mac (§3.2) reduce la ventana; el anuncio ARP que Nemo
OS envía al arrancar también ayuda. No es un bug de Nemo OS.

### 7.3 `ping` no responde, nunca

- **`genet: el enlace no subio`** en la UART → no hay cable, o el otro
  extremo no negocia. Comprobar el cable y que el Mac tenga la interfaz
  activa.
- **`genet: el PHY no responde por MDIO en absoluto`** → eso sí sería un
  fallo del driver (dirección MDIO equivocada o PHY sin inicializar).
  Nunca ha pasado en la Pi 4 B.
- **Enlace arriba pero sin ARP en la UART** → el Mac no está buscando
  esa IP. Revisar la IP fija del Mac (§3.2) y que sea en la misma
  subred `169.254.x.x`. En QEMU, recordar que `ping` no funciona (§4).
- **`Request timeout` durante ~25 segundos y luego responde** → macOS
  todavía no tenía IP en la interfaz. Poner la IP fija (§3.2) lo evita.

### 7.4 Conecta pero la shell no responde

- **`tcp: datos recibidos` aparece en la UART pero nada vuelve** →
  el segmento de respuesta se pierde. Mirar si aparece `net: nic_send
  FALLO`. Si el banner inicial se perdió, esa conexión está muerta para
  siempre (sin retransmisión, todo lo demás llega "fuera de orden" para
  el cliente): cerrar y reconectar.
- **`tcp: segmento fuera de secuencia`** → un problema de numeración. La
  línea dice qué esperábamos y qué llegó.
- **Nada entre `ESTABLECIDA` y `FIN del cliente`** → los datos nunca
  llegaron a `tcp.c`. Comprobar que en el cliente se pulsó Enter (`nc`
  no envía hasta el salto de línea).

### 7.5 `nc` se cierra al instante

Nemo OS respondió con RST: o el puerto no es el 2323, o hay ya una
conexión en un estado que no admite otra. Mirar la UART. Si la anterior
quedó colgada, un SYN nuevo la sustituye — repetir suele bastar.

---

## 8. Probar sin hardware

Toda la pila menos los drivers se prueba en el host. Las baterías viven
fuera del árbol del kernel (se reconstruyen a mano, no están en el
Makefile), pero la receta es:

- Un `genet_pi4.c` **simulado** que guarda lo último enviado y deja
  inyectar una trama de entrada (`test_inyectar_rx`, `test_ultimo_tx`,
  `test_tx_count`).
- `net.c` y `tcp.c` reales, compilados para el host con
  `-fsanitize=address,undefined`.
- Un "cliente TCP" escrito a mano en la prueba que construye cada
  segmento con sus números de secuencia y ACK, y examina cada
  respuesta byte a byte, checksums incluidos.

Lo que cubren las baterías existentes:

- **ARP/ICMP:** petición ARP real → respuesta byte a byte; ping de 74
  bytes con la misma carga que el `ping` de macOS → respuesta con
  identificador, secuencia y carga idénticos, checksums IP e ICMP
  válidos; tráfico a otra IP ignorado; checksum corrupto descartado.
- **TCP (10 grupos):** RST a puerto cerrado; saludo de tres vías con
  MSS; eco con ACK a caballo; ACK acumulado; retransmisión del cliente
  → re-ACK sin eco doble; FIN → FIN+ACK; cierre y reconexión con ISN
  distinto; checksum corrupto descartado; banner al conectar; cierre
  activo por `exit`.
- **Shell (26 comprobaciones):** con el `nemofs.c` real sobre un disco
  en memoria — `ls`, `cd` anidado y `..`, `cat` con conversión a
  `\r\n` y corte a 900 bytes, `run` con y sin argumento, `tareas`,
  `mem`, `disco`, línea partida en dos segmentos, `\r\n`, retroceso,
  línea vacía, comando desconocido, `exit`, sesión nueva en la raíz.

Regla que ha salido de estas pruebas: **cuando la batería del host pasa
y el hardware falla, mirar primero si el arnés y el código los escribió
la misma persona con la misma idea.** El checksum de TCP con
pseudo-cabecera coincidía en los dos por construcción; lo que lo
verificó de verdad fue un paquete real de macOS.

---

## 9. Ampliar

- **Cambiar la IP:** `mi_ip` en `src/net.c`. Si se cambia en la Pi,
  cambiar también la del Mac a la misma subred.
- **Cambiar el puerto:** `NET_PUERTO_SHELL` en `src/net.h`. En QEMU,
  también el `hostfwd` del `Makefile`.
- **Añadir un comando a la shell:** una función `cmd_xxx(sal_t *s, ...)`
  en `src/netshell.c` y una rama en `ejecutar()`. La salida va con
  `sal_s`/`sal_u`/`sal_bytes`, que se cortan solos al llegar a la
  ventana. Recordar que corre dentro del tick del sistema: nada que
  espere, nada que bloquee.
- **Servir otra cosa por TCP:** implementar los ganchos `tcp_app_*` de
  `src/tcp.h` en otro archivo en vez de `netshell.c`.
- **Otra tarjeta de red** (un adaptador USB, otra placa): implementar
  las cinco funciones de `src/nic.h` y elegirla ahí según el build. La
  pila no se toca.
- **Lo que exigiría trabajo de verdad:** retransmisión (temporizadores,
  búfer de lo enviado sin confirmar), varias conexiones (una tabla de
  estados en vez de variables globales), salida de más de una ventana
  por comando (una cola de envío), UDP, DHCP.
