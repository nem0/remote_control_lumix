# Remote Control Plugin

This plugin adds a simple TCP-based remote control interface to the Lumix Editor.

**Overview**
- The plugin starts a background TCP listener on `127.0.0.1:17123` when the editor loads the plugin.
- It accepts JSON messages with the following format:
  ```json
  {"type": "<command>", "payload": "<data>"}
  ```
- Currently supported command:
  - `logError` — writes the `payload` to the editor log as an error.

The implementation is in [plugins/remote_control/src/editor/remote_control_plugins.cpp](plugins/remote_control/src/editor/remote_control_plugins.cpp).

**Usage**
- Start the editor (the plugin starts automatically when loaded).
- From another process on the same machine connect to `127.0.0.1:17123` and send:
  ```json
  {"type": "logError", "payload": "Hello world"}
  ```

**Included helper**
- A small Windows batch helper `send_log_error.bat` is included in this folder to quickly send a `logError` message to the running editor. Use it like:

  - `send_log_error.bat "Hello from batch"`

- You can use any tool that can send TCP data (e.g., PowerShell, Python, netcat) to send JSON messages.
- You can ask AI to send the message.

**Troubleshooting**
- If the plugin fails to initialize networking it will log an error on startup.
- If a sent line exceeds 1MB it will be rejected and the connection closed (to avoid unbounded memory use).

