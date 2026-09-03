root = WindowMenu()
menuArchivo = CreateMenu("ARCHIVO", 0, root)
menuSalir = CreateMenu("SALIR", 99, menuArchivo)

btn = CreateButton("Pulsame", 10, 30, 90, 24)
clics = 0

Repeat
    Pump
    ev = GadgetEvent()
    If ev = btn Then
        clics = clics + 1
        SetGadgetText(btn, "Clics: " + Str$(clics))
    EndIf
    If ev = menuSalir Then
        End
    EndIf
Forever
