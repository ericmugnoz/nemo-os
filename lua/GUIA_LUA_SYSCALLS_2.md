# Referencia de syscalls para Lua en Nemo OS (2)

Segunda parte: gadgets y demás controles, temporizadores, reloj y bancos
de memoria. La primera parte, con sistema, archivos, dibujo, imágenes,
fuentes, teclado y ratón, está en [GUIA_LUA_SYSCALLS.md](GUIA_LUA_SYSCALLS.md).

### Gadgets

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 100 | `SYS_CREATE_BUTTON` | a0=puntero a texto, a1=x, a2=y, a3=(ancho<<16| alto), a4=estilo (0 normal, 2 casilla, 3 radio) -> id de gadget, o -1. La ventana se deduce de la tarea, NO se pasa. |
| 101 | `SYS_CREATE_PANEL` | a0=x, a1=y, a2=(ancho<<16| alto) |
| 102 | `SYS_CREATE_TEXTFIELD` | a0=x, a1=y, a2=(ancho<<16| alto) |
| 103 | `SYS_CREATE_LISTBOX` | a0=x, a1=y, a2=(ancho<<16| alto) |
| 104 | `SYS_GADGET_FREE` | a0=id |
| 105 | `SYS_GADGET_SET_TEXT` | a0=id, a1=puntero a texto |
| 106 | `SYS_GADGET_GET_TEXT` | a0=id, a1=buffer salida, a2=tamaño max -> longitud |
| 107 | `SYS_GADGET_RECT` | Devuelve (x<<48 |  y<<32 |  ancho<<16 |  alto), cada campo de 16 bits |
| 108 | `SYS_GADGET_MOVE` | a0=id, a1=x, a2=y |
| 109 | `SYS_GADGET_RESIZE` | a0=id, a1=ancho, a2=alto |
| 110 | `SYS_GADGET_SHOW` | a0=id, a1=visible (0/1) |
| 111 | `SYS_GADGET_ENABLE` | a0=id, a1=activo (0/1) |
| 112 | `SYS_GADGET_ACTIVATE` | a0=id -- da el foco de teclado (TextField) |
| 113 | `SYS_GADGET_EVENT` | Sin argumentos -> id del gadget que disparo un evento pendiente en la ventana propia (0 si ninguno). Alternativa a POLL_EVENT+GET_EVENT_INFO. |
| 114 | `SYS_LISTBOX_ADD_ITEM` | a0=id, a1=puntero a texto |
| 115 | `SYS_LISTBOX_CLEAR` | a0=id |
| 116 | `SYS_LISTBOX_SELECTED` | a0=id -> indice seleccionado, -1 si ninguno |
| 117 | `SYS_LISTBOX_SELECT` | a0=id, a1=indice |
| 118 | `SYS_LISTBOX_ITEM_COUNT` | a0=id -> cantidad de elementos |
| 119 | `SYS_LISTBOX_ITEM_TEXT` | a0=id, a1=indice, a2=buffer, a3=tamaño max -> longitud |
| 141 | `SYS_BUTTON_STATE` | ButtonState/SetButtonState -- casilla/radio (CreateButton style 2/3) a0=id -> 1/0 |
| 142 | `SYS_SET_BUTTON_STATE` | a0=id, a1=estado (0/1) |
| 144 | `SYS_GADGET_ENABLED` | a0=id -> 1 si el gadget/menu esta activo. |
| 145 | `SYS_TEXTAREA_ADD_TEXT` | a0=id, a1=texto |
| 146 | `SYS_TEXTAREA_LEN` | a0=id, a1=units (1=caracteres,2=lineas) -> longitud |
| 147 | `SYS_TEXTAREA_LINE_LEN` | a0=id, a1=linea -> longitud de esa linea |
| 148 | `SYS_TEXTAREA_LINE_OF_CHAR` | a0=id, a1=indice de caracter -> indice de linea |
| 149 | `SYS_TEXTAREA_GET_TEXT` | a0=id, a1=start, a2=count (-1=hasta el final), a3=buffer salida, a4=tamaño max |
| 157 | `SYS_GADGET_INSERT_ITEM` | a0=id, a1=indice, a2=texto. Inserta un elemento en un ListBox. |
| 158 | `SYS_GADGET_REMOVE_ITEM` | a0=id, a1=indice |
| 159 | `SYS_GADGET_MODIFY_ITEM` | a0=id, a1=indice, a2=texto |
| 160 | `SYS_CREATE_LABEL` | a0=puntero a texto, a1=x, a2=y, a3=(ancho<<16| alto), a4=estilo -> id. |
| 161 | `SYS_CREATE_PROGBAR` | a0=x, a1=y, a2=(ancho<<16| alto) |
| 162 | `SYS_UPDATE_PROGBAR` | a0=id, a1=bits crudos de un double (0.0-1.0) |
| 163 | `SYS_CREATE_SLIDER` | a0=x, a1=y, a2=(ancho<<16| alto), a3=estilo -> id. |
| 164 | `SYS_SET_SLIDER_RANGE` | a0=id, a1=visible, a2=total |
| 165 | `SYS_SET_SLIDER_VALUE` | a0=id, a1=valor |
| 166 | `SYS_SLIDER_VALUE` | a0=id -> valor actual |
| 167 | `SYS_CREATE_COMBOBOX` | a0=x, a1=y, a2=(ancho<<16| alto) -> id. Comparte las syscalls de items (114-119) con ListBox. |
| 168 | `SYS_CREATE_TABBER` | a0=x, a1=y, a2=(ancho<<16| alto) |
| 207 | `SYS_SET_PANEL_COLOR` | a0=id, a1=(r<<16| g<<8| b) |
| 208 | `SYS_LOCK_BUFFER` | a0=buffer. Bloquea el buffer para acceso directo a pixeles (modelo de bloqueo unico global). |
| 209 | `SYS_UNLOCK_BUFFER` | a0=buffer (se ignora -- modelo de bloqueo unico global) |
| 210 | `SYS_LOCKED_PIXELS` | a0=buffer -> handle centinela (o 0 si no hay bloqueo) |
| 211 | `SYS_LOCKED_PITCH` | a0=buffer -> bytes por fila |
| 212 | `SYS_LOCKED_FORMAT` | a0=buffer -> formato de pixel (4, igual que GraphicsFormat) |
| 213 | `SYS_READ_PIXEL_FAST` | a0=x, a1=y, a2=buffer -> color. Exige un buffer bloqueado; mas rapida que READ_PIXEL. |
| 214 | `SYS_WRITE_PIXEL_FAST` | a0=x, a1=y, a2=argb, a3=buffer |
| 215 | `SYS_COPY_PIXEL_FAST` | a0=(src_x<<16| src_y), a1=src_buffer, a2=(dest_x<<16| dest_y), a3=dest_buffer |
| 216 | `SYS_PAUSE_TIMER` | a0=handle |
| 217 | `SYS_RESUME_TIMER` | a0=handle |
| 218 | `SYS_RESET_TIMER` | a0=handle |
| 219 | `SYS_TIMER_TICKS` | a0=handle -> contador de ticks |
| 220 | `SYS_READ_BYTES_BANK` | a0=banco, a1=handle de archivo, a2=offset en el banco, a3=cantidad -> 0/-1. Lee bytes del archivo al banco. |
| 221 | `SYS_WRITE_BYTES_BANK` | a0=banco, a1=handle de archivo, a2=offset en el banco, a3=cantidad -> 0 si ok, -1 si error |
| 222 | `SYS_SET_PANEL_IMAGE` | a0=id de panel, a1=puntero a nombre de archivo (se carga aqui mismo, igual que CreateToolBar) |
| 223 | `SYS_SET_GADGET_GROUP` | a0=id de gadget, a1=handle de grupo |
| 224 | `SYS_GADGET_GROUP` | a0=id de gadget -> handle de grupo (0 si no se asigno) |
| 225 | `SYS_TFORM_IMAGE` | a0=handle de imagen, a1..a4=bits crudos de a#,b#,c#,d# (matriz 2x2) |
| 226 | `SYS_LOAD_SOUND` | a0=puntero a nombre -> handle de sonido, o -1. |
| 227 | `SYS_FREE_SOUND` | a0=handle |
| 228 | `SYS_PLAY_SOUND` | a0=handle (bloquea hasta que termina de sonar) |
| 229 | `SYS_SOUND_VOLUME` | a0=handle, a1=bits crudos de un double (0.0-1.0) |
| 230 | `SYS_SOUND_PAN` | a0=handle, a1=bits crudos de un double (-1.0 a 1.0) |
| 231 | `SYS_SOUND_PITCH` | a0=handle, a1=hercios (entero) |
| 232 | `SYS_EXEC_FILE` | a0=puntero a 'programa argumentos' -> 0 si se lanzo. Como LAUNCH_PROGRAM pero con la linea completa. |
| 233 | `SYS_CREATE_PROCESS` | a0=puntero a "programa argumentos" -> id de tarea (o -1), usado como "stream" |
| 234 | `SYS_FILE_RENAME` | a0=nombre actual, a1=nombre nuevo, a2=carpeta padre, a3=volumen -> 0 exito, -1 fallo (solo NemoFS por ahora) |
| 235 | `SYS_ICON_CATALOG_COUNT` | Sin argumentos -> cuantos iconos hay en el catalogo del sistema. |
| 236 | `SYS_DESKTOP_ICON_COUNT` | -> cuantos iconos hay en el escritorio ahora mismo |
| 237 | `SYS_DESKTOP_ICON_GET` | a0=indice, a1=buffer de 16 (etiqueta), a2=buffer de 32 (target .pro), a3=puntero a 3 int32 (icon_id, x, y) -> 1 exito, 0 no existe |
| 238 | `SYS_DESKTOP_ICON_ADD` | a0=etiqueta, a1=target .pro, a2=icon_id, a3=x, a4=y -> indice nuevo, o -1 |
| 239 | `SYS_DESKTOP_ICON_REMOVE` | a0=indice -> 0 exito, -1 fallo |
| 240 | `SYS_DESKTOP_ICON_MOVE` | a0=indice, a1=x, a2=y -> 0 exito, -1 fallo |
| 241 | `SYS_DESKTOP_ICON_SET_GRAPHIC` | a0=indice, a1=icon_id -> 0 exito, -1 fallo |
| 242 | `SYS_DESKTOP_SAVE` | sin argumentos -- persiste el escritorio actual en disco |
| 243 | `SYS_DESKTOP_ICON_SCALE_GET` | -> 1 (24x24) o 2 (48x48), la escala actual de todo el sistema |
| 244 | `SYS_DESKTOP_ICON_SCALE_SET` | a0=nueva escala (1 o 2) -- la aplica, la persiste, y la comparten escritorio y explorador |
| 245 | `SYS_SCREEN_RES_GET_PENDING` | -> (ancho<<32 |  alto) guardado en SCREEN.CFG (puede no ser el activo ahora mismo, si aun no se ha reiniciado) |
| 246 | `SYS_SCREEN_RES_SET_PENDING` | a0=ancho, a1=alto -- guarda la preferencia; hace falta REINICIAR para que se aplique de verdad |

