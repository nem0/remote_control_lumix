# Remote Control Plugin

This plugin adds a simple TCP-based remote control interface to the Lumix Editor.

**Overview**
- The plugin starts a background TCP listener on `127.0.0.1:17123` when the editor loads the plugin.
- It accepts text-based commands in the following format:
  ```
  <command> "<argument>"
  ```
- Supported commands:
  - `logError "message"` — writes the message to the editor log as an error.
  - `runLua "code"` — executes Lua code in the engine's Lua state. Use `\\\"` for escaped quotes and `\\\\` for escaped backslashes within the code string.

The implementation is in [src/editor/remote_control_plugins.cpp](src/editor/remote_control_plugins.cpp).

**Usage**
- Start the editor (the plugin starts automatically when loaded).
- From another process on the same machine connect to `127.0.0.1:17123` and send commands:
  ```
  logError "Hello world"
  ```
  ```
  runLua "LumixAPI.logError(\"Hello from Lua\")"
  ```

**Included helpers**
- `send_log_error.bat` — sends a `logError` message to the running editor:
  ```
  send_log_error.bat "Hello from batch"
  ```


- You can use any tool that can send TCP data (e.g., PowerShell, Python, netcat) to send commands.
- You can ask AI to send the message.

**Troubleshooting**
- If the plugin fails to initialize networking it will log an error on startup.
- If a message exceeds 1MB it will be rejected and the connection closed (to avoid unbounded memory use).

