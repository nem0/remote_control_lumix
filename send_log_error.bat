@echo off
setlocal
set "message=%~1"
if "%message%"=="" set "message=Test from remote"

powershell -NoProfile -Command "& { param($m) $tcp = New-Object System.Net.Sockets.TcpClient('127.0.0.1',17123); $s = $tcp.GetStream(); $w = New-Object System.IO.StreamWriter($s); $w.WriteLine('logError \"' + $m + '\"'); $w.Flush(); $tcp.Close() } '%message%'"