### Menus

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 120 | `SYS_WINDOW_MENU` | Sin argumentos -> id de la raiz del menu de la ventana propia (se crea la primera vez). |
| 121 | `SYS_CREATE_MENU` | a0=puntero a texto, a1=tag (numero libre), a2=padre (la raiz, u otra entrada para anidar) -> id. Al elegirse dispara 0x1001 con fuente=id. |
| 122 | `SYS_MENU_CHECK` | a0=id, a1=marcado (0/1) |
| 123 | `SYS_MENU_ENABLE` | a0=id, a1=activo (0/1) |
| 124 | `SYS_MENU_GET_TAG` | a0=id -> el 'tag' que se le puso al crearlo |

### TextArea

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 125 | `SYS_CREATE_TEXTAREA` | a0=x, a1=y, a2=(ancho<<16| alto) -> id. Caja de texto multilinea (solo lectura en v1). |
| 126 | `SYS_TEXTAREA_SET_TEXT` | a0=id, a1=puntero a texto -- reemplaza TODO el contenido, partido en lineas por '\n'. |

### Barras de herramientas

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 169 | `SYS_LOAD_ICON_STRIP` | a0=puntero a nombre de imagen, a1=ancho de celda -> handle de tira de iconos. |
| 170 | `SYS_FREE_ICON_STRIP` | a0=handle |
| 171 | `SYS_SET_GADGET_ICON_STRIP` | a0=id de gadget, a1=handle de tira |
| 172 | `SYS_CREATE_TOOLBAR` | a0=handle de imagen YA cargada, a1=x, a2=y, a3=(ancho<<16| alto) |
| 173 | `SYS_ENABLE_TOOLBAR_ITEM` | a0=id, a1=indice, a2=activo (0/1) |
| 174 | `SYS_SET_TOOLBAR_TIPS` | a0=id, a1=texto (separado por comas) |

