-- aronnax_proyecto.lua -- los proyectos de Aronnax, el diseñador visual de
-- Nemo Basic: que es un proyecto, como se guarda (.anx) y como se convierte
-- en un programa de Nemo Basic (.nb) que el compilador normal acepta.
--
-- Aronnax, por el profesor Pierre Aronnax de "Veinte mil leguas de viaje
-- submarino": el que observa, dibuja y cataloga lo que ve por los
-- ventanales del Nautilus.
--
-- El programa generado es Nemo Basic corriente, sin magia: una parte que
-- escribe Aronnax (crear la ventana y los controles, preparar los pines,
-- y el bucle que reparte los eventos) y, detras, el codigo del usuario tal
-- cual. Solo se llama a las funciones que el usuario ha escrito: si no hay
-- Boton1_Click, no se llama, y el programa compila igual.
local M = {}
-- La version de esta biblioteca: aronnax.lua comprueba que es la que necesita
-- (3: menus, barra de herramientas y temporizador)
M.VERSION = 3

-- ---------------------------------------------------------------------
-- Los tipos de control
-- ---------------------------------------------------------------------
-- nombre     prefijo del nombre que se propone al crearlo (Boton1, Boton2...)
-- texto      si lleva texto (CreateButton, CreateLabel...)
-- evento     sufijo de su funcion de evento ("Click", "Cambia"), o nil
-- ancho/alto tamaño con que nace
-- gpio       "salida", "entrada", "pwm" o "analogico" (seccion Nautilus)
M.TIPOS = {
  Boton       = { prefijo = "Boton",      texto = "Boton",   evento = "Click",  ancho = 90,  alto = 26 },
  Etiqueta    = { prefijo = "Etiqueta",   texto = "Etiqueta",                   ancho = 100, alto = 20 },
  Campo       = { prefijo = "Campo",                          evento = "Cambia", ancho = 140, alto = 22 },
  Casilla     = { prefijo = "Casilla",    texto = "Casilla", evento = "Click",  ancho = 120, alto = 20 },
  Opcion      = { prefijo = "Opcion",     texto = "Opcion",  evento = "Click",  ancho = 120, alto = 20 },
  Lista       = { prefijo = "Lista",                          evento = "Cambia", ancho = 140, alto = 100 },
  Desplegable = { prefijo = "Desplegable",                    evento = "Cambia", ancho = 140, alto = 22 },
  Pestanas    = { prefijo = "Pestanas",                       evento = "Cambia", ancho = 240, alto = 24 },
  Deslizador  = { prefijo = "Deslizador",                     evento = "Cambia", ancho = 160, alto = 20 },
  Progreso    = { prefijo = "Progreso",                                          ancho = 160, alto = 16 },
  Texto       = { prefijo = "Texto",                                             ancho = 200, alto = 100 },
  Panel       = { prefijo = "Panel",                                             ancho = 120, alto = 80 },
  -- una imagen NIMG: en el programa, un panel con SetPanelImage.
  -- El kernel la busca en la raiz, en DOCUMENTOS y en DOCUMENTOS/IMAGENES; si el panel es mas grande
  -- que la imagen, la repite en mosaico.
  Imagen      = { prefijo = "Imagen",                                            ancho = 32,  alto = 32, imagen = "ico32_imagen.nimg" },
  -- el arbol (sus nodos se añaden desde el codigo; Cambia recibe
  -- el nodo elegido) y el lienzo (Dibuja: lo que se pinta en el, ya con
  -- SetBuffer puesto; Lienzo1_Redibuja() lo vuelve a pintar)
  Arbol       = { prefijo = "Arbol",                          evento = "Cambia", ancho = 160, alto = 120 },
  Lienzo      = { prefijo = "Lienzo",                         evento = "Dibuja", ancho = 200, alto = 120 },
  -- Nautilus: controles conectados a los pines
  Led         = { prefijo = "Led",        gpio = "salida",    ancho = 24,  alto = 24,  pin = 17 },
  Interruptor = { prefijo = "Interruptor", texto = "Encender", evento = "Click", gpio = "salida", ancho = 120, alto = 20, pin = 27 },
  Pulsador    = { prefijo = "Pulsador",   gpio = "entrada",   evento = "Cambia", ancho = 24, alto = 24, pin = 4 },
  Pwm         = { prefijo = "Pwm",        gpio = "pwm",       evento = "Cambia", ancho = 160, alto = 20, pin = 18 },
  Analogico   = { prefijo = "Analogico",  gpio = "analogico", evento = "Cambia", ancho = 160, alto = 16, canal = 0 },
  -- Telemetria: hablar con otra maquina de la red
  --
  -- Estos dos no existen porque "haga falta un control para la red" --
  -- los comandos de red ya se pueden escribir a mano en el codigo de
  -- cualquier boton. Existen porque el GENERADOR se queda con el bucle
  -- asincrono, que es donde estan las dos trampas que no dan error:
  --
  --   * vaciar LA COLA ENTERA de UDP en cada vuelta, no un datagrama;
  --   * preguntar de quien era PEGADO al UdpRecv$ que lo trajo.
  --
  -- Es lo mismo que hace Aronnax con el pin libre de los del Nautilus:
  -- encargarse de lo que nadie se acuerda de hacer bien.
  Sensor      = { prefijo = "Sensor",   red = "http", evento = "Respuesta", ancho = 180, alto = 20,
                  ip = "10.0.2.2", puerto = 8099, ruta = "/sensor", cada = 1000 },
  Mensaje     = { prefijo = "Mensaje",  red = "udp",  evento = "Recibido",  ancho = 180, alto = 20,
                  puerto = 7000 },
}
-- (el orden de la paleta)
M.ORDEN = { "Boton", "Etiqueta", "Campo", "Casilla", "Opcion", "Lista", "Desplegable", "Pestanas",
            "Deslizador", "Progreso", "Texto", "Panel", "Imagen", "Arbol", "Lienzo", "Led", "Interruptor", "Pulsador", "Pwm", "Analogico",
            "Sensor", "Mensaje" }
M.PINES_PWM = { [12] = true, [13] = true, [18] = true, [19] = true }

-- Topes del kernel que Aronnax tiene que conocer para avisar (gadgets.c).
-- Estan aqui y no repartidos por el codigo para que se vean juntos y para
-- que, si algun dia cambian en el kernel, se cambien en un solo sitio.
M.CAJAS_TEXTO       = 4      -- TEXTAREA_CAJAS: en TODO el sistema, no por programa
M.LINEAS_TEXTO      = 200    -- TEXTAREA_MAX_LINES
M.LARGO_LINEA_TEXTO = 128    -- TEXTAREA_LINE_MAX

-- ---------------------------------------------------------------------
-- Un proyecto
-- ---------------------------------------------------------------------
function M.nuevo()
  return { ventana = { titulo = "Mi programa", x = 100, y = 100, ancho = 400, alto = 300,
                       maximizar = true, minimizar = true, cerrar = true },
           controles = {}, codigo = "",
           -- menus, barra de herramientas y temporizador
           menus = {},        -- { { texto = "Archivo", entradas = { { texto = "Nuevo", nombre = "MenuNuevo" }, ... } }, ... }
                              -- una entrada con 'entradas' propias abre un submenu
           barra = nil,       -- { archivo = "tb_basica24.nimg", ayudas = "Nuevo,Abrir,..." }
           temporizador = 0 } -- veces por segundo (0 = sin temporizador)
end

-- ---------------------------------------------------------------------
-- Menus, barra de herramientas y temporizador
-- ---------------------------------------------------------------------
-- Las tiras de iconos de DOCUMENTOS/IMAGENES, con los textos de ayuda de sus botones
M.BARRAS = {
  { "tb_basica24.nimg",   "Nuevo,Abrir,Guardar,Cortar,Copiar,Pegar,Deshacer,Ejecutar,Parar,Ayuda" },
  { "tb_archivo24.nimg",  "Nuevo,Abrir,Guardar,Imprimir,Cerrar" },
  { "tb_edicion24.nimg",  "Deshacer,Rehacer,Cortar,Copiar,Pegar,Borrar,Buscar" },
  { "tb_ejecutar24.nimg", "Ejecutar,Pausa,Parar,Paso,Compilar,Depurar" },
  { "tb_navegar24.nimg",  "Atrás,Adelante,Arriba,Abajo,Inicio,Recargar,Acercar,Alejar" },
  { "tb_nautilus24.nimg", "LED encendido,LED apagado,Pulsador,PWM,Sensor,Chip,Enchufe,Rayo" },
  { "tb_avisos24.nimg",   "Información,Aviso,Error,Correcto,Ayuda" },
  { "tb_varios24.nimg",   "Ajustes,Añadir,Quitar,Carpeta,Archivo,Imagen,Música,Reloj,Correo,Estrella" },
}
M.ALTO_BARRA = 24                    -- las tiras de Aronnax son las de 24 px
M.ALTO_MENUS = 16                    -- la barra de menus que dibuja el kernel

-- Un nombre de funcion a partir del texto de una entrada: "Guardar como..."
-- -> "MenuGuardarcomo" (sin tildes ni signos), unico en el proyecto
local SIN_TILDE = { ["á"] = "a", ["é"] = "e", ["í"] = "i", ["ó"] = "o", ["ú"] = "u", ["ñ"] = "n", ["ü"] = "u",
                    ["Á"] = "A", ["É"] = "E", ["Í"] = "I", ["Ó"] = "O", ["Ú"] = "U", ["Ñ"] = "N" }
