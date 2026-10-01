# Emulador de ARM64

`arm64.py` ejecuta un `.pro` de Nemo Basic en el ordenador de desarrollo, sin
Nemo OS y sin Raspberry Pi: lo carga en la dirección en la que lo cargaría el
kernel (`0x80c00000`), lo recorre instrucción a instrucción y atiende las
llamadas al sistema de escribir (11), registro (28) y salir (0). El resto de
llamadas no hacen nada y devuelven 0, así que sirve para programas de consola,
no para los que abren ventanas.

    python3 herramientas/emu/arm64.py programa.pro

Es la forma más rápida de ver qué hace un programa recién compilado: compilar y
ejecutar son dos órdenes, sin escribir una tarjeta ni reiniciar la placa.

**Una instrucción que no conoce detiene la emulación y lo dice**, con su número
en hexadecimal. Nunca da un resultado falso en silencio, que es lo único que
importa de una herramienta así: un emulador que se inventa lo que no entiende
no sirve, porque entonces el programa *parece* funcionar.

No implementa ARM64 entero, solo lo que generan el compilador y el bloque de
runtime. Cuando sale una instrucción nueva, el número que imprime **es** la
instrucción: se busca en el manual de ARM y se añade a `paso()` o a `carga()`.
Las de registro escalar (`ushr`, `scvtf`, `movi`) llegaron así, al usar `Rnd()`
sin argumento y `Val#` por primera vez.
