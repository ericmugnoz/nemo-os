Global suma = 0

Dim numeros(5)
numeros(0) = 10
numeros(1) = 20
numeros(2) = 30

For i = 0 To 2
    suma = suma + numeros(i)
Next

If suma > 50 Then
    Print "Suma grande: " + Str$(suma)
ElseIf suma > 10 Then
    Print "Suma media"
Else
    Print "Suma pequeña"
EndIf

Global contador = 0
While contador < 3
    Print "While: " + Str$(contador)
    contador = contador + 1
Wend

Global r = 0
Repeat
    r = r + 1
Until r >= 4
Print "Repeat termino en: " + Str$(r)

For fila = 1 To 2
    For col = 1 To 2
        Print "Celda " + Str$(fila) + "," + Str$(col)
    Next
Next
