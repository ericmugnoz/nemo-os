# Referencia de Nemo Basic (4)

*El índice alfabético completo de los 260 comandos.*

Viene de [REFERENCIA_NEMO_BASIC_3.md](REFERENCIA_NEMO_BASIC_3.md).
# 12. Índice alfabético completo

Las **260** funciones y comandos incorporados del lenguaje, con su firma y su
número de llamada al sistema. Un guion en la columna de syscall significa que
se calcula dentro del programa, sin llamar al sistema.

Va como lista y no como tabla a propósito: una tabla de 260 filas no cabe en el
montón de 4 MB del visor.

- **`Abs`** — firma `N`, syscall —
- **`Abs#`** — firma `N`, syscall —
- **`Abs%`** — firma `N`, syscall —
- **`ActivateGadget`** — firma `N`, syscall 112
- **`AddGadgetItem`** — firma `NS`, syscall 114
- **`AddTextAreaText`** — firma `NS`, syscall 145
- **`AddTreeViewNode`** — firma `SN`, syscall 177
- **`Asc`** — firma `S`, syscall —
- **`ATan`** — firma `N`, syscall —
- **`ATan#`** — firma `N`, syscall —
- **`BackBuffer`** — firma `—`, syscall —
- **`Bin$`** — firma `N`, syscall —
- **`ButtonState`** — firma `N`, syscall 141
- **`CanvasBuffer`** — firma `N`, syscall —
- **`CardSectors`** — firma `—`, syscall 279
- **`Ceil`** — firma `N`, syscall —
- **`Ceil#`** — firma `N`, syscall —
- **`CheckMenu`** — firma `N`, syscall 122
- **`Chr$`** — firma `N`, syscall —
- **`ClearGadgetItems`** — firma `N`, syscall 115
- **`ClientHeight`** — firma `—`, syscall 33
- **`ClientWidth`** — firma `—`, syscall 33
- **`CloseDir`** — firma `N`, syscall 80
- **`CloseFile`** — firma `N`, syscall 44
- **`CloseWindow`** — firma `—`, syscall 313
- **`ClsColor`** — firma `NNN`, syscall —
- **`CollapseTreeViewNode`** — firma `N`, syscall 181
- **`CopyImage`** — firma `N`, syscall 93
- **`Cos`** — firma `N`, syscall —
- **`Cos#`** — firma `N`, syscall —
- **`CountGadgetItems`** — firma `N`, syscall 118
- **`CountTreeViewNodes`** — firma `N`, syscall 182
- **`CpuCores`** — firma `—`, syscall 286
- **`CpuMHz`** — firma `—`, syscall 286
- **`CpuName$`** — firma `—`, syscall 285
- **`CreateButton`** — firma `SNNNN`, syscall 100
- **`CreateCanvas`** — firma `NNNN`, syscall 188
- **`CreateCheckBox`** — firma `SNNNN`, syscall 100
- **`CreateComboBox`** — firma `NNNN`, syscall 167
- **`CreateContextMenu`** — firma `—`, syscall 315
- **`CreateDir`** — firma `S`, syscall 24
- **`CreateImage`** — firma `NN`, syscall 52
- **`CreateLabel`** — firma `SNNNN`, syscall 160
- **`CreateListBox`** — firma `NNNN`, syscall 103
- **`CreateMenu`** — firma `SNN`, syscall 121
- **`CreatePanel`** — firma `NNNN`, syscall 101
- **`CreateProgBar`** — firma `NNNN`, syscall 161
- **`CreateRadio`** — firma `SNNNN`, syscall 100
- **`CreateScrollBar`** — firma `NNNNN`, syscall 314
- **`CreateSlider`** — firma `NNNNN`, syscall 163
- **`CreateTabber`** — firma `NNNN`, syscall 168
- **`CreateTextArea`** — firma `NNNN`, syscall 125
- **`CreateTextField`** — firma `NNNN`, syscall 102
- **`CreateTimer`** — firma `N`, syscall 127
- **`CreateToolBar`** — firma `SNNNN`, syscall 172
- **`CreateTreeView`** — firma `NNNN`, syscall 175
- **`Day`** — firma `—`, syscall 134
- **`DeleteFile`** — firma `S`, syscall 84
- **`DisableGadget`** — firma `N`, syscall 111
- **`DisableMenu`** — firma `N`, syscall 123
- **`DisableToolBarItem`** — firma `NN`, syscall 173
- **`DiskTotalBlocks`** — firma `—`, syscall 250
- **`DiskUsedBlocks`** — firma `—`, syscall 250
- **`DrawBlock`** — firma `NNN`, syscall 50
- **`DrawImage`** — firma `NNN`, syscall 50
- **`EnableGadget`** — firma `N`, syscall 111
- **`EnableMenu`** — firma `N`, syscall 123
- **`EnableToolBarItem`** — firma `NN`, syscall 173
- **`Eof`** — firma `N`, syscall 43
- **`EventData`** — firma `—`, syscall 9
- **`EventSource`** — firma `—`, syscall 9
- **`EventX`** — firma `—`, syscall 256
- **`EventY`** — firma `—`, syscall 256
- **`Exp`** — firma `N`, syscall —
- **`Exp#`** — firma `N`, syscall —
- **`ExpandTreeViewNode`** — firma `N`, syscall 181
- **`FileExists`** — firma `S`, syscall 82
- **`FileLength`** — firma `N`, syscall 75
- **`FilePos`** — firma `N`, syscall 73
- **`FileSize`** — firma `S`, syscall 81
- **`FileType`** — firma `S`, syscall 82
- **`FillRow`** — firma `NNNNN`, syscall 291
- **`FlipCanvas`** — firma `N`, syscall —
- **`Float`** — firma `N`, syscall —
- **`Float#`** — firma `N`, syscall —
- **`Floor`** — firma `N`, syscall —
- **`Floor#`** — firma `N`, syscall —
- **`FontCharWidth`** — firma `—`, syscall 206
- **`FontHeight`** — firma `—`, syscall 196
- **`FreeGadget`** — firma `N`, syscall 104
- **`FreeImage`** — firma `N`, syscall 88
- **`FreeSound`** — firma `N`, syscall 227
- **`FreeTimer`** — firma `N`, syscall 131
- **`FreeTreeViewNode`** — firma `N`, syscall 180
- **`FrontBuffer`** — firma `—`, syscall —
- **`GadgetGroup`** — firma `N`, syscall 224
- **`GadgetHeight`** — firma `N`, syscall 107
- **`GadgetText$`** — firma `N`, syscall 106
- **`GadgetWidth`** — firma `N`, syscall 107
- **`GadgetX`** — firma `N`, syscall 107
- **`GadgetY`** — firma `N`, syscall 107
- **`GetKey`** — firma `—`, syscall 54
- **`GpioMode`** — firma `NN`, syscall 264
- **`GpioPwm`** — firma `NNN`, syscall 267
- **`GpioRead`** — firma `N`, syscall 266
- **`GpioWrite`** — firma `NN`, syscall 265
- **`HandleImage`** — firma `NNN`, syscall 89
- **`Hex$`** — firma `N`, syscall —
- **`HideGadget`** — firma `N`, syscall 110
- **`Hour`** — firma `—`, syscall 134
- **`HttpBody$`** — firma `—`, syscall 308
- **`HttpCode`** — firma `—`, syscall 297
- **`HttpFail$`** — firma `—`, syscall 299
- **`HttpGet`** — firma `SNS`, syscall 307
- **`HttpPost`** — firma `SNSS`, syscall 307
- **`HttpState`** — firma `—`, syscall 297
- **`I2cRead$`** — firma `NN`, syscall 269
- **`I2cWrite`** — firma `NS`, syscall 268
- **`ImageBuffer`** — firma `N`, syscall —
- **`ImageHeight`** — firma `N`, syscall 51
- **`ImageWidth`** — firma `N`, syscall 51
- **`Input`** — firma `—`, syscall 258
- **`Input$`** — firma `—`, syscall 258
- **`InsertGadgetItem`** — firma `NNS`, syscall 157
- **`InsertTreeViewNode`** — firma `NSN`, syscall 178
- **`Instr`** — firma `SSN`, syscall —
- **`Int`** — firma `N`, syscall —
- **`Int%`** — firma `N`, syscall —
- **`KernelHeapTotal`** — firma `—`, syscall 252
- **`KernelHeapUsed`** — firma `—`, syscall 252
- **`KernelRam`** — firma `—`, syscall 288
- **`KeyBank`** — firma `N`, syscall 273
- **`KeyDown`** — firma `N`, syscall 48
- **`KeyHit`** — firma `N`, syscall 53
- **`Left$`** — firma `SN`, syscall —
- **`Len`** — firma `S`, syscall —
- **`LoadAnimImage`** — firma `SNNNN`, syscall 99
- **`LoadImage`** — firma `S`, syscall 49
- **`LoadSound`** — firma `S`, syscall 226
- **`Log`** — firma `N`, syscall —
- **`Log#`** — firma `N`, syscall —
- **`Lower$`** — firma `S`, syscall —
- **`LSet$`** — firma `SN`, syscall —
- **`MaskImage`** — firma `NN`, syscall 92
- **`Max`** — firma `NN`, syscall —
- **`MicroSecs`** — firma `—`, syscall 272
- **`Mid$`** — firma `SNN`, syscall —
- **`MilliSecs`** — firma `—`, syscall 2
- **`Min`** — firma `NN`, syscall —
- **`Minute`** — firma `—`, syscall 134
- **`ModifyGadgetItem`** — firma `NNS`, syscall 159
- **`ModifyTreeViewNode`** — firma `NS`, syscall 179
- **`Month`** — firma `—`, syscall 134
- **`MouseDown`** — firma `—`, syscall 34
- **`MouseHit`** — firma `N`, syscall 56
- **`MouseX`** — firma `—`, syscall 34
- **`MouseY`** — firma `—`, syscall 34
- **`NetDns$`** — firma `—`, syscall 309
- **`NetGateway$`** — firma `—`, syscall 309
- **`NetIp$`** — firma `—`, syscall 309
- **`NetMask$`** — firma `—`, syscall 309
- **`NetReady`** — firma `—`, syscall 310
- **`NextFile$`** — firma `N`, syscall 79
- **`OpenFile`** — firma `S`, syscall 41
- **`PartitionSectors`** — firma `N`, syscall 280
- **`PartitionStart`** — firma `N`, syscall 280
- **`PartitionType`** — firma `N`, syscall 280
- **`PauseTimer`** — firma `N`, syscall 216
- **`PlaySound`** — firma `N`, syscall 228
- **`PollEvent`** — firma `—`, syscall 8
- **`Pump`** — firma `—`, syscall 14
- **`Rand`** — firma `NN`, syscall —
- **`ReadChar`** — firma `—`, syscall 12
- **`ReadDir`** — firma `S`, syscall 83
- **`ReadLine$`** — firma `N`, syscall 42
- **`ReadRow`** — firma `NNNN`, syscall 289
- **`RemoveGadgetItem`** — firma `NN`, syscall 158
- **`RenameFile`** — firma `SS`, syscall 312
- **`Replace$`** — firma `SSS`, syscall —
- **`ResetTimer`** — firma `N`, syscall 218
- **`ResumeTimer`** — firma `N`, syscall 217
- **`Right$`** — firma `SN`, syscall —
- **`Rnd`** — firma `N`, syscall —
- **`RowRun`** — firma `NNNNN`, syscall 292
- **`RowSkip`** — firma `NNNNN`, syscall 292
- **`RSet$`** — firma `SN`, syscall —
- **`SaveImage`** — firma `NS`, syscall 94
- **`ScreenHeight`** — firma `—`, syscall 35
- **`ScreenWidth`** — firma `—`, syscall 35
- **`Second`** — firma `—`, syscall 134
- **`Seed`** — firma `N`, syscall —
- **`SeekFile`** — firma `NN`, syscall 74
- **`SelectedGadgetItem`** — firma `N`, syscall 116
- **`SelectedTreeViewNode`** — firma `N`, syscall 183
- **`SelectGadgetItem`** — firma `NN`, syscall 117
- **`SelectTreeViewNode`** — firma `N`, syscall 184
- **`SetBuffer`** — firma `N`, syscall 128
- **`SetButtonState`** — firma `NN`, syscall 142
- **`SetGadgetGroup`** — firma `NN`, syscall 223
- **`SetGadgetShape`** — firma `NNNNN`, syscall 108, 109
- **`SetGadgetText`** — firma `NS`, syscall 105
- **`SetPanelColor`** — firma `NNNN`, syscall 207
- **`SetPanelImage`** — firma `NS`, syscall 222
- **`SetSliderRange`** — firma `NNN`, syscall 164
- **`SetSliderValue`** — firma `NN`, syscall 165
- **`SetTextAreaText`** — firma `NS`, syscall 126
- **`SetToolBarTips`** — firma `NS`, syscall 174
- **`Sgn`** — firma `N`, syscall —
- **`ShowContextMenu`** — firma `NNN`, syscall 316
- **`ShowGadget`** — firma `N`, syscall 110
- **`Sin`** — firma `N`, syscall —
- **`Sin#`** — firma `N`, syscall —
- **`SliderValue`** — firma `N`, syscall 166
- **`SoundPan`** — firma `NN`, syscall 230
- **`SoundPitch`** — firma `NN`, syscall 231
- **`SoundVolume`** — firma `NN`, syscall 229
- **`SpiTransfer$`** — firma `SNNN`, syscall 270
- **`Sqr`** — firma `N`, syscall —
- **`Sqr#`** — firma `N`, syscall —
- **`Str$`** — firma `N`, syscall —
- **`String$`** — firma `SN`, syscall —
- **`Tan`** — firma `N`, syscall —
- **`Tan#`** — firma `N`, syscall —
- **`TaskCount`** — firma `—`, syscall 251
- **`TaskName$`** — firma `N`, syscall 295
- **`TaskSlot`** — firma `N`, syscall 294
- **`TaskSlots`** — firma `—`, syscall 251
- **`TaskTurns`** — firma `N`, syscall 294
- **`TaskWindow`** — firma `N`, syscall 294
- **`TextAreaLen`** — firma `NN`, syscall 146
- **`TextAreaLineLen`** — firma `NN`, syscall 147
- **`TextAreaLineOfChar`** — firma `NN`, syscall 148
- **`TextAreaText$`** — firma `N`, syscall 149
- **`TextWidth`** — firma `S`, syscall 254
- **`TimerTicks`** — firma `N`, syscall 219
- **`TotalRam`** — firma `—`, syscall 287
- **`TreeViewRoot`** — firma `N`, syscall 176
- **`Trim$`** — firma `S`, syscall —
- **`UdpClose`** — firma `N`, syscall 302
- **`UdpFrom$`** — firma `N`, syscall 305
- **`UdpFromPort`** — firma `N`, syscall 305
- **`UdpLost`** — firma `N`, syscall 306
- **`UdpOpen`** — firma `N`, syscall 301
- **`UdpPending`** — firma `N`, syscall 311
- **`UdpPort`** — firma `N`, syscall 317
- **`UdpRecv$`** — firma `N`, syscall 304
- **`UdpSend`** — firma `NSNS`, syscall 303
- **`UncheckMenu`** — firma `N`, syscall 122
- **`UpdateProgBar`** — firma `NN`, syscall 162
- **`UpdateWindowMenu`** — firma `N`, syscall —
- **`Upper$`** — firma `S`, syscall —
- **`Val`** — firma `S`, syscall —
- **`Val#`** — firma `S`, syscall —
- **`WaitEvent`** — firma `—`, syscall 8, 14
- **`WindowButtons`** — firma `NNN`, syscall 271
- **`WindowMenu`** — firma `—`, syscall 120
- **`WriteFile`** — firma `SS`, syscall 20, 22
- **`WriteLine`** — firma `NS`, syscall 72
- **`WriteRow`** — firma `NNNN`, syscall 290
- **`Year`** — firma `—`, syscall 134