function M.nombres_usados(p)
  local u = {}
  for _, c in ipairs(p.controles) do u[c.nombre:lower()] = true end
  for _, e in ipairs(M.todas_las_entradas(p)) do if e.nombre then u[e.nombre:lower()] = true end end
  u.barra = true; u.temporizador = true; u.ventana = true
  return u
end
function M.nombre_menu(p, texto)
  local t = texto:gsub("[\195][\128-\191]", function(c) return SIN_TILDE[c] or "" end):gsub("[^%w]", "")
  if t == "" then t = "Entrada" end
  t = t:sub(1, 1):upper() .. t:sub(2)
  local base, n, usados = "Menu" .. t:sub(1, 20), 1, M.nombres_usados(p)
  local nombre = base
  while usados[nombre:lower()] do n = n + 1; nombre = base .. n end
  return nombre
end
function M.anadir_menu(p, texto)
  local m = { texto = texto or "Menu", entradas = {} }
  p.menus[#p.menus + 1] = m
  return m
end
-- texto "" = una linea de separacion (sin nombre ni funcion)
function M.anadir_entrada(p, m, texto)
  local e = { texto = texto or "" }
  if e.texto ~= "" then e.nombre = M.nombre_menu(p, e.texto) end
  m.entradas[#m.entradas + 1] = e
  return e
end
-- Una subentrada dentro de la entrada 'madre' (que pasa a abrir un submenu)
function M.anadir_subentrada(p, madre, texto)
  madre.entradas = madre.entradas or {}
  local e = { texto = texto or "" }
  if e.texto ~= "" then e.nombre = M.nombre_menu(p, e.texto) end
  madre.entradas[#madre.entradas + 1] = e
  return e
end
-- Todas las entradas del proyecto, las de los submenus incluidas, en orden
function M.todas_las_entradas(p)
  local r = {}
  for _, m in ipairs(p.menus or {}) do
    for _, e in ipairs(m.entradas) do
      r[#r + 1] = e
      for _, s in ipairs(e.entradas or {}) do r[#r + 1] = s end
    end
  end
  return r
end
-- Una entrada con submenu no tiene funcion propia: pulsarla lo abre
function M.tiene_submenu(e) return e.entradas ~= nil and #e.entradas > 0 end

-- Anclajes: que hace el control cuando la ventana cambia de
-- tamaño. El programa generado los aplica en Aronnax_Recolocar().
M.ANCLAS = { "fijo", "derecha", "abajo", "derecha-abajo", "ancho", "alto", "ancho-alto" }
function M.ancla_siguiente(a)
  for i, v in ipairs(M.ANCLAS) do if v == (a or "fijo") then return M.ANCLAS[i % #M.ANCLAS + 1] end end
  return M.ANCLAS[1]
end
-- Como afecta el cambio de tamaño: mueve en x, mueve en y, estira a lo ancho, a lo alto
function M.ancla_efecto(a)
  a = a or "fijo"
  return a == "derecha" or a == "derecha-abajo", a == "abajo" or a == "derecha-abajo",
         a == "ancho" or a == "ancho-alto", a == "alto" or a == "ancho-alto"
end


-- ---- Ayuda mientras escribes ----
-- Las ordenes mas usadas de Nemo Basic, con sus argumentos. Aronnax las
-- sugiere al escribir y enseña la firma de la que estas escribiendo.
M.COMANDOS = {
  -- ventana y eventos
  { "CreateWindow", "titulo$, x, y, ancho, alto" }, { "WaitEvent", "()" }, { "EventSource", "()" }, { "EventData", "()" },
  { "ClientWidth", "()" }, { "ClientHeight", "()" }, { "WindowButtons", "max, min, cerrar" },
  -- controles
  { "CreateButton", "texto$, x, y, ancho, alto" }, { "CreateLabel", "texto$, x, y, ancho, alto" },
  { "CreateTextField", "x, y, ancho, alto" }, { "CreateCheckBox", "texto$, x, y, ancho, alto" },
  { "CreateRadio", "texto$, x, y, ancho, alto" }, { "CreateListBox", "x, y, ancho, alto" },
  { "CreateComboBox", "x, y, ancho, alto" }, { "CreateTabber", "x, y, ancho, alto" },
  { "CreateSlider", "x, y, ancho, alto" }, { "CreateProgBar", "x, y, ancho, alto" },
  { "CreateTextArea", "x, y, ancho, alto" }, { "CreatePanel", "x, y, ancho, alto" },
  { "CreateTreeView", "x, y, ancho, alto" }, { "CreateCanvas", "x, y, ancho, alto" },
  { "SetGadgetText", "id, texto$" }, { "GadgetText$", "id" }, { "FreeGadget", "id" },
  { "AddGadgetItem", "id, texto$" }, { "InsertGadgetItem", "id, indice, texto$" }, { "ModifyGadgetItem", "id, indice, texto$" },
  { "RemoveGadgetItem", "id, indice" }, { "ClearGadgetItems", "id" }, { "CountGadgetItems", "id" },
  { "SelectedGadgetItem", "id" }, { "SelectGadgetItem", "id, indice" },
  { "HideGadget", "id" }, { "ShowGadget", "id" }, { "DisableGadget", "id" }, { "EnableGadget", "id" }, { "ActivateGadget", "id" },
  { "ButtonState", "id (1 si esta marcado)" }, { "SetButtonState", "id, 0 o 1" },
  { "SliderValue", "id" }, { "SetSliderValue", "id, valor" }, { "SetSliderRange", "id, visible, total" },
  { "UpdateProgBar", "id, de 0.0 a 1.0" }, { "SetPanelColor", "id, r, v, a" }, { "SetPanelImage", "id, archivo$" },
  { "SetTextAreaText", "id, texto$" }, { "AddTextAreaText", "id, texto$" },
  { "SetGadgetShape", "id, x, y, ancho, alto" }, { "GadgetX", "id" }, { "GadgetY", "id" },
  { "GadgetWidth", "id" }, { "GadgetHeight", "id" },
  -- menus, barra, temporizador
  { "WindowMenu", "()" }, { "CreateMenu", "texto$, etiqueta, padre" }, { "UpdateWindowMenu", "()" },
  { "CheckMenu", "menu" }, { "UncheckMenu", "menu" }, { "EnableMenu", "menu" }, { "DisableMenu", "menu" },
  { "CreateToolBar", "archivo$, x, y, ancho, alto" }, { "SetToolBarTips", "barra, textos$" },
  { "EnableToolBarItem", "barra, boton" }, { "DisableToolBarItem", "barra, boton" },
  { "CreateTimer", "veces por segundo" }, { "FreeTimer", "t" }, { "PauseTimer", "t" }, { "ResumeTimer", "t" },
  { "ResetTimer", "t" }, { "TimerTicks", "t" },
  -- arbol
  { "TreeViewRoot", "arbol" }, { "AddTreeViewNode", "texto$, padre" }, { "InsertTreeViewNode", "indice, texto$, padre" },
  { "ModifyTreeViewNode", "nodo, texto$" }, { "FreeTreeViewNode", "nodo" },
  { "ExpandTreeViewNode", "nodo" }, { "CollapseTreeViewNode", "nodo" },
  { "CountTreeViewNodes", "padre" }, { "SelectedTreeViewNode", "arbol" }, { "SelectTreeViewNode", "nodo" },
  -- teclado y raton
  { "KeyDown", "codigo (1 mientras este pulsada)" }, { "KeyHit", "codigo (1 una vez por pulsacion)" },
  { "GetKey", "() (el siguiente codigo de la cola, 0 si no hay)" },
  { "MouseX", "()" }, { "MouseY", "()" },
  { "MouseDown", "boton (1 izquierdo, 2 derecho)" }, { "MouseHit", "boton (1 una vez por clic)" },
  -- dibujo
  { "ClsColor", "r, v, a (el color con el que borra Cls)" },
  { "CreateImage", "ancho, alto" }, { "MaskImage", "imagen, color 0xRRGGBB" },
  { "FreeImage", "imagen" }, { "CopyImage", "imagen" }, { "SaveImage", "imagen, archivo$" },
  { "HandleImage", "imagen, x, y (su punto de referencia)" }, { "FlipCanvas", "lienzo" },
  -- numeros
  { "Floor", "numero" }, { "Ceil", "numero" }, { "Sgn", "numero (-1, 0 o 1)" },
  { "Min", "a, b" }, { "Max", "a, b" }, { "Float", "numero" }, { "Seed", "semilla" },
  { "SetBuffer", "destino" }, { "CanvasBuffer", "lienzo" }, { "BackBuffer", "()" }, { "ImageBuffer", "imagen" },
  { "Color", "r, v, a" }, { "Cls", "()" }, { "Rect", "x, y, ancho, alto, relleno" }, { "Oval", "x, y, ancho, alto, relleno" },
  { "Line", "x1, y1, x2, y2" }, { "Plot", "x, y" }, { "Text", "x, y, texto$" },
  { "LoadImage", "archivo$" }, { "DrawImage", "imagen, x, y" }, { "ImageWidth", "imagen" }, { "ImageHeight", "imagen" },
  -- cadenas
  { "Len", "texto$" }, { "Left$", "texto$, n" }, { "Right$", "texto$, n" }, { "Mid$", "texto$, desde, n" },
  { "Upper$", "texto$" }, { "Lower$", "texto$" }, { "Trim$", "texto$" }, { "Replace$", "texto$, viejo$, nuevo$" },
  { "Instr", "texto$, busca$" }, { "Str$", "numero" }, { "Val", "texto$" }, { "Chr$", "codigo" }, { "Asc", "texto$" },
  { "String$", "texto$, veces" }, { "LSet$", "texto$, ancho" }, { "RSet$", "texto$, ancho" },
  { "Hex$", "numero" }, { "Bin$", "numero" },
  -- archivos
  { "OpenFile", "nombre$" }, { "ReadLine$", "archivo" }, { "Eof", "archivo" }, { "CloseFile", "archivo" },
  { "WriteFile", "nombre$ (o nombre$, texto$)" }, { "WriteLine", "archivo, texto$" },
  { "FilePos", "archivo" }, { "SeekFile", "archivo, posicion" }, { "FileLength", "archivo" },
  { "FileSize", "nombre$" }, { "FileType", "nombre$" }, { "FileExists", "nombre$" }, { "DeleteFile", "nombre$" },
  { "CreateDir", "nombre$" }, { "ReadDir", "carpeta$" }, { "NextFile$", "carpeta" }, { "CloseDir", "carpeta" },
  -- numeros, GPIO y sonido
  { "Rnd", "hasta (o desde, hasta)" }, { "Int", "numero" }, { "Abs", "numero" }, { "Sqr", "numero" },
  { "Sin", "grados" }, { "Cos", "grados" }, { "Tan", "grados" }, { "ATan", "numero" }, { "Exp", "n" }, { "Log", "n" },
  { "MilliSecs", "()" }, { "Delay", "milisegundos" },
  { "GpioMode", "pin, modo (0 entrada, 1 salida)" }, { "GpioWrite", "pin, 0 o 1" }, { "GpioRead", "pin" },
  { "GpioPwm", "pin, de 0 a 255" },
  { "I2cWrite", "direccion, datos$" }, { "I2cRead$", "direccion, cuantos" }, { "SpiTransfer$", "datos$" },
}
-- Los nombres que encajan con lo escrito (controles, funciones y ordenes)
function M.sugerencias(p, prefijo, maximo)
  local r, vistos = {}, {}
  local pre = prefijo:lower()
  local function mete(nombre, ayuda)
    if #r >= (maximo or 8) or vistos[nombre] then return end
    if nombre:lower():sub(1, #pre) == pre then vistos[nombre] = true; r[#r + 1] = { nombre, ayuda } end
  end
  for _, c in ipairs(p.controles) do mete(c.nombre, M.NOMBRE_TIPO and M.NOMBRE_TIPO[c.tipo] or c.tipo) end
  for _, e in ipairs(M.todas_las_entradas(p)) do if e.nombre then mete(e.nombre, "entrada de menú") end end
  for nombre in (p.codigo or ""):gmatch("Function%s+([%w_$#]+)%s*%(") do mete(nombre, "función tuya") end
  for _, c in ipairs(M.COMANDOS) do mete(c[1], c[2]) end
  return r
end
-- La firma de una orden ("SetGadgetText" -> "id, texto$"), o nil
function M.firma(nombre)
  for _, c in ipairs(M.COMANDOS) do if c[1]:lower() == nombre:lower() then return c[1], c[2] end end
  return nil
end


-- ---- Plantillas de proyecto ----
-- Cada una devuelve un proyecto COMPLETO, que compila y se puede ejecutar tal
-- cual: mejor empezar de algo que funciona que de una ventana vacia.
local function pon(p, tipo, x, y, ancho, alto, texto, extra)
  local c = M.anadir(p, tipo, x, y)
  if ancho then c.ancho, c.alto = ancho, alto end
  if texto then c.texto = texto end
  for k, v in pairs(extra or {}) do c[k] = v end
  return c
end

M.PLANTILLAS = {
  { "Formulario", "etiquetas, campos y botones Aceptar / Cancelar", function()
      local p = M.nuevo()
      p.ventana.titulo, p.ventana.ancho, p.ventana.alto = "Datos", 420, 220
      pon(p, "Etiqueta", 16, 20, 80, 20, "Nombre:")
      local nombre = pon(p, "Campo", 110, 16, 280, 26, "", { ancla = "ancho" })
      pon(p, "Etiqueta", 16, 60, 80, 20, "Correo:")
      local correo = pon(p, "Campo", 110, 56, 280, 26, "", { ancla = "ancho" })
      pon(p, "Casilla", 110, 96, 200, 22, "Avisarme por correo")
      local aviso = pon(p, "Etiqueta", 16, 130, 380, 20, "", { ancla = "ancho" })
      local aceptar = pon(p, "Boton", 220, 170, 80, 28, "Aceptar", { ancla = "derecha-abajo" })
      local cancelar = pon(p, "Boton", 310, 170, 80, 28, "Cancelar", { ancla = "derecha-abajo" })
      p.codigo = string.format([[
Function %s_Click()
  If GadgetText$(%s) = "" Then
    SetGadgetText %s, "Escribe tu nombre."
  Else
    SetGadgetText %s, "Hola, " + GadgetText$(%s) + " (" + GadgetText$(%s) + ")"
  EndIf
End Function

Function %s_Click()
  SetGadgetText %s, ""
  SetGadgetText %s, ""
  SetGadgetText %s, "Campos vaciados."
End Function
]], aceptar.nombre, nombre.nombre, aviso.nombre, aviso.nombre, nombre.nombre, correo.nombre,
    cancelar.nombre, nombre.nombre, correo.nombre, aviso.nombre)
      return p
    end },
  { "Lista y detalle", "una lista que crece, con Añadir y Quitar", function()
      local p = M.nuevo()
      p.ventana.titulo, p.ventana.ancho, p.ventana.alto = "Mi lista", 460, 300
      local lista = pon(p, "Lista", 16, 16, 200, 220, nil, { ancla = "alto" })
      local campo = pon(p, "Campo", 230, 16, 210, 26, "", { ancla = "ancho" })
      local anadir = pon(p, "Boton", 230, 52, 100, 28, "Añadir")
      local quitar = pon(p, "Boton", 340, 52, 100, 28, "Quitar")
      local detalle = pon(p, "Etiqueta", 230, 96, 210, 20, "Elige algo de la lista", { ancla = "ancho" })
      p.codigo = string.format([[
Function %s_Click()
  If GadgetText$(%s) <> ""
    AddGadgetItem %s, GadgetText$(%s)
    SetGadgetText %s, ""
  EndIf
End Function

Function %s_Click()
  n = SelectedGadgetItem(%s)
  If n >= 0 Then RemoveGadgetItem %s, n
End Function

Function %s_Cambia(valor)
  If valor >= 0 Then SetGadgetText %s, "Elegido: " + GadgetText$(%s)
End Function
]], anadir.nombre, campo.nombre, lista.nombre, campo.nombre, campo.nombre,
    quitar.nombre, lista.nombre, lista.nombre,
    lista.nombre, detalle.nombre, lista.nombre)
      return p
    end },
  { "Menús y barra", "menús Archivo y Ayuda, barra de herramientas y texto", function()
      local p = M.nuevo()
      p.ventana.titulo, p.ventana.ancho, p.ventana.alto = "Editor", 500, 340
      p.barra = { archivo = "tb_basica24.nimg", ayudas = M.ayudas_de("tb_basica24.nimg") }
      local texto = pon(p, "Texto", 12, 44, 476, 250, nil, { ancla = "ancho-alto" })
      local m = M.anadir_menu(p, "Archivo")
      local nuevo = M.anadir_entrada(p, m, "Nuevo")
      M.anadir_entrada(p, m, "")
      local salir = M.anadir_entrada(p, m, "Salir")
      local ayuda = M.anadir_menu(p, "Ayuda")
      local acerca = M.anadir_entrada(p, ayuda, "Acerca de")
      p.codigo = string.format([[
Function %s_Click()
  SetTextAreaText %s, ""
End Function

Function %s_Click()
  End
End Function

Function %s_Click()
  AddTextAreaText %s, "Hecho con Aronnax, en Nemo OS." + Chr$(10)
End Function

Function Barra_Click(boton)
  AddTextAreaText %s, "Botón " + Str$(boton) + " de la barra." + Chr$(10)
End Function
]], nuevo.nombre, texto.nombre, salir.nombre, acerca.nombre, texto.nombre, texto.nombre)
      return p
    end },
  { "Panel del Nautilus", "LED, pulsador, PWM y sensor, con temporizador", function()
      local p = M.nuevo()
      p.ventana.titulo, p.ventana.ancho, p.ventana.alto = "Panel del Nautilus", 460, 260
      p.temporizador = 5
      local led = pon(p, "Led", 20, 30, nil, nil, nil, { pin = 17 })
      pon(p, "Etiqueta", 20, 10, 100, 20, "Luz")
      -- el LED se enciende desde el codigo (dos controles no pueden compartir pin)
      local inter = pon(p, "Boton", 60, 26, 120, 28, "Encender")
      pon(p, "Etiqueta", 240, 10, 140, 20, "Brillo")
      local pwm = pon(p, "Pwm", 240, 30, 180, 24, nil, { pin = 18 })
      local puls = pon(p, "Pulsador", 20, 110, nil, nil, nil, { pin = 4 })
      pon(p, "Etiqueta", 20, 90, 160, 20, "Pulsador (GPIO 4)")
      local ana = pon(p, "Analogico", 240, 110, 180, 22, nil, { canal = 0 })
      local sensor = pon(p, "Etiqueta", 240, 90, 180, 20, "Sensor: -")
      p.codigo = string.format([[
Global luz_encendida

Function %s_Click()
  If luz_encendida = 0
    luz_encendida = 1
    %s_Pon(1)
    SetGadgetText %s, "Apagar"
  Else
    luz_encendida = 0
    %s_Pon(0)
    SetGadgetText %s, "Encender"
  EndIf
End Function

Function %s_Cambia(valor)
  SetGadgetText %s, "Sensor: " + Str$(valor)
End Function

Function %s_Pulsa(valor)
  If valor = 1 Then SetGadgetText %s, "Sensor: pulsado"
End Function

Function Temporizador_Tick()
  ; aqui puedes refrescar lo que quieras cinco veces por segundo
End Function
]], inter.nombre, led.nombre, inter.nombre, led.nombre, inter.nombre,
    ana.nombre, sensor.nombre, puls.nombre, sensor.nombre)
      return p
    end },
  { "Telemetría", "manda una lectura por HTTP y habla por UDP con la red", function()
      local p = M.nuevo()
      p.ventana.titulo, p.ventana.ancho, p.ventana.alto = "Telemetría", 420, 260
      local eti = pon(p, "Etiqueta", 16, 16, 380, 20, "Valor que se manda:")
      local desl = pon(p, "Deslizador", 16, 40, 380, 22)
      local sen = pon(p, "Sensor", 16, 76, 380, 20, nil,
                      { ip = "10.0.2.2", puerto = 8099, ruta = "/sensor", cada = 2000 })
      local reg = pon(p, "Texto", 16, 136, 380, 100, nil, { ancla = "ancho-alto" })
      local msg = pon(p, "Mensaje", 16, 108, 380, 20, nil, { puerto = 7000 })
      local hola = pon(p, "Boton", 16, 236, 120, 24, "Saludar a la red")
      p.codigo = string.format([[
; El valor que manda el sensor cada dos segundos. Aronnax llama a esta
; funcion cuando le toca: tu decides QUE se manda, el se encarga de
; cuando y de esperar la respuesta sin congelar el programa.
Function %s_Valor$()
  Return "nivel=" + Str$(SliderValue(%s))
End Function

Function %s_Respuesta(codigo, cuerpo$)
  If codigo >= 200 And codigo < 300
    AddTextAreaText %s, "enviado, el servidor dice: " + cuerpo$ + Chr$(10)
  Else
    AddTextAreaText %s, "no se pudo enviar: " + HttpFail$() + Chr$(10)
  EndIf
End Function

; Un mensaje de otra maquina de la red. 'de$' es SU direccion, la del
; datagrama que acaba de llegar -- no la del ultimo que hablo.
Function %s_Recibido(texto$, de$, puerto)
  AddTextAreaText %s, de$ + " dice: " + texto$ + Chr$(10)
  ; Contestarle a el, no a gritos. Solo vale aqui dentro: en cuanto se
  ; lee otro mensaje, el remitente cambia.
  If Left$(texto$, 4) = "hola" Then %s_Responde("hola tu tambien")
End Function

; A toda la red, sin saber quien hay ni donde esta. Asi es como dos
; maquinas se encuentran sin teclear ninguna direccion.
Function %s_Click()
  %s_Grita("hola, soy " + NetIp$())
End Function
]], sen.nombre, desl.nombre, sen.nombre, reg.nombre, reg.nombre,
    msg.nombre, reg.nombre, msg.nombre, hola.nombre, msg.nombre)
      return p
    end },
  { "Lienzo animado", "una pelota que rebota, con decimales y gravedad", function()
      local p = M.nuevo()
      p.ventana.titulo, p.ventana.ancho, p.ventana.alto = "Pelota", 420, 300
      p.temporizador = 20
      local lienzo = pon(p, "Lienzo", 10, 10, 400, 240, nil, { ancla = "ancho-alto" })
      local marcador = pon(p, "Etiqueta", 10, 260, 400, 20, "x = 0", { ancla = "ancho" })
      p.codigo = string.format([[
; La pelota se mueve con DECIMALES: el rebote sale suave y se puede tener
; gravedad, en vez de ir a saltos de un numero entero de pixeles.
Global x#, y#, vx#, vy#, RADIO
Function Ventana_Abre()
  RADIO = 25
  x# = 60.0 : y# = 60.0
  vx# = 3.4 : vy# = 0.0
End Function
Function %s_Dibuja()
  Color 250, 250, 246
  Rect 0, 0, GadgetWidth(%s), GadgetHeight(%s), 1
  Color 210, 60, 45
  Oval x# - RADIO, y# - RADIO, RADIO * 2, RADIO * 2, 1
End Function
Function Temporizador_Tick()
  Local ancho, alto
  ancho = GadgetWidth(%s)
  alto = GadgetHeight(%s)
  vy# = vy# + 0.45                      ; la gravedad
  x# = x# + vx#
  y# = y# + vy#
  If x# < RADIO
    x# = RADIO
    vx# = -vx#
  EndIf
  If x# > ancho - RADIO
    x# = ancho - RADIO
    vx# = -vx#
  EndIf
  If y# > alto - RADIO                  ; el suelo: rebota perdiendo algo
    y# = alto - RADIO
    vy# = -vy# * 0.86
    If vy# > -1.2 Then vy# = -6.5       ; si se queda parada, un impulso
  EndIf
  SetGadgetText %s, "x = " + Str$(Int(x#))
  %s_Redibuja()
End Function
]], lienzo.nombre, lienzo.nombre, lienzo.nombre, lienzo.nombre, lienzo.nombre, marcador.nombre, lienzo.nombre)
      return p
    end },
}

function M.ayudas_de(archivo)
  for _, b in ipairs(M.BARRAS) do if b[1] == archivo then return b[2] end end
  return ""
end
-- Donde empiezan los controles: debajo de la barra de herramientas, si la hay
function M.alto_barra(p) return p.barra and M.ALTO_BARRA or 0 end

-- El siguiente nombre libre para un tipo: Boton1, Boton2...
function M.nombre_libre(p, tipo)
  local prefijo = M.TIPOS[tipo].prefijo
  local n = 1
  local usados = {}
  for _, c in ipairs(p.controles) do usados[c.nombre] = true end
  while usados[prefijo .. n] do n = n + 1 end
  return prefijo .. n
end

-- Añade un control con los valores de su tipo; devuelve el control
function M.anadir(p, tipo, x, y)
  local t = assert(M.TIPOS[tipo], "tipo desconocido: " .. tostring(tipo))
  local c = { tipo = tipo, nombre = M.nombre_libre(p, tipo), x = x or 10, y = y or 10, ancho = t.ancho, alto = t.alto,
              texto = t.texto, pin = t.pin, canal = t.canal, imagen = t.imagen,
              ip = t.ip, puerto = t.puerto, ruta = t.ruta, cada = t.cada }
  p.controles[#p.controles + 1] = c
  return c
end

function M.buscar(p, nombre)
  for i, c in ipairs(p.controles) do if c.nombre == nombre then return c, i end end
end

function M.quitar(p, nombre)
  local _, i = M.buscar(p, nombre)
  if i then table.remove(p.controles, i) end
end

-- ¿Es un nombre valido de Nemo Basic? (letra, luego letras, cifras o _)
function M.nombre_valido(n) return type(n) == "string" and n:match("^[%a][%w_]*$") ~= nil end

-- ---------------------------------------------------------------------
-- El codigo del usuario
-- ---------------------------------------------------------------------
-- Las funciones que ha escrito: tabla nombre -> true
function M.funciones(codigo)
  local f = {}
  for linea in (codigo .. "\n"):gmatch("(.-)\n") do
    local n = linea:match("^%s*[Ff]unction%s+([%a][%w_]*)")
    if n then f[n] = true end
  end
  return f
end

-- Las lineas "Global ..." del usuario que estan FUERA de sus funciones:
-- lista de { linea = n, texto = "Global x" }
function M.globales_usuario(codigo)
  local r, dentro, n = {}, false, 0
  for linea in (codigo .. "\n"):gmatch("(.-)\n") do
    n = n + 1
    if linea:match("^%s*[Ff]unction%s") then dentro = true
    elseif linea:match("^%s*[Ee]nd%s+[Ff]unction") then dentro = false
    elseif not dentro and (linea:match("^%s*[Gg]lobal%s") or linea:match("^%s*[Ii]nclude[%s\"]")) then
      -- los Include tambien suben: las funciones y Global de sus
      -- archivos quedan disponibles desde el principio
      r[#r + 1] = { linea = n, texto = linea:gsub("^%s+", "") }
    end
  end
  return r
end

-- El codigo del usuario con esas lineas cambiadas por un comentario
function M.sin_globales(codigo)
  local quitar = {}
  for _, g in ipairs(M.globales_usuario(codigo)) do quitar[g.linea] = true end
  local t, n = {}, 0
  for linea in (codigo .. "\n"):gmatch("(.-)\n") do
    n = n + 1
    t[#t + 1] = quitar[n] and ("; (" .. linea:gsub("^%s+", "") .. ": subido al principio del programa)") or linea
  end
  if t[#t] == "" then t[#t] = nil end
  return table.concat(t, "\n") .. "\n"
end

-- El nombre de la funcion de evento de un control ("Boton1_Click"), o nil
function M.funcion_evento(c)
  local t = M.TIPOS[c.tipo]
  return t and t.evento and (c.nombre .. "_" .. t.evento) or nil
end

-- Si una funcion no existe, la añade al final del codigo; devuelve la linea
-- (del codigo del usuario) donde esta. 'con_valor': recibe (valor).
function M.asegurar_funcion_nombre(p, nombre, parametro)
  return M.asegurar_funcion(p, { nombre = "", tipo = "_funcion", _funcion = nombre, _param = parametro })
end

-- Si la funcion de evento de un control no existe, la añade al final del
-- codigo. Devuelve la linea (del codigo del usuario) donde esta.
function M.asegurar_funcion(p, c)
  local nombre = c._funcion or M.funcion_evento(c)
  if not nombre then return nil end
  local linea_n = 0
  for linea in (p.codigo .. "\n"):gmatch("(.-)\n") do
    linea_n = linea_n + 1
    if linea:match("^%s*[Ff]unction%s+" .. nombre .. "%s*%(") then return linea_n end
  end
  local param = c._funcion and (c._param or "") or ((M.TIPOS[c.tipo].evento == "Cambia") and "valor" or "")
  if p.codigo ~= "" and not p.codigo:match("\n$") then p.codigo = p.codigo .. "\n" end
  if p.codigo ~= "" then p.codigo = p.codigo .. "\n" end
  local antes = select(2, p.codigo:gsub("\n", "")) + ((p.codigo == "") and 0 or 0)
  p.codigo = p.codigo .. "Function " .. nombre .. "(" .. param .. ")\n  \nEnd Function\n"
  return antes + 2                        -- la linea en blanco de dentro, donde se escribe
end

-- ---------------------------------------------------------------------
-- Guardar y abrir (.anx)
-- ---------------------------------------------------------------------
-- Formato de texto, legible y editable a mano:
--   ; Aronnax 1
--   ventana titulo="Mi programa" x=100 y=100 ancho=400 alto=300 maximizar=1 minimizar=1 cerrar=1
--   control tipo=Boton nombre=Boton1 x=20 y=20 ancho=90 alto=26 texto="Aceptar"
--   ---- codigo ----
--   (el codigo del usuario, tal cual, hasta el final)
local function cita(s) return '"' .. tostring(s):gsub('\\', '\\\\'):gsub('"', '\\"') .. '"' end
local function descita(s) return (s:gsub('\\(.)', '%1')) end

local CAMPOS = { "tipo", "nombre", "x", "y", "ancho", "alto", "texto", "pin", "canal", "imagen", "ancla",
                 "ip", "puerto", "ruta", "cada" }

function M.escribir(p)
  local v = p.ventana
  local t = { "; Aronnax 1",
    string.format("ventana titulo=%s x=%d y=%d ancho=%d alto=%d maximizar=%d minimizar=%d cerrar=%d",
      cita(v.titulo), v.x, v.y, v.ancho, v.alto, v.maximizar and 1 or 0, v.minimizar and 1 or 0, v.cerrar and 1 or 0) }
  for _, c in ipairs(p.controles) do
    local partes = { "control" }
    for _, k in ipairs(CAMPOS) do
      local val = c[k]
      if val ~= nil then partes[#partes + 1] = k .. "=" .. ((type(val) == "string") and cita(val) or tostring(val)) end
    end
    t[#t + 1] = table.concat(partes, " ")
  end
  for _, m in ipairs(p.menus or {}) do
    t[#t + 1] = "menu texto=" .. cita(m.texto)
    for _, e in ipairs(m.entradas) do
      t[#t + 1] = "entrada texto=" .. cita(e.texto) .. (e.nombre and (" nombre=" .. cita(e.nombre)) or "")
      for _, s in ipairs(e.entradas or {}) do
        t[#t + 1] = "subentrada texto=" .. cita(s.texto) .. (s.nombre and (" nombre=" .. cita(s.nombre)) or "")
      end
    end
  end
  if p.barra then t[#t + 1] = "barra archivo=" .. cita(p.barra.archivo) .. " ayudas=" .. cita(p.barra.ayudas or "") end
  if (p.temporizador or 0) > 0 then t[#t + 1] = "temporizador hz=" .. p.temporizador end
  t[#t + 1] = "---- codigo ----"
  return table.concat(t, "\n") .. "\n" .. p.codigo
end

local function campos(linea)
  local r = {}
  local i = 1
  while i <= #linea do
    local k, resto = linea:match("^%s*([%a_]+)=()", i)
    if not k then break end
    i = resto
    if linea:sub(i, i) == '"' then
      local j = i + 1
      while j <= #linea and linea:sub(j, j) ~= '"' do if linea:sub(j, j) == "\\" then j = j + 1 end; j = j + 1 end
      r[k] = descita(linea:sub(i + 1, j - 1)); i = j + 1
    else
      local v, fin = linea:match("^(%S*)()", i)
      r[k] = tonumber(v) or v; i = fin
    end
  end
  return r
end

-- Devuelve el proyecto, o nil y el motivo
function M.leer(texto)
  local p = M.nuevo()
  local cab, codigo = texto:match("^(.-)\n%-%-%-%- codigo %-%-%-%-\n?(.*)$")
  if not cab then return nil, "no es un proyecto de Aronnax (falta la linea '---- codigo ----')" end
  p.codigo = codigo
  for linea in (cab .. "\n"):gmatch("(.-)\n") do
    local que, resto = linea:match("^(%a+)%s(.*)$")
    if que == "ventana" then
      local r = campos(resto)
      local v = p.ventana
      v.titulo = r.titulo or v.titulo; v.x = r.x or v.x; v.y = r.y or v.y; v.ancho = r.ancho or v.ancho; v.alto = r.alto or v.alto
      v.maximizar = r.maximizar ~= 0; v.minimizar = r.minimizar ~= 0; v.cerrar = r.cerrar ~= 0
    elseif que == "control" then
      local r = campos(resto)
      if not M.TIPOS[r.tipo] then return nil, "tipo de control desconocido: " .. tostring(r.tipo) end
      local c = {}
      for _, k in ipairs(CAMPOS) do c[k] = r[k] end
      p.controles[#p.controles + 1] = c
    elseif que == "menu" then
      local r = campos(resto)
      p.menus[#p.menus + 1] = { texto = r.texto or "Menu", entradas = {} }
    elseif que == "entrada" then
      local r = campos(resto)
      local m = p.menus[#p.menus]
      if not m then return nil, "una entrada de menu sin menu" end
      m.entradas[#m.entradas + 1] = { texto = r.texto or "", nombre = r.nombre }
    elseif que == "subentrada" then
      local r = campos(resto)
      local m = p.menus[#p.menus]
      local madre = m and m.entradas[#m.entradas]
      if not madre then return nil, "una subentrada sin entrada" end
      madre.entradas = madre.entradas or {}
      madre.entradas[#madre.entradas + 1] = { texto = r.texto or "", nombre = r.nombre }
    elseif que == "barra" then
      local r = campos(resto)
      p.barra = { archivo = r.archivo, ayudas = r.ayudas or "" }
    elseif que == "temporizador" then
      local r = campos(resto)
      p.temporizador = tonumber(r.hz) or 0
    end
  end
  return p
end

-- ---------------------------------------------------------------------
-- Comprobar el diseño antes de generar
-- ---------------------------------------------------------------------
-- Devuelve una lista de problemas (vacia si todo esta bien)
function M.comprobar(p)
  -- DOS listas, y la diferencia importa: 'prob' impide generar el
  -- programa y 'avisos' no. Meter un aviso en 'prob' deja a Aronnax sin
  -- poder compilar nada -- paso al primer intento, con la nota de las
  -- cajas de texto: cualquier proyecto con un control Texto se quedaba
  -- sin generar. Un aviso que bloquea no es un aviso, es un error mal
  -- redactado.
  local prob, avisos, nombres, pines = {}, {}, {}, {}
  local puertos, n_sensores, n_textos = {}, 0, 0
  local fun_usuario = M.funciones(p.codigo)
  for _, c in ipairs(p.controles) do
    if not M.nombre_valido(c.nombre) then prob[#prob + 1] = "nombre no valido: " .. tostring(c.nombre) end
    if nombres[c.nombre:lower()] then prob[#prob + 1] = "nombre repetido: " .. c.nombre end
    nombres[c.nombre:lower()] = true
    local t = M.TIPOS[c.tipo]
    if t.gpio and t.gpio ~= "analogico" then
      local pin = tonumber(c.pin)
      if not pin or pin < 2 or pin > 27 or pin == 14 or pin == 15 then prob[#prob + 1] = c.nombre .. ": pin no valido (2 a 27, salvo 14 y 15)"
      elseif pines[pin] then prob[#prob + 1] = c.nombre .. ": el pin " .. pin .. " ya lo usa " .. pines[pin]
      else pines[pin] = c.nombre end
      if t.gpio == "pwm" and not M.PINES_PWM[pin] then prob[#prob + 1] = c.nombre .. ": el PWM solo existe en los pines 12, 13, 18 y 19" end
    end
    if c.tipo == "Texto" then n_textos = n_textos + 1 end
    if c.tipo == "Imagen" and (c.imagen or "") ~= "" and not c.imagen:lower():match("%.nimg$") then
      prob[#prob + 1] = c.nombre .. ": la imagen tiene que ser un archivo .nimg"
    end
    if t.gpio == "analogico" then
      local canal = tonumber(c.canal)
      if not canal or canal < 0 or canal > 7 then prob[#prob + 1] = c.nombre .. ": canal no valido (0 a 7)" end
    end
    -- Telemetria
    if t.red then
      local puerto = tonumber(c.puerto)
      if not puerto or puerto < 1 or puerto > 65535 then
        prob[#prob + 1] = c.nombre .. ": puerto no valido (1 a 65535)"
      elseif puerto == 68 then
        prob[#prob + 1] = c.nombre .. ": el puerto 68 es del sistema (DHCP)"
      end
    end
    if t.red == "udp" then
      local puerto = tonumber(c.puerto)
      -- Dos sockets en el mismo puerto se repartirian los datagramas por
      -- casualidad: el kernel rechaza el segundo, y entonces uno de los
      -- dos controles no recibiria NADA sin decir por que.
      if puerto and puertos[puerto] then
        prob[#prob + 1] = c.nombre .. ": el puerto " .. puerto .. " ya lo usa " .. puertos[puerto]
      elseif puerto then puertos[puerto] = c.nombre end
    end
    if t.red == "http" then
      n_sensores = n_sensores + 1
      if (c.ip or "") == "" then prob[#prob + 1] = c.nombre .. ": falta la direccion del servidor" end
      if not (c.ruta or ""):match("^/") then prob[#prob + 1] = c.nombre .. ": la ruta tiene que empezar por /" end
      local cada = tonumber(c.cada) or 0
      if cada < 0 then prob[#prob + 1] = c.nombre .. ": el periodo no puede ser negativo" end
      -- Con periodo pero SIN la funcion que da el valor, el control no
      -- mandaria nada nunca y no habria ningun error: es justo el tipo de
      -- silencio que hay que convertir en un aviso.
      if cada > 0 and not fun_usuario[c.nombre .. "_Valor"] then
        avisos[#avisos + 1] = c.nombre .. ": con periodo hace falta Function " .. c.nombre ..
                              "_Valor$(), que devuelve lo que se manda -- sin ella no mandara nada"
      end
    end
  end
  -- En todo Nemo OS hay UN SOLO cliente HTTP, asi que dos sensores no
  -- pueden estar mandando a la vez: el segundo recibiria un no. Funciona
  -- igual --cada uno espera su turno-- pero conviene decirlo, porque con
  -- periodos cortos se pierden lecturas y el motivo no se ve por ningun
  -- lado.
  if n_sensores > 1 then
    avisos[#avisos + 1] = "hay " .. n_sensores .. " sensores: solo puede haber UNA peticion HTTP a la vez en todo el sistema, " ..
                          "asi que se turnaran y con periodos cortos se perderan lecturas"
  end
  -- Las cajas de texto.
  --
  -- El kernel tiene CUATRO en todo el sistema (TEXTAREA_CAJAS en
  -- gadgets.c) y se reparten entre TODOS los programas abiertos, no por
  -- programa. Cuando no queda hueco, CreateTextArea devuelve un
  -- identificador VALIDO igualmente: el control sale en pantalla, se
  -- puede pinchar, y no guarda nada de lo que se escriba. Ni un error ni
  -- un aviso.
  --
  -- Por eso se dice desde la PRIMERA: no depende solo de este programa.
  -- Con el IDE o Timonel abiertos ya hay cajas cogidas, y un programa que
  -- funcionaba solo deja de funcionar acompañado.
  if n_textos > 0 then
    if n_textos > M.CAJAS_TEXTO then
      prob[#prob + 1] = "hay " .. n_textos .. " cajas de texto y en todo el sistema solo caben " ..
                        M.CAJAS_TEXTO .. ": las de mas apareceran en pantalla pero NO guardaran nada"
    else
      avisos[#avisos + 1] = "en todo el sistema solo hay " .. M.CAJAS_TEXTO ..
                            " cajas de texto, repartidas entre todos los programas abiertos " ..
                            "(el IDE y Timonel usan una cada uno). Sin hueco, la caja sale pero no guarda nada"
    end
    -- Y aunque haya hueco, guarda menos de lo que parece. Es el motivo de
    -- que Timonel lleve su propio editor.
    avisos[#avisos + 1] = "una caja de texto guarda " .. M.LINEAS_TEXTO .. " lineas de " ..
                          M.LARGO_LINEA_TEXTO .. " letras; al llenarse DESCARTA la mas antigua y parte las lineas largas. " ..
                          "Si tu programa guarda lo que se escriba ahi, guardara menos de lo que el usuario escribio"
  end
  for _, e in ipairs(M.todas_las_entradas(p)) do
    if e.nombre then
      if not M.nombre_valido(e.nombre) then prob[#prob + 1] = "nombre de menu no valido: " .. tostring(e.nombre) end
      if nombres[e.nombre:lower()] then prob[#prob + 1] = "nombre repetido: " .. e.nombre end
      nombres[e.nombre:lower()] = true
    end
  end
  if (p.temporizador or 0) < 0 or (p.temporizador or 0) > 100 then prob[#prob + 1] = "temporizador: de 1 a 100 veces por segundo (0 = ninguno)" end
  -- Se devuelven las dos. Quien solo coja la primera --todo el codigo que
  -- ya existia-- sigue funcionando igual.
  return prob, avisos
end

-- ---------------------------------------------------------------------
-- Generar el programa
-- ---------------------------------------------------------------------
local function texto_nb(s) return '"' .. tostring(s or ""):gsub('"', "'") .. '"' end   -- Nemo Basic no escapa comillas

-- Devuelve: el programa (texto), y la linea del programa donde empieza el
-- codigo del usuario (para traducir los errores del compilador).
function M.generar(p)
  local L = {}
  local function e(s) L[#L + 1] = s end
  local fun = M.funciones(p.codigo)
  local v = p.ventana
  local hay_sondeo = false
  for _, c in ipairs(p.controles) do
    local g = M.TIPOS[c.tipo].gpio
    if g == "entrada" or g == "analogico" then hay_sondeo = true end
    -- Los de red tambien: hay que mirar la cola y el estado de la
    -- descarga en cada vuelta. Con WaitEvent el programa se quedaria
    -- dormido hasta que alguien tocara un boton, y un mensaje que llega
    -- no es un evento de ventana -- no despertaria a nadie.
    if M.TIPOS[c.tipo].red then hay_sondeo = true end
  end

  e("; Programa generado por Aronnax, el diseñador de Nemo Basic.")
  e("; ---- Parte generada: no la edites aqui; cambia el diseño en Aronnax ----")
  -- (los deslizadores van de 0 a 100: el kernel da de 0 a total - visible,
  -- asi que el total es 101; con 100, un PWM no llegaba nunca al 100 %)
  local globales = {}
  for _, c in ipairs(p.controles) do
    globales[#globales + 1] = c.nombre
    if M.TIPOS[c.tipo].gpio == "entrada" or M.TIPOS[c.tipo].gpio == "analogico" then globales[#globales + 1] = c.nombre .. "_antes" end
    if M.TIPOS[c.tipo].red == "http" then
      globales[#globales + 1] = c.nombre .. "_ocupado"
      globales[#globales + 1] = c.nombre .. "_toca"
    elseif M.TIPOS[c.tipo].red == "udp" then
      -- El socket va en una variable APARTE. El nombre del control es
      -- siempre su gadget, en todo Aronnax, y guardar las dos cosas en
      -- la misma variable hacia que el SetGadgetText de despues usara el
      -- numero del socket como si fuera una etiqueta. Se vio generando.
      globales[#globales + 1] = c.nombre .. "_sock"
    end
  end
  for _, en in ipairs(M.todas_las_entradas(p)) do if en.nombre then globales[#globales + 1] = en.nombre end end
  for _, c in ipairs(p.controles) do
    if (c.ancla or "fijo") ~= "fijo" then globales[#globales + 1] = "aronnax_dw"; globales[#globales + 1] = "aronnax_dh"; break end
  end
  if p.barra then globales[#globales + 1] = "Barra" end
  if (p.temporizador or 0) > 0 then globales[#globales + 1] = "Temporizador" end
  for _, n in ipairs(globales) do e("Global " .. n) end
  -- Los Include y los Global del usuario (fuera de sus funciones) suben aqui,
  -- antes del programa: su codigo va DETRAS del bucle principal.
  -- Primero los Include, en su orden; despues los Global.
  local subidas = M.globales_usuario(p.codigo)
  for _, g in ipairs(subidas) do if g.texto:match("^[Ii]nclude") then e(g.texto) end end
  for _, g in ipairs(subidas) do if not g.texto:match("^[Ii]nclude") then e(g.texto) end end
  e(string.format("CreateWindow %s, %d, %d, %d, %d", texto_nb(v.titulo), v.x, v.y, v.ancho, v.alto))
  if not (v.maximizar and v.minimizar and v.cerrar) then
    e(string.format("WindowButtons %d, %d, %d", v.maximizar and 1 or 0, v.minimizar and 1 or 0, v.cerrar and 1 or 0))
  end
  -- los menus: la etiqueta de cada entrada es su numero de orden (EventData)
  local etiquetas = {}
  if #(p.menus or {}) > 0 then
    e("aronnax_barra_menus = WindowMenu()")
    local n = 0
    for _, m in ipairs(p.menus) do
      e(string.format("aronnax_menu = CreateMenu(%s, 0, aronnax_barra_menus)", texto_nb(m.texto)))
      for _, en in ipairs(m.entradas) do
        if en.nombre then
          n = n + 1
          if not M.tiene_submenu(en) then etiquetas[#etiquetas + 1] = { n, en.nombre } end
          e(string.format("%s = CreateMenu(%s, %d, aronnax_menu)", en.nombre, texto_nb(en.texto), n))
          -- su submenu: las subentradas, con la entrada como padre
          for _, sub in ipairs(en.entradas or {}) do
            if sub.nombre then
              n = n + 1; etiquetas[#etiquetas + 1] = { n, sub.nombre }
              e(string.format("%s = CreateMenu(%s, %d, %s)", sub.nombre, texto_nb(sub.texto), n, en.nombre))
            else
              e(string.format("CreateMenu \"\", 0, %s", en.nombre))
            end
          end
        else
          e("CreateMenu \"\", 0, aronnax_menu")
        end
      end
    end
  end
  if p.barra then
    e(string.format("Barra = CreateToolBar(%s, 0, 0, 0, 0)", texto_nb(p.barra.archivo)))
    if (p.barra.ayudas or "") ~= "" then e(string.format("SetToolBarTips Barra, %s", texto_nb(p.barra.ayudas))) end
  end
  if (p.temporizador or 0) > 0 then e(string.format("Temporizador = CreateTimer(%d)", p.temporizador)) end
  for _, c in ipairs(p.controles) do
    local geo = string.format("%d, %d, %d, %d", c.x, c.y, c.ancho, c.alto)
    local t = c.tipo
    if t == "Boton" then e(string.format("%s = CreateButton(%s, %s)", c.nombre, texto_nb(c.texto), geo))
    elseif t == "Etiqueta" then e(string.format("%s = CreateLabel(%s, %s)", c.nombre, texto_nb(c.texto), geo))
    elseif t == "Campo" then e(string.format("%s = CreateTextField(%s)", c.nombre, geo))
    elseif t == "Casilla" or t == "Interruptor" then e(string.format("%s = CreateCheckBox(%s, %s)", c.nombre, texto_nb(c.texto), geo))
    elseif t == "Opcion" then e(string.format("%s = CreateRadio(%s, %s)", c.nombre, texto_nb(c.texto), geo))
    elseif t == "Lista" then e(string.format("%s = CreateListBox(%s)", c.nombre, geo))
    elseif t == "Desplegable" then e(string.format("%s = CreateComboBox(%s)", c.nombre, geo))
    elseif t == "Pestanas" then e(string.format("%s = CreateTabber(%s)", c.nombre, geo))
    elseif t == "Deslizador" then e(string.format("%s = CreateSlider(%s)", c.nombre, geo)); e(string.format("SetSliderRange %s, 1, 101", c.nombre))
    elseif t == "Progreso" or t == "Analogico" then e(string.format("%s = CreateProgBar(%s)", c.nombre, geo))
    elseif t == "Texto" then e(string.format("%s = CreateTextArea(%s)", c.nombre, geo))
    elseif t == "Panel" then e(string.format("%s = CreatePanel(%s)", c.nombre, geo))
    elseif t == "Arbol" then e(string.format("%s = CreateTreeView(%s)", c.nombre, geo))
    elseif t == "Lienzo" then e(string.format("%s = CreateCanvas(%s)", c.nombre, geo))
    elseif t == "Imagen" then
      e(string.format("%s = CreatePanel(%s)", c.nombre, geo))
      if (c.imagen or "") ~= "" then e(string.format("SetPanelImage %s, %s", c.nombre, texto_nb(c.imagen))) end
    elseif t == "Led" or t == "Pulsador" then
      e(string.format("%s = CreatePanel(%s)", c.nombre, geo)); e(string.format("SetPanelColor %s, 70, 70, 80", c.nombre))
    elseif t == "Pwm" then e(string.format("%s = CreateSlider(%s)", c.nombre, geo)); e(string.format("SetSliderRange %s, 1, 101", c.nombre))
    end
    -- los de red: una etiqueta que dice como va, y el socket abierto
    if t == "Sensor" then
      e(string.format("%s = CreateLabel(%s, %s)", c.nombre, texto_nb("sensor: en espera"), geo))
      e(string.format("%s_ocupado = 0", c.nombre))
      -- La primera lectura no sale nada mas abrir: se deja pasar un
      -- periodo. Mandar en el arranque, antes de que el programa haya
      -- tenido ocasion de leer nada, manda un dato vacio.
      e(string.format("%s_toca = MilliSecs() + %d", c.nombre, c.cada or 1000))
    elseif t == "Mensaje" then
      e(string.format("%s = CreateLabel(%s, %s)", c.nombre, texto_nb("..."), geo))
      -- El socket, con su numero de puerto. Si no se puede abrir se dice
      -- EN LA ETIQUETA: un socket que no abrio y un programa que no
      -- recibe nada se parecen demasiado, y el segundo manda a buscar el
      -- fallo en el otro ordenador.
      e(string.format("%s_sock = UdpOpen(%d)", c.nombre, c.puerto or 7000))
      e(string.format("If %s_sock < 0 Then SetGadgetText %s, %s Else SetGadgetText %s, %s",
                      c.nombre, c.nombre, texto_nb("no se pudo abrir el puerto " .. tostring(c.puerto or 7000)),
                      c.nombre, texto_nb("escuchando en el " .. tostring(c.puerto or 7000))))
    end
    -- los pines
    if t == "Led" or t == "Interruptor" then e(string.format("GpioMode %d, 1", c.pin))
    elseif t == "Pulsador" then e(string.format("GpioMode %d, 2", c.pin)); e(string.format("%s_antes = -1", c.nombre))
    elseif t == "Analogico" then e(string.format("%s_antes = -1", c.nombre))
    end
  end
  if fun.Ventana_Abre then e("Ventana_Abre()") end
  -- anclajes: colocar ya al abrir, por si la ventana no abre en su tamaño
  local hay_anclas = false
  for _, c in ipairs(p.controles) do if (c.ancla or "fijo") ~= "fijo" then hay_anclas = true end end
  if hay_anclas then e("Aronnax_Recolocar()") end
  -- los lienzos: su primer dibujo, ya con los controles creados
  for _, c in ipairs(p.controles) do
    if c.tipo == "Lienzo" and fun[c.nombre .. "_Dibuja"] then e(c.nombre .. "_Redibuja()") end
  end

  -- el bucle
  e("Repeat")
  e(hay_sondeo and "  aronnax_e = PollEvent()" or "  aronnax_e = WaitEvent()")
  -- menus y temporizador
  local hay_menus = false
  for _, t in ipairs(etiquetas) do if fun[t[2] .. "_Click"] then hay_menus = true end end
  if hay_menus then
    e("  If aronnax_e = EVENT_MENUACTION")
    e("    aronnax_dato = EventData()")
    for _, t in ipairs(etiquetas) do
      if fun[t[2] .. "_Click"] then e(string.format("    If aronnax_dato = %d Then %s_Click()", t[1], t[2])) end
    end
    e("  EndIf")
  end
  if (p.temporizador or 0) > 0 and fun.Temporizador_Tick then e("  If aronnax_e = EVENT_TIMERTICK Then Temporizador_Tick()") end
  e("  If aronnax_e = EVENT_GADGETACTION")
  e("    aronnax_fuente = EventSource()")
  if p.barra and fun.Barra_Click then e("    If aronnax_fuente = Barra Then Barra_Click(EventData())") end
  for _, c in ipairs(p.controles) do
    local t, f = c.tipo, M.funcion_evento(c)
    if t == "Interruptor" then
      e(string.format("    If aronnax_fuente = %s", c.nombre))
      e(string.format("      GpioWrite %d, ButtonState(%s)", c.pin, c.nombre))
      if fun[f] then e(string.format("      %s()", f)) end
      e("    EndIf")
    elseif t == "Pwm" then
      e(string.format("    If aronnax_fuente = %s", c.nombre))
      e(string.format("      GpioPwm %d, 1000, SliderValue(%s) * 100", c.pin, c.nombre))
      if fun[f] then e(string.format("      %s(SliderValue(%s))", f, c.nombre)) end
      e("    EndIf")
    elseif f and fun[f] and M.TIPOS[t].gpio == nil and M.TIPOS[t].red == nil and t ~= "Lienzo" then
      -- (un lienzo no tiene eventos; los de red tampoco -- su "evento"
      -- lo dispara la red, no un clic, y se llama desde el sondeo de
      -- abajo con sus argumentos. Sin excluirlos aqui se generaba una
      -- llamada SIN argumentos a una funcion que lleva dos.)
      local arg = ""
      if t == "Deslizador" then arg = "SliderValue(" .. c.nombre .. ")"
      elseif t == "Lista" or t == "Desplegable" or t == "Pestanas" then arg = "SelectedGadgetItem(" .. c.nombre .. ")"
      elseif t == "Arbol" then arg = "SelectedTreeViewNode(" .. c.nombre .. ")"
      elseif t == "Campo" then arg = "0" end
      e(string.format("    If aronnax_fuente = %s Then %s(%s)", c.nombre, f, arg))
    end
  end
  e("  EndIf")
  if hay_sondeo then
    e("  ; las entradas del Nautilus, unas 50 veces por segundo")
    for _, c in ipairs(p.controles) do
      local f = M.funcion_evento(c)
      if c.tipo == "Pulsador" then
        -- con resistencia a positivo, pulsado lee 0: el valor es 1 - lectura
        e(string.format("  aronnax_v = 1 - GpioRead(%d)", c.pin))
        e(string.format("  If aronnax_v <> %s_antes", c.nombre))
        e(string.format("    %s_antes = aronnax_v", c.nombre))
        e(string.format("    If aronnax_v = 1 Then SetPanelColor %s, 240, 190, 40 Else SetPanelColor %s, 70, 70, 80", c.nombre, c.nombre))
        if fun[f] then e(string.format("    %s(aronnax_v)", f)) end
        e("  EndIf")
      elseif c.tipo == "Analogico" then
        e(string.format("  aronnax_r$ = SpiTransfer$(Chr$(1) + Chr$(%d) + Chr$(0), 0, 1000000, 0)", 128 + c.canal * 16))
        e("  aronnax_v = (Asc(Mid$(aronnax_r$, 2, 1)) Mod 4) * 256 + Asc(Mid$(aronnax_r$, 3, 1))")
        e(string.format("  If aronnax_v <> %s_antes", c.nombre))
        e(string.format("    %s_antes = aronnax_v", c.nombre))
        e(string.format("    UpdateProgBar %s, aronnax_v / 1023.0", c.nombre))
        if fun[f] then e(string.format("    %s(aronnax_v)", f)) end
        e("  EndIf")
      end
    end
    -- ---- Telemetria ----
    for _, c in ipairs(p.controles) do
      local t, f = c.tipo, M.funcion_evento(c)
      if t == "Mensaje" then
        -- SE VACIA LA COLA ENTERA, no un datagrama. Leyendo uno por
        -- vuelta, con tres maquinas hablando la cola crece mas rapido de
        -- lo que se vacia: al llenarse se tiran los que llegan y se ven
        -- los mensajes con retraso. No da ningun error; solo va mal.
        --
        -- Y la condicion es UdpPending, no otra cosa: UdpFromPort se
        -- queda enganchado en el ultimo leido, asi que usarlo como "ya no
        -- hay mas" da un bucle QUE NO TERMINA NUNCA en cuanto llega el
        -- primer mensaje. Paso de verdad escribiendolo a mano.
        e(string.format("  If %s_sock >= 0", c.nombre))
        e(string.format("    While UdpPending(%s_sock) > 0", c.nombre))
        e(string.format("      aronnax_m$ = UdpRecv$(%s_sock)", c.nombre))
        if fun[f] then
          -- De quien era ESTE datagrama: se pregunta pegado al UdpRecv$
          -- que lo trajo. Guardarlo "para despues" es como se acaba
          -- contestandole al remitente equivocado, y eso no da error.
          e(string.format("      %s(aronnax_m$, UdpFrom$(%s_sock), UdpFromPort(%s_sock))", f, c.nombre, c.nombre))
        end
        e("    Wend")
        e("  EndIf")
      elseif t == "Sensor" then
        -- La maquina de estados de una peticion HTTP. Hay UNA SOLA
        -- descarga en todo el sistema, asi que hay que mirar si esta
        -- libre antes de empezar otra -- si no, la segunda se pierde en
        -- silencio y el programa cree que la mando.
        e(string.format("  If %s_ocupado = 1", c.nombre))
        e("    If HttpState() <> 1")
        e(string.format("      %s_ocupado = 0", c.nombre))
        e("      aronnax_cod = HttpCode()")
        e(string.format("      If HttpState() = 2 Then SetGadgetText %s, %s + Str$(aronnax_cod) Else SetGadgetText %s, %s + HttpFail$()",
                        c.nombre, texto_nb("ok "), c.nombre, texto_nb("fallo: ")))
        if fun[f] then e(string.format("      %s(aronnax_cod, HttpBody$())", f)) end
        e("    EndIf")
        e("  EndIf")
        -- M.funciones guarda los nombres SIN el $ final (su patron no lo
        -- recoge), asi que se busca sin el y se llama con el.
        if (c.cada or 0) > 0 and fun[c.nombre .. "_Valor"] then
          -- El periodo. El valor que se manda lo da TU funcion
          -- <Nombre>_Valor$(), igual que el evento de un pulsador te
          -- llama a ti: Aronnax no puede saber que quieres mandar.
          e(string.format("  If %s_ocupado = 0 And MilliSecs() >= %s_toca", c.nombre, c.nombre))
          e(string.format("    %s_toca = MilliSecs() + %d", c.nombre, c.cada))
          e(string.format("    %s_Manda(%s_Valor$())", c.nombre, c.nombre))
          e("  EndIf")
        end
      end
    end
    e("  Delay 20")
  end
  if hay_anclas then e("  If aronnax_e = EVENT_WINDOWSIZE Then Aronnax_Recolocar()") end
  e("Until aronnax_e = EVENT_WINDOWCLOSE")
  e("End")
  -- ---- Telemetria: lo que el programador llama ----
  --
  -- Estas funciones son la razon de ser de los dos controles. Cada una
  -- envuelve algo que se hace SIEMPRE igual y que se hace mal una de
  -- cada dos veces si se escribe a mano.
  for _, c in ipairs(p.controles) do
    local t = c.tipo
    if t == "Sensor" then
      -- <Nombre>_Manda(datos$): arranca un POST. Devuelve 1 si salio.
      --
      -- Comprueba que no hay OTRA peticion en marcha. En todo Nemo OS
      -- hay un solo cliente HTTP, asi que pedir una segunda mientras la
      -- primera sigue devuelve un no -- y sin esta comprobacion el
      -- programa creeria que la mando.
      e(string.format("Function %s_Manda(datos$)", c.nombre))
      e(string.format("  If %s_ocupado = 1 Then Return 0", c.nombre))
      e(string.format("  If HttpPost(%s, %d, %s, datos$) = 0 Then Return 0",
                      texto_nb(c.ip or "10.0.2.2"), c.puerto or 8099, texto_nb(c.ruta or "/sensor")))
      e(string.format("  %s_ocupado = 1", c.nombre))
      e(string.format("  SetGadgetText %s, %s", c.nombre, texto_nb("enviando...")))
      e("  Return 1")
      e("End Function")
    elseif t == "Mensaje" then
      -- <Nombre>_Manda(a$, puerto, texto$): a una maquina concreta.
      --
      -- Reintenta mientras salga -3, que es lo que devuelve la PRIMERA
      -- vez que se le habla a una maquina nueva: falta resolver su MAC
      -- por ARP. No es un error, pero tampoco ha salido el paquete.
      e(string.format("Function %s_Manda(a$, puerto, texto$)", c.nombre))
      e("  Local r, intento")
      e(string.format("  If %s_sock < 0 Then Return -1", c.nombre))
      e("  For intento = 1 To 10")
      e(string.format("    r = UdpSend(%s_sock, a$, puerto, texto$)", c.nombre))
      e("    If r >= 0 Then Return r")
      e("    If r <> -3 Then Return r")
      e("    Delay 5")
      e("  Next")
      e("  Return -3")
      e("End Function")
      -- <Nombre>_Grita(texto$): a TODA la red. Asi se encuentran dos
      -- maquinas que no se conocen, sin teclear ninguna direccion.
      e(string.format("Function %s_Grita(texto$)", c.nombre))
      e(string.format("  If %s_sock < 0 Then Return -1", c.nombre))
      e(string.format("  Return UdpSend(%s_sock, %s, %d, texto$)",
                      c.nombre, texto_nb("255.255.255.255"), c.puerto or 7000))
      e("End Function")
      -- <Nombre>_Responde(texto$): a quien mando el ultimo recibido.
      --
      -- SOLO vale dentro de <Nombre>_Recibido, o justo despues: el
      -- remitente es el del ultimo datagrama LEIDO, y en cuanto se lee
      -- otro cambia. Por eso existe la funcion en vez de dejar que cada
      -- uno se guarde la direccion por su cuenta.
      e(string.format("Function %s_Responde(texto$)", c.nombre))
      e(string.format("  If %s_sock < 0 Then Return -1", c.nombre))
      e(string.format("  If UdpFromPort(%s_sock) = 0 Then Return -1", c.nombre))
      e(string.format("  Return UdpSend(%s_sock, UdpFrom$(%s_sock), UdpFromPort(%s_sock), texto$)",
                      c.nombre, c.nombre, c.nombre))
      e("End Function")
    end
  end
  -- los anclajes: recolocar segun lo que haya crecido la ventana
  if hay_anclas then
    e("Function Aronnax_Recolocar()")
    e(string.format("  aronnax_dw = ClientWidth() - %d", p.ventana.ancho))
    e(string.format("  aronnax_dh = ClientHeight() - %d", p.ventana.alto))
    for _, c in ipairs(p.controles) do
      local mx, my, ew, eh = M.ancla_efecto(c.ancla)
      if mx or my or ew or eh then
        e(string.format("  SetGadgetShape %s, %d%s, %d%s, %d%s, %d%s", c.nombre,
          c.x, mx and " + aronnax_dw" or "", c.y, my and " + aronnax_dh" or "",
          c.ancho, ew and " + aronnax_dw" or "", c.alto, eh and " + aronnax_dh" or ""))
      end
    end
    e("End Function")
  end
  -- los lienzos: Lienzo1_Redibuja() pinta con Lienzo1_Dibuja(), dentro del lienzo
  for _, c in ipairs(p.controles) do
    if c.tipo == "Lienzo" and fun[c.nombre .. "_Dibuja"] then
      e(string.format("Function %s_Redibuja()", c.nombre))
      e(string.format("  SetBuffer CanvasBuffer(%s)", c.nombre))
      e(string.format("  %s_Dibuja()", c.nombre))
      e("  SetBuffer BackBuffer()")
      e("End Function")
    end
  end
  -- funciones de ayuda de los LED: Led1_Pon(1) lo enciende, Led1_Pon(0) lo apaga
  for _, c in ipairs(p.controles) do
    if c.tipo == "Led" then
      e(string.format("Function %s_Pon(v)", c.nombre))
      e(string.format("  GpioWrite %d, v", c.pin))
      e(string.format("  If v Then SetPanelColor %s, 60, 220, 90 Else SetPanelColor %s, 70, 70, 80", c.nombre, c.nombre))
      e("End Function")
    end
  end
  e("; ---- Tu codigo ----")
  local inicio = #L + 1
  -- en su sitio, un comentario: asi los numeros de linea no se mueven
  local codigo = M.sin_globales(p.codigo)
  local texto = table.concat(L, "\n") .. "\n" .. codigo
  if not texto:match("\n$") then texto = texto .. "\n" end
  return texto, inicio
end

-- Traduce una linea de error del programa generado: devuelve ("codigo", n)
-- si esta en el codigo del usuario (linea n de su codigo), o
-- ("generado", n) si esta en la parte que escribe Aronnax.
function M.traducir_linea(inicio, linea)
  if linea >= inicio then return "codigo", linea - inicio + 1 end
  return "generado", linea
end

return M
