@echo off
setlocal
set "message=%~1"
if "%message%"=="" set "message=Test from remote"

REM New protocol: <length><space><lua_code>
REM Sends: LumixAPI.logError("message")
powershell -NoProfile -Command "& { param($m) $lua = 'LumixAPI.logError(\"' + $m + '\")'; $len = [System.Text.Encoding]::UTF8.GetByteCount($lua); $msg = $len.ToString() + ' ' + $lua; $tcp = New-Object System.Net.Sockets.TcpClient('127.0.0.1',17123); $s = $tcp.GetStream(); $bytes = [System.Text.Encoding]::UTF8.GetBytes($msg); $s.Write($bytes, 0, $bytes.Length); $s.Flush(); $tcp.Close() } '%message%'"
