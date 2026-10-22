# Проверка окна без глаз

Скрипты, которыми проверяют окно AIControls, не глядя на экран: снимок окна, распознавание текста на нём,
клики сообщениями (настоящий указатель остаётся на месте) и поиск ползунков по пикселям.
Windows PowerShell 5.1; координаты — в пикселях (при 100 % DIP = пиксель).

```powershell
$p = Start-Process build\dev\aicontrols.exe -PassThru; Start-Sleep 4
tools\probes\ui\shot.ps1 -ProcessId $p.Id -Out $env:TEMP\1.png             # снимок окна (PrintWindow)
tools\probes\ui\ocr.ps1 -Path $env:TEMP\1.png -MaxX 480                     # текст и где он (OCR Windows)
tools\probes\ui\crop.ps1 -Path $env:TEMP\1.png -X 6 -Y 190 -W 470 -H 34 -Out $env:TEMP\row.png  # мелкое — крупнее
tools\probes\ui\column.ps1 -Path $env:TEMP\1.png -X 985 -Top 30 -Bottom 835 # где ползунок
powershell -Command "& tools\probes\ui\mouse.ps1 -ProcessId $($p.Id) -Steps @('down:977,742','drag:977,300','up:977,300')"
Stop-Process $p.Id
```

- На снимке клиентская область начинается на 8 px правее края (невидимая рамка окна), по вертикали — с 0.
  `mouse.ps1` берёт координаты клиента.
- Массив шагов в `mouse.ps1` передавать через `-Command`: через `-File` он приходит одной строкой.
- Второй экземпляр пишет в тот же `%APPDATA%\AIControls\settings.json`, что и установленный.
- Средняя кнопка читает настоящий указатель (`GetCursorPos`): нажатие далеко от него — это «зажал и повёл».
