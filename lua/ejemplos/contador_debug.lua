-- contador_debug.lua -- version con diagnostico crudo: imprime todo
-- lo que las syscalls de evento devuelven, para ver si el clic llega
-- a detectarse en absoluto o no.
local S = { CREATE_WINDOW=40, POLL_EVENT=8, GET_EVENT_INFO=9,
            CREATE_BUTTON=100, CREATE_LABEL=160, GADGET_EVENT=113 }

nemo.syscall(S.CREATE_WINDOW, "Debug", 100, 100, 300, 160)
local etiqueta = nemo.syscall(S.CREATE_LABEL, "Pulsaciones: 0", 20, 20, (260<<16)|24, 0)
local boton = nemo.syscall(S.CREATE_BUTTON, "Pulsa aqui", 20, 60, (120<<16)|32, 0)
print("boton creado con id:", boton, " etiqueta id:", etiqueta)

local n = 0
local vueltas = 0
while true do
  nemo.pump()
  vueltas = vueltas + 1

  local ev = nemo.syscall(S.POLL_EVENT)
  if ev == 0x803 then print("cierre, fin"); break end
  if ev ~= 0 then
    local info = nemo.syscall(S.GET_EVENT_INFO)
    local fuente = (info >> 32) & 0xFFFFFFFF
    print("POLL_EVENT:", string.format("0x%x", ev), "fuente:", fuente, "== boton?", fuente == boton)
  end

  local g = nemo.syscall(S.GADGET_EVENT)
  if g ~= 0 then
    print("GADGET_EVENT crudo:", g, "== boton?", g == boton)
  end

  if vueltas % 200 == 0 then
    nemo.syscall(105, etiqueta, "Vueltas: " .. vueltas)  -- SYS_GADGET_SET_TEXT, para ver que el bucle sigue vivo
  end
end