### TreeView

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 175 | `SYS_CREATE_TREEVIEW` | a0=x, a1=y, a2=(ancho<<16| alto) -> id. Cada nodo es un gadget con handle propio. |
| 176 | `SYS_TREEVIEW_ROOT` | a0=treeview -> id del nodo raiz |
| 177 | `SYS_ADD_TREEVIEW_NODE` | a0=texto, a1=padre -> id del nodo nuevo |
| 178 | `SYS_INSERT_TREEVIEW_NODE` | a0=indice, a1=texto, a2=padre -> id del nodo nuevo |
| 179 | `SYS_MODIFY_TREEVIEW_NODE` | a0=nodo, a1=texto |
| 180 | `SYS_FREE_TREEVIEW_NODE` | a0=nodo |
| 181 | `SYS_EXPAND_TREEVIEW_NODE` | a0=nodo, a1=expandir (1) o colapsar (0) |
| 182 | `SYS_COUNT_TREEVIEW_NODES` | a0=padre -> cantidad de hijos directos |
| 183 | `SYS_SELECTED_TREEVIEW_NODE` | a0=treeview -> id del nodo seleccionado (-1 si ninguno) |
| 184 | `SYS_SELECT_TREEVIEW_NODE` | a0=nodo |

### Temporizadores

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 127 | `SYS_CREATE_TIMER` | a0=hercios -> handle. Dispara EVENT_TIMERTICK (0x4001) en la ventana propia. |
| 131 | `SYS_FREE_TIMER` | a0=handle (el mismo que devolvio CreateTimer) |
| 132 | `SYS_TIMER_READY` | a0=handle -> 1 si ya toca el siguiente disparo (consulta pura, sin efectos) |
| 133 | `SYS_TIMER_CONSUME` | a0=handle -- avanza al siguiente disparo (llamar justo despues de que TIMER_READY de 1) |