## Nombres alternativos

El mismo comando llamado como lo llama otro Basic. No son comandos distintos:
el compilador cambia el nombre antes de nada, así que hacen exactamente lo
mismo y no cuestan nada de más. Si tu programa define una `Function` con uno de
estos nombres, manda la tuya.

| Se puede escribir | Y es |
|---|---|
| `CreateListView` | `CreateListBox` |
| `AddToList`, `AddToComboBox` | `AddGadgetItem` — es genérico, vale para cualquier lista |
| `ClearList` | `ClearGadgetItems` |
| `SelectedList` | `SelectedGadgetItem` |
| `CreateProgressBar` | `CreateProgBar` |
| `UpdateProgressBar` | `UpdateProgBar` |
| `SetSoundVolume`, `SetSoundPitch`, `SetSoundPan` | `SoundVolume`, `SoundPitch`, `SoundPan` |
| `WindowWidth`, `WindowHeight` | `ClientWidth`, `ClientHeight` — la zona de dibujo, sin marco ni título |
| `RndInt` | `Rnd` |

`Rand(a, b)` **no** está en esta tabla: no es otro nombre de `Rnd`, sino un
comando propio que cuenta de otra forma. Está en el índice de arriba.

Un mensaje de error nombra siempre el comando principal, porque para cuando se
comprueban los argumentos el nombre ya está cambiado.

