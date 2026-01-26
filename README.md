# Remote Control Plugin

This plugin adds a simple TCP-based remote control interface to the Lumix Editor.

**Overview**
- The plugin starts a background TCP listener on `127.0.0.1:17123` when the editor loads the plugin.
- It accepts messages in the following binary format:
  ```
  <length> <lua_code>
  ```
  Where `<length>` is an ASCII decimal number, followed by a space, followed by exactly that many bytes of Lua code.
- The Lua code is executed in the engine's Lua state.

The implementation is in [src/editor/remote_control_plugins.cpp](src/editor/remote_control_plugins.cpp).

**Usage**
- Download and compile the plugin [as any](../../docs/plugins.md) other [plugin](https://nem0.github.io/LumixEngine/plugins.html).
- Start the editor (the plugin starts automatically when loaded).
- From another process on the same machine connect to `127.0.0.1:17123` and send a message:
  ```
  24 LumixAPI.logError("Hi")
  ```

**Included helpers**
- `send_log_error.bat` — sends a Lua `logError` call to the running editor:
  ```
  send_log_error.bat "Hello from batch"
  ```

- You can use any tool that can send TCP data (e.g., PowerShell, Python, netcat) to send commands.
- You can ask AI to send the message.

**Troubleshooting**
- If the plugin fails to initialize networking it will log an error on startup.
- If a message exceeds 1MB it will be rejected and the connection closed (to avoid unbounded memory use).