### Reloj (RTC)

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 129 | `SYS_RTC_NOW` | Sin argumentos -> timestamp Unix (segundos desde 1970). |
| 134 | `SYS_RTC_CIVIL` | Sin argumentos -> (año<<48 |  mes<<40 |  dia<<32 |  hora<<24 |  minuto<<16 |  segundo). |

### Bancos de memoria

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 59 | `SYS_CREATE_BANK` | a0=tamaño -> handle de banco de memoria, o -1. PEEK/POKE_FLOAT reutilizan PEEK/POKE_INT con los bits crudos del float. |
| 60 | `SYS_FREE_BANK` | a0=handle |
| 61 | `SYS_BANK_SIZE` | a0=handle -> tamaño actual, o 0 si el handle no es valido |
| 62 | `SYS_RESIZE_BANK` | a0=handle, a1=nuevo_tamaño -> 0 si ok, -1 si fallo |
| 63 | `SYS_COPY_BANK` | a0=handle_origen, a1=offset_origen, a2=handle_destino, a3=offset_destino, a4=cantidad |
| 64 | `SYS_PEEK_BYTE` | a0=handle, a1=offset -> valor (0 si fuera de rango) |
| 65 | `SYS_PEEK_SHORT` |  |
| 66 | `SYS_PEEK_INT` |  |
| 67 | `SYS_POKE_BYTE` | a0=handle, a1=offset, a2=valor |
| 68 | `SYS_POKE_SHORT` |  |
| 69 | `SYS_POKE_INT` |  |
