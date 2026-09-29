# Remote Control Plugin

This plugin hosts an experimental MCP server **inside a running Lumix Studio instance** on Windows. It listens on `http://127.0.0.1:17123/mcp` using Streamable HTTP (POST requests, sessionless). Configure an MCP client that supports HTTP servers with that URL; there is no separate bridge process or stdin/stdout connection.

The server exposes `new_world`, `save_world`, `load_world`, `make_screenshot`, `create_entity`, `list_assets`, `add_component`, `set_property`, and `evox_execute`. To create a new world:

```json
{"name":"new_world","arguments":{}}
```

This uses Studio's New World action. If the current world has unsaved changes, Studio prompts for confirmation instead; `structuredContent.created` is `false` and `confirmation_required` is `true` until the user confirms in Studio.

To save the active world (all named partitions):

```json
{"name":"save_world","arguments":{}}
```

For a new unnamed world, supply a project-relative `.unv` path (up to 63 characters):

```json
{"name":"save_world","arguments":{"path":"worlds/my_world.unv"}}
```

Saving is unavailable in game mode. The tool reports that saving was requested; check Studio logs for filesystem errors.

To replace the current world, or load another world as an additive partition:

```json
{"name":"load_world","arguments":{"path":"worlds/my_world.unv"}}
{"name":"load_world","arguments":{"path":"worlds/extra.unv","additive":true}}
```

`additive` defaults to `false`. Paths must be project-relative, end in `.unv`, and fit in 63 characters. An additive load requires a named current world and cannot load an already-open partition. A non-additive load with unsaved changes opens Studio's confirmation dialog rather than loading immediately; check `structuredContent.loaded` and `confirmation_required`. Loading is unavailable in game mode.

To queue a screenshot of Studio's scene view:

```json
{"name":"make_screenshot","arguments":{"path":"screenshots/scene.tga"}}
```

The path must be project-relative, end in `.tga`, and have an existing parent directory. The tool returns `structuredContent.path` and `queued: true`; the GPU readback and file write happen asynchronously, so check Studio logs for failures. This captures the scene view, not the entire Studio window.

To queue a screenshot of the **game view** (the in-game UI included), use `make_game_screenshot` with the same `.tga` path rules. The Game View window must be visible (or merged with the scene view) so it has a size; the tool reports an error otherwise:

```json
{"name":"make_game_screenshot","arguments":{"path":"screenshots/game.tga"}}
```

`create_entity`:

```json
{"name":"create_entity","arguments":{"name":"Example"}}
```

It creates an entity in the active world and returns its entity index. The `name` argument is optional.

To list indexed assets (including subresources), with optional path prefix and pagination:

```json
{"name":"list_assets","arguments":{"prefix":"editor/fonts/","limit":50,"offset":0}}
```

Results are sorted by path. `structuredContent` contains `assets` (each with `path`, human-readable `type`, and decimal-string `type_hash`), `total` matching assets, and `next_offset` when there is another page. The default limit is 100, maximum 500. The compiler's index can still be populating just after opening a project.

Use the returned `structuredContent.entity_id` from `create_entity` to add a component and set a reflected property:

```json
{"name":"add_component","arguments":{"entity_id":1,"component":"point_light"}}
{"name":"set_property","arguments":{"entity_id":1,"component":"point_light","property":"Intensity","value":4.5}}
{"name":"set_property","arguments":{"entity_id":1,"component":"point_light","property":"Color","value":[0.2,0.4,0.8]}}
```

Component IDs and property names are case-sensitive reflection names (see `src/*/*_module.gen.h`). `set_property` supports numeric, boolean, string, path, entity-reference, and 2–4 element vector properties; entity references take an entity ID or `null`. Only top-level properties are supported, not array items or blobs. Both tools validate entity and component existence and use editor commands for undo.

To run a complete Evox source file in a temporary runtime against the active Studio world:

```json
{"name":"evox_execute","arguments":{"code":"import \"core:world\"\nfn main(world : World) : void { world.createEntity(); }"}}
```

The source must define `fn main(world : World) : void` and may import project and `core:` modules. Each request compiles in its own runtime (no shared script globals), but native Evox APIs can modify the active world. Compilation and execution failures return MCP `isError: true` with diagnostics. Scripts must finish without yielding; an infinite loop will block Studio, so use this only with trusted code.

The HTTP listener runs on a worker thread; tool execution runs on Studio's update thread. Requests are handled one at a time. The server implements MCP initialization, `tools/list`, and `tools/call`. Only POST `/mcp` is supported (GET returns 405), with one JSON-RPC message per request and a 1 MB request-body limit. It binds to loopback and checks Host/Origin, but has **no authentication**. The JSON parser and protocol surface are intentionally minimal; do not expose this port to untrusted users or networks.
