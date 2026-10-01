-- ayuda.lua -- abre la ayuda del sistema.
--
-- No dibuja nada: busca MANUALES/AYUDA, le pide al visor que abra
-- el indice y se va. Es a proposito.
--
-- La alternativa era una aplicacion con su propia ventana que
-- maquetara las paginas ella misma, y eso significa cargar OTRA copia
-- del motor de HTML (nemo_html) en OTRA tarea, con su monton de 4 MB.
-- Ya se midio lo justo que va el visor con documentos grandes (por eso
-- la guia de syscalls esta partida en dos archivos): no tiene sentido
-- duplicar el motor para enseñar unas paginas que el visor ya sabe
-- enseñar. Asi la ayuda no cuesta ni un hueco de tarea ni un byte de
-- memoria una vez abierta.

local arch = require("nemo_archivos")

local SYS_LAUNCH_PROGRAM = 5
local INDICE = "indice.html"

-- MANUALES/AYUDA, tal y como las instala el kernel al arrancar (ver
-- embedded_lua.c). Si algo falta, se dice y se sale: es mejor eso que abrir
-- el visor contra una carpeta que no existe.
--
-- La ayuda vivia en DOCUMENTOS/AYUDA y se movio a MANUALES con
-- el resto de la documentacion. Se prueban las dos por orden, porque una
-- tarjeta que arranque con este Lua y un kernel anterior seguiria teniendola
-- en el sitio viejo, y quedarse sin ayuda por eso seria absurdo.
local function carpeta_ayuda()
  for _, padre in ipairs({ "MANUALES", "DOCUMENTOS" }) do
    local p = arch.buscar(padre, arch.RAIZ)
    if p and arch.es_carpeta_entrada(p) then
      local a = arch.buscar("AYUDA", p.inodo)
      if a and arch.es_carpeta_entrada(a) and arch.buscar(INDICE, a.inodo) then
        return a, padre
      end
    end
  end
  return nil
end

local ayuda, donde = carpeta_ayuda()
if not ayuda then
  print("No encuentro la ayuda: falta MANUALES/AYUDA/" .. INDICE .. ".")
  return
end
if donde ~= "MANUALES" then
  print("Aviso: la ayuda sigue en " .. donde .. "/AYUDA (kernel anterior).")
end

-- Mismo formato que usan el explorador y el Navegante para abrir un
-- documento: "<inodo de la carpeta>:<nombre>".
nemo.syscall(SYS_LAUNCH_PROGRAM, "visor.lua", tostring(ayuda.inodo) .. ":" .. INDICE, ayuda.inodo)