## Palabras del lenguaje

`And`, `After`, `Before`, `Case`, `Cls`, `Color`, `ClsColor`, `Console`, `Const`, `CreateWindow`, `Data`, `Default`, `Delay`, `Delete`, `Desktop`, `Dim`, `Each`, `Else`, `ElseIf`, `End`, `End Function`, `End If`, `End Select`, `End Type`, `EndIf`, `Exit`, `False`, `Field`, `First`, `For`, `Forever`, `Function`, `Global`, `Gosub`, `Goto`, `Graphics`, `If`, `Include`, `Insert`, `Last`, `Line`, `Local`, `Mod`, `New`, `Next`, `Not`, `Null`, `Or`, `Oval`, `Plot`, `Print`, `Read`, `Rect`, `Repeat`, `Restore`, `Return`, `Sar`, `Select`, `Shl`, `Shr`, `Step`, `Text`, `Then`, `To`, `True`, `Type`, `Until`, `Wend`, `While`, `Xor`.

`Try`, `Catch` y `Throw` están en el lexer pero **el analizador no las acepta**:
no hay excepciones en Nemo Basic.

---

Continúa en [REFERENCIA_NEMO_BASIC_5.md](REFERENCIA_NEMO_BASIC_5.md):
las trampas del lenguaje y dónde seguir leyendo.
