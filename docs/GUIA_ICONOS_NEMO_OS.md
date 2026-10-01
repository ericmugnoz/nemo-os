# Los iconos de Nemo OS

En **DOCUMENTOS/IMAGENES** hay una biblioteca de iconos en formato NIMG, lista para usar en tus programas: **tiras** para la barra de herramientas y **iconos sueltos** de 32 píxeles para paneles o para `LoadImage`.

## Las tiras de la barra de herramientas

Cada tira existe en dos tamaños: `16` y `24` píxeles (`tb_basica16.nimg`, `tb_basica24.nimg`). Al pulsar un botón, tu programa recibe **su número**, empezando en 0: en Nemo Basic, `EventData()`; en Lua, el dato de `info_evento`; en Aronnax, el argumento de `Barra_Click(boton)`.

**tb_basica** (10 botones): 0 nuevo, 1 abrir, 2 guardar, 3 cortar, 4 copiar, 5 pegar, 6 deshacer, 7 ejecutar, 8 parar, 9 ayuda.

**tb_archivo** (5 botones): 0 nuevo, 1 abrir, 2 guardar, 3 imprimir, 4 cerrar.

**tb_edicion** (7 botones): 0 deshacer, 1 rehacer, 2 cortar, 3 copiar, 4 pegar, 5 borrar (papelera), 6 buscar.

**tb_ejecutar** (6 botones): 0 ejecutar, 1 pausa, 2 parar, 3 paso a paso, 4 compilar (engranaje), 5 depurar (bicho).

**tb_navegar** (8 botones): 0 atrás, 1 adelante, 2 arriba, 3 abajo, 4 inicio (casa), 5 recargar, 6 acercar (lupa +), 7 alejar (lupa −).

**tb_nautilus** (8 botones): 0 LED encendido, 1 LED apagado, 2 pulsador, 3 PWM (onda cuadrada), 4 sensor (termómetro), 5 chip, 6 enchufe, 7 rayo.

**tb_avisos** (5 botones): 0 información, 1 aviso, 2 error, 3 correcto, 4 ayuda.

**tb_varios** (10 botones): 0 ajustes, 1 añadir, 2 quitar, 3 carpeta, 4 archivo, 5 imagen, 6 música, 7 reloj, 8 correo, 9 estrella.

```basic
barra = CreateToolBar("tb_basica24.nimg", 0, 0, 0, 0)
SetToolBarTips barra, "Nuevo,Abrir,Guardar,Cortar,Copiar,Pegar,Deshacer,Ejecutar,Parar,Ayuda"
```

## Los iconos sueltos, de 32 píxeles

**Todos** los iconos de las tiras existen también sueltos, a 32 píxeles, con el mismo nombre: `ico32_nuevo.nimg`, `ico32_cortar.nimg`, `ico32_led_on.nimg`... Son opacos, sobre el gris claro de las ventanas (un panel dibuja su imagen tal cual, sin transparencias).

Así un programa puede mostrar en grande el icono del botón que se acaba de pulsar:

```basic
Dim boton$(9)
Restore nombres
For i = 0 To 9
  Read boton$(i)
Next
; ... y al pulsar un boton de la barra:
SetPanelImage icono, "ico32_" + boton$(EventData()) + ".nimg"

.nombres
Data "nuevo", "abrir", "guardar", "cortar", "copiar", "pegar", "deshacer", "ejecutar", "parar", "ayuda"
```

## Para los juegos: los mismos, pero transparentes

`tr_mina.nimg`, `tr_explosion.nimg` y `tr_bandera.nimg` son las tres de 32 px con el **fondo transparente** (magenta), para dibujarlas sobre lo que sea:

```basic
mina = LoadImage("tr_mina.nimg")
MaskImage mina, 16711935          ; 0xFF00FF: ese color pasa a transparente
DrawImage mina, x, y
```

## Dónde se buscan las imágenes

`CreateToolBar`, `SetPanelImage` y `LoadImage` buscan la imagen por su nombre en la **raíz**, en **DOCUMENTOS** y en **DOCUMENTOS/IMAGENES**, por ese orden. Guarda ahí tus imágenes y tus tiras.

## Hacer tus propias tiras

Una tira es una imagen NIMG con los iconos **cuadrados, uno al lado del otro**: el alto es el tamaño de cada botón y el número de botones es el ancho dividido por el alto. El **primer píxel** (arriba a la izquierda) marca el color transparente. Como mucho, 256 píxeles de ancho: 16 botones de 16 o 10 de 24.

Para convertir tus imágenes está `herramientas/nimg/nimg_convert.py`, y los iconos de esta biblioteca los dibuja `herramientas/iconos/crear_iconos.py`, donde se pueden añadir más.
