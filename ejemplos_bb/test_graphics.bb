Graphics 400, 300
SetBuffer BackBuffer()

x = 50
y = 50
speed = 3

Repeat
    Color 0, 0, 60
    Cls

    If KeyDown(30) Then x = x - speed
    If KeyDown(32) Then x = x + speed
    If KeyDown(17) Then y = y - speed
    If KeyDown(31) Then y = y + speed

    Color 255, 200, 0
    Oval x, y, 30, 30

    Color 255, 255, 255
    Text 10, 10, "x=" + Str$(x) + " y=" + Str$(y)
    Text 10, 24, "ms=" + Str$(MilliSecs())

    If RectsOverlap(x, y, 30, 30, 150, 150, 60, 60) Then Text 10, 40, "COLISION!"

    Flip
Until KeyDown(1)
End
