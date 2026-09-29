#include <string>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cctype>
#include <cstring>
#include <vector>
#include <algorithm>
#include "editor/studio_app.h"
#include "editor/asset_compiler.h"
#include "editor/asset_browser.h"
#include "editor/world_editor.h"
#include "engine/component_uid.h"
#include "engine/engine.h"
#include "engine/file_system.h"
#include "engine/world.h"
#include "engine/reflection.h"
#include "evox/evox_module.h"
#include "renderer/editor/scene_view.h"
#include "core/allocator.h"
#include "core/path.h"
#include "core/string.h"
#include "core/os.h"
#include "core/thread.h"
#include "core/atomic.h"
#include "core/sync.h"
#include "core/log.h"
#ifdef _WIN32
	#define NOMINMAX
	#include <windows.h>
#endif

using namespace Lumix;

namespace {

// Decode a JSON number or an array of numbers. Reject partial parses, NaN and infinity.
static bool parseNumber(const std::string& raw, double& result) {
	if (raw.empty() || raw[0] == '"' || raw[0] == '+' || std::isspace((unsigned char)raw[0])) return false;
	char* end = nullptr;
	result = std::strtod(raw.c_str(), &end);
	return end != raw.c_str() && *end == 0 && std::isfinite(result);
}

static bool parseIndex(const std::string& raw, i32& result) {
	if (raw.empty()) return false;
	for (char c : raw) if (c < '0' || c > '9') return false;
	double n;
	if (!parseNumber(raw, n) || n > 2147483647) return false;
	result = (i32)n;
	return true;
}

static bool parseVector(const std::string& raw, double* out, int count) {
	if (raw.size() < 2 || raw.front() != '[' || raw.back() != ']') return false;
	const char* p = raw.c_str() + 1;
	for (int i = 0; i < count; ++i) {
		while (std::isspace((unsigned char)*p)) ++p;
		if (*p == '+' || *p == '"') return false;
		char* end = nullptr;
		out[i] = std::strtod(p, &end);
		if (end == p || !std::isfinite(out[i]) || std::abs(out[i]) > 3.402823466e38) return false;
		p = end;
		while (std::isspace((unsigned char)*p)) ++p;
		if (*p++ != (i + 1 == count ? ']' : ',')) return false;
	}
	while (std::isspace((unsigned char)*p)) ++p;
	return *p == 0;
}

struct SetPropertyVisitor final : reflection::IEmptyPropertyVisitor {
	WorldEditor& editor;
	ComponentType component;
	EntityRef entity;
	std::string property;
	std::string raw;
	std::string decoded;
	bool is_string;
	bool success = false;
	const char* reason = "Unsupported property type or value";

	SetPropertyVisitor(WorldEditor& editor, ComponentType component, EntityRef entity, const std::string& property,
		const std::string& raw, const std::string& decoded, bool is_string)
		: editor(editor), component(component), entity(entity), property(property), raw(raw), decoded(decoded), is_string(is_string) {}

	template <typename T> void apply(const reflection::Property<T>& prop, T value) {
		if (prop.isReadonly()) { reason = "Property is read-only"; return; }
		editor.setProperty(component, "", -1, property.c_str(), Span(&entity, 1), value);
		success = true;
	}
	void visit(const reflection::Property<float>& prop) override {
		double n; if (!is_string && parseNumber(raw, n) && std::abs(n) <= 3.402823466e38) apply(prop, (float)n);
	}
	void visit(const reflection::Property<int>& prop) override {
		double n; if (!is_string && parseNumber(raw, n) && n >= -2147483648.0 && n <= 2147483647.0 && std::floor(n) == n) apply(prop, (int)n);
	}
	void visit(const reflection::Property<u32>& prop) override {
		double n; if (!is_string && parseNumber(raw, n) && n >= 0 && n <= 4294967295.0 && std::floor(n) == n) apply(prop, (u32)n);
	}
	void visit(const reflection::Property<EntityPtr>& prop) override {
		if (raw == "null") { apply(prop, INVALID_ENTITY); return; }
		i32 idx; if (!is_string && parseIndex(raw, idx) && editor.getWorld()->hasEntity(EntityRef(idx))) apply(prop, EntityPtr{idx});
	}
	void visit(const reflection::Property<bool>& prop) override {
		if (raw == "true") apply(prop, true);
		else if (raw == "false") apply(prop, false);
	}
	void visit(const reflection::Property<const char*>& prop) override { if (is_string) apply(prop, decoded.c_str()); }
	void visit(const reflection::Property<Path>& prop) override { if (is_string) apply(prop, Path(decoded.c_str())); }
	void visit(const reflection::Property<Vec2>& prop) override {
		double n[2]; if (!is_string && parseVector(raw, n, 2)) apply(prop, Vec2((float)n[0], (float)n[1]));
	}
	void visit(const reflection::Property<Vec3>& prop) override {
		double n[3]; if (!is_string && parseVector(raw, n, 3)) apply(prop, Vec3((float)n[0], (float)n[1], (float)n[2]));
	}
	void visit(const reflection::Property<Vec4>& prop) override {
		double n[4]; if (!is_string && parseVector(raw, n, 4)) apply(prop, Vec4((float)n[0], (float)n[1], (float)n[2], (float)n[3]));
	}
	void visit(const reflection::Property<IVec3>& prop) override {
		double n[3]; if (!is_string && parseVector(raw, n, 3)
			&& n[0] >= -2147483648.0 && n[0] <= 2147483647.0 && std::floor(n[0]) == n[0]
			&& n[1] >= -2147483648.0 && n[1] <= 2147483647.0 && std::floor(n[1]) == n[1]
			&& n[2] >= -2147483648.0 && n[2] <= 2147483647.0 && std::floor(n[2]) == n[2])
			apply(prop, IVec3((i32)n[0], (i32)n[1], (i32)n[2]));
	}
};

// In-process, sessionless MCP over Streamable HTTP, bound to loopback only.
struct RemoteControl final : StudioApp::IPlugin {
	static constexpr u32 MAX_MESSAGE = 1024 * 1024;
	static constexpr u16 DEFAULT_PORT = 17123;
	struct TcpThread final : Thread {
		RemoteControl& owner;
		TcpThread(IAllocator& allocator, RemoteControl& owner) : Thread(allocator), owner(owner) {}
		i32 task() override;
	};

	StudioApp& app;
	Mutex mutex;
	Semaphore done{0, 2};
	std::string pending;
	std::string response;
	bool has_pending = false;
	AtomicI32 stop{0};
	os::NetworkStream* volatile client = nullptr;
	TcpThread* thread = nullptr;
	u16 port = DEFAULT_PORT;
	bool network_initialized = false;

	explicit RemoteControl(StudioApp& app) : app(app) {
#ifdef _WIN32
		char port_env[16] = {};
		const DWORD port_len = GetEnvironmentVariableA("LUMIX_REMOTE_PORT", port_env, sizeof(port_env));
		if (port_len > 0 && port_len < sizeof(port_env)) {
			char* end = nullptr;
			const long value = std::strtol(port_env, &end, 10);
			if (*end == 0 && value > 0 && value <= 65535) port = (u16)value;
		}

		network_initialized = os::initNetwork();
		if (!network_initialized) { logError("Remote control: network initialization failed"); return; }
		thread = LUMIX_NEW(app.getAllocator(), TcpThread)(app.getAllocator(), *this);
		if (!thread->create("remote_control_tcp", false)) {
			logError("Remote control: could not start listener thread");
			LUMIX_DELETE(app.getAllocator(), thread);
			thread = nullptr;
		}
#else
		logError("Remote control: TCP listener is currently only supported on Windows");
#endif
	}
	~RemoteControl() override {
		stop = 1;
		done.signal();
		os::NetworkStream* connected = (os::NetworkStream*)exchangePtr((void* volatile*)&client, nullptr);
		if (connected) os::interrupt(*connected);
		if (thread) {
			// Unblock accept if the worker is waiting for a connection.
			auto* wake = os::connect("127.0.0.1", port, app.getAllocator());
			if (wake) os::close(*wake);
			thread->destroy();
			LUMIX_DELETE(app.getAllocator(), thread);
		}
		if (connected) os::close(*connected);
		if (network_initialized) os::shutdownNetwork();
	}
	void init() override {}
	const char* getName() const override { return "remote_control"; }
	bool showGizmo(WorldView&, ComponentUID) override { return false; }

	void update(float) override {
		std::string request;
		mutex.enter();
		if (has_pending) { request.swap(pending); has_pending = false; }
		mutex.exit();
		if (request.empty()) return;
		response.clear();
		handle(request);
		done.signal();
	}

	static std::string jsonString(const std::string& s) {
		std::string out = "\"";
		const char* hex = "0123456789abcdef";
		for (unsigned char c : s) {
			if (c == '"' || c == '\\') { out += '\\'; out += c; }
			else if (c == '\n') out += "\\n";
			else if (c == '\r') out += "\\r";
			else if (c == '\t') out += "\\t";
			else if (c < 0x20) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
			else out += c;
		}
		return out + "\"";
	}
	static std::string fieldString(const std::string& s, const char* key) {
		const std::string needle = std::string("\"") + key + "\"";
		size_t p = s.find(needle);
		if (p == std::string::npos || (p = s.find(':', p + needle.size())) == std::string::npos) return "";
		p = s.find('"', p + 1);
		if (p == std::string::npos) return "";
		std::string out;
		for (++p; p < s.size(); ++p) {
			if (s[p] == '"') return out;
			if (s[p] == '\\' && p + 1 < s.size()) {
				const char c = s[++p];
				switch (c) {
					case 'n': out += '\n'; break;
					case 'r': out += '\r'; break;
					case 't': out += '\t'; break;
					case '"': out += '"'; break;
					case '\\': out += '\\'; break;
					default: return ""; // Invalid/unsupported escape.
				}
			} else out += s[p];
		}
		return "";
	}

	void send(const std::string& s) { response = s; }
	void handle(const std::string& msg) {
		const std::string id = fieldRaw(msg, "id");
		const std::string method = fieldString(msg, "method");
		if (method == "initialize") {
			send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"protocolVersion\":\"2025-03-26\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"lumix-remote\",\"version\":\"0.1.0\"}}}");
		}
		else if (method == "notifications/initialized") {}
		else if (method == "tools/list") {
			send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"tools\":["
				"{\"name\":\"create_entity\",\"description\":\"Create an entity in the active world.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"additionalProperties\":false}},"
				"{\"name\":\"new_world\",\"description\":\"Create a new world in Studio. If the current world has unsaved changes, Studio asks for confirmation instead.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
				"{\"name\":\"save_world\",\"description\":\"Save all named world partitions. For a new unnamed world, provide a .unv path relative to the project.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"additionalProperties\":false}},"
				"{\"name\":\"make_screenshot\",\"description\":\"Queue a TGA screenshot of Studio's scene view to a project-relative path. The file is written asynchronously.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"],\"additionalProperties\":false}},"
				"{\"name\":\"load_world\",\"description\":\"Load a project-relative .unv world. Set additive to true to load it as another partition; otherwise replace the current world (Studio prompts if there are unsaved changes).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"additive\":{\"type\":\"boolean\"}},\"required\":[\"path\"],\"additionalProperties\":false}},"
				"{\"name\":\"list_assets\",\"description\":\"List indexed project assets, sorted by path, with optional path prefix and pagination.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prefix\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\",\"minimum\":0},\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}},\"additionalProperties\":false}},"
				"{\"name\":\"add_component\",\"description\":\"Add a reflected component to an entity.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"entity_id\":{\"type\":\"integer\"},\"component\":{\"type\":\"string\"}},\"required\":[\"entity_id\",\"component\"],\"additionalProperties\":false}},"
				"{\"name\":\"set_property\",\"description\":\"Set a top-level reflected component property; vectors are numeric arrays.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"entity_id\":{\"type\":\"integer\"},\"component\":{\"type\":\"string\"},\"property\":{\"type\":\"string\"},\"value\":{}},\"required\":[\"entity_id\",\"component\",\"property\",\"value\"],\"additionalProperties\":false}},"
				"{\"name\":\"evox_execute\",\"description\":\"Compile and execute Evox source in a fresh runtime against the current world. Source must define fn main(world : World) : void.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"}},\"required\":[\"code\"],\"additionalProperties\":false}}]}}");
		}
		else if (method == "tools/call") {
			const std::string tool = fieldString(msg, "name");
			if (tool != "create_entity" && tool != "new_world" && tool != "save_world" && tool != "make_screenshot" && tool != "load_world" && tool != "evox_execute" && tool != "add_component" && tool != "set_property" && tool != "list_assets") { error(id, -32602, "Unknown tool"); return; }
			if (tool == "list_assets") {
				const size_t args_pos = msg.find("\"arguments\"");
				const std::string args = args_pos == std::string::npos ? "" : msg.substr(args_pos);
				const std::string prefix = fieldString(args, "prefix");
				i32 offset = 0;
				i32 limit = 100;
				const std::string offset_raw = fieldValue(args, "offset");
				const std::string limit_raw = fieldValue(args, "limit");
				if ((!offset_raw.empty() && !parseIndex(offset_raw, offset))
					|| (!limit_raw.empty() && (!parseIndex(limit_raw, limit) || limit < 1 || limit > 500))) {
					error(id, -32602, "Invalid offset or limit (limit must be 1..500)"); return;
				}
				struct AssetInfo {
					std::string path;
					ResourceType type;
				};
				std::vector<AssetInfo> assets;
				AssetCompiler& compiler = app.getAssetCompiler();
				const auto& resources = compiler.lockResources();
				for (const AssetCompiler::ResourceItem& item : resources) {
					const char* path = item.path.c_str();
					if (std::string(path).rfind(prefix, 0) == 0) assets.push_back({path, item.type});
				}
				compiler.unlockResources();
				std::sort(assets.begin(), assets.end(), [](const AssetInfo& a, const AssetInfo& b) { return a.path < b.path; });
				std::string payload = "{\"assets\":[";
				const size_t start = std::min((size_t)offset, assets.size());
				const size_t end = std::min(start + (size_t)limit, assets.size());
				for (size_t i = start; i < end; ++i) {
					if (i != start) payload += ',';
					std::string label;
					for (AssetBrowser::IPlugin* plugin : app.getAssetBrowser().getPlugins()) {
						if (plugin->getResourceType() == assets[i].type) { label = plugin->getLabel(); break; }
					}
					payload += "{\"path\":" + jsonString(assets[i].path) + ",\"type\":" + jsonString(label)
						+ ",\"type_hash\":" + jsonString(std::to_string(assets[i].type.type.getHashValue())) + "}";
				}
				payload += "],\"total\":" + std::to_string(assets.size());
				if (end < assets.size()) payload += ",\"next_offset\":" + std::to_string(end);
				payload += "}";
				send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":" + jsonString(payload) + "}],\"structuredContent\":" + payload + "}}");
				return;
			}
			if (tool == "new_world") {
				const bool needs_confirmation = app.getWorldEditor().isWorldChanged();
				app.newWorld();
				const std::string text = needs_confirmation ? "Studio is waiting for confirmation to discard unsaved changes" : "New world created";
				send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":" + jsonString(text) + "}],\"structuredContent\":{\"created\":" + (needs_confirmation ? "false" : "true") + ",\"confirmation_required\":" + (needs_confirmation ? "true" : "false") + "}}}");
				return;
			}
			if (tool == "make_screenshot") {
				const size_t args_pos = msg.find("\"arguments\"");
				const std::string args = args_pos == std::string::npos ? "" : msg.substr(args_pos);
				const std::string path = fieldString(args, "path");
				if (path.size() < 5 || path.size() >= MAX_PATH || path.compare(path.size() - 4, 4, ".tga") != 0
					|| path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos
					|| path.find('\\') != std::string::npos || path.find("..") != std::string::npos) {
					error(id, -32602, "Expected a project-relative .tga path without traversal"); return;
				}
				auto* plugin = app.getGUIPlugin("scene_view");
				if (!plugin) { error(id, -32000, "Scene view unavailable"); return; }
				auto* view = static_cast<SceneView*>(plugin);
				if (!view->getPipeline() || view->getPipeline()->getDisplaySize().x <= 0 || view->getPipeline()->getDisplaySize().y <= 0) {
					error(id, -32000, "Scene view has no renderable size"); return;
				}
				view->makeScreenshot(StringView(path.c_str(), path.size()));
				const std::string payload = "{\"path\":" + jsonString(path) + ",\"queued\":true}";
				send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":" + jsonString("Screenshot queued: " + path + " (check Studio logs for write errors)") + "}],\"structuredContent\":" + payload + "}}");
				return;
			}
			World* world = app.getWorldEditor().getWorld();
			if (!world) { error(id, -32000, "No active world"); return; }
			if (tool == "load_world") {
				WorldEditor& editor = app.getWorldEditor();
				if (editor.isGameMode() || editor.isLoading()) { error(id, -32000, "Cannot load world while playing or loading"); return; }
				const size_t args_pos = msg.find("\"arguments\"");
				const std::string args = args_pos == std::string::npos ? "" : msg.substr(args_pos);
				const std::string path = fieldString(args, "path");
				if (path.size() < 5 || path.size() >= sizeof(world->getPartitions()[0].name)
					|| path.compare(path.size() - 4, 4, ".unv") != 0
					|| path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos
					|| path.find('\\') != std::string::npos || path.find("..") != std::string::npos) {
					error(id, -32602, "Expected a project-relative .unv path (max 63 characters) without traversal"); return;
				}
				const Path world_path(path.c_str());
				const std::string normalized_path = world_path.c_str();
				const std::string additive_raw = fieldValue(args, "additive");
				if (!additive_raw.empty() && additive_raw != "true" && additive_raw != "false") {
					error(id, -32602, "additive must be a boolean"); return;
				}
				const bool additive = additive_raw == "true";
				const auto& partitions = world->getPartitions();
				if (additive) {
					if (partitions.size() == 1 && !partitions[0].name[0]) {
						error(id, -32602, "Save the current world before loading additively"); return;
					}
					for (const World::Partition& partition : partitions) {
						if (normalized_path == partition.name) { error(id, -32602, "World is already loaded"); return; }
					}
				}
				if (!app.getEngine().getFileSystem().fileExists(world_path.c_str())) {
					error(id, -32602, "World file does not exist"); return;
				}
				const bool confirmation_required = !additive && editor.isWorldChanged();
				app.tryLoadWorld(world_path, additive);
				bool loaded = false;
				if (!confirmation_required) {
					for (const World::Partition& partition : editor.getWorld()->getPartitions()) {
						if (normalized_path == partition.name) { loaded = true; break; }
					}
					if (!loaded) { error(id, -32000, "World failed to load; check Studio logs"); return; }
				}
				const std::string text = confirmation_required ? "Studio is waiting for confirmation to discard unsaved changes" : "World loaded";
				send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":" + jsonString(text) + "}],\"structuredContent\":{\"path\":" + jsonString(normalized_path) + ",\"additive\":" + (additive ? "true" : "false") + ",\"loaded\":" + (loaded ? "true" : "false") + ",\"confirmation_required\":" + (confirmation_required ? "true" : "false") + "}}}");
				return;
			}
			if (tool == "save_world") {
				WorldEditor& editor = app.getWorldEditor();
				if (editor.isGameMode()) { error(id, -32000, "Cannot save while the game is running"); return; }
				const size_t args_pos = msg.find("\"arguments\"");
				const std::string args = args_pos == std::string::npos ? "" : msg.substr(args_pos);
				const std::string path = fieldString(args, "path");
				const auto& partitions = world->getPartitions();
				const bool unnamed = partitions.size() == 1 && !partitions[0].name[0];
				if (unnamed) {
					if (path.empty()) { error(id, -32602, "New world requires a .unv path relative to the project"); return; }
					if (path.size() >= sizeof(partitions[0].name) || path.size() < 5 || path.compare(path.size() - 4, 4, ".unv") != 0
						|| path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos || path.find('\\') != std::string::npos
						|| path.find("..") != std::string::npos) {
						error(id, -32602, "Invalid project-relative .unv path (max 63 characters)"); return;
					}
					copyString(world->getPartition(partitions[0].handle).name, path.c_str());
				}
				else if (!path.empty()) { error(id, -32602, "Path is only supported for a new unnamed world"); return; }
				for (const World::Partition& partition : partitions) editor.savePartition(partition.handle);
				const std::string text = "World save requested";
				send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":" + jsonString(text) + "}]}}");
				return;
			}
			if (tool == "evox_execute") {
				const size_t args_pos = msg.find("\"arguments\"");
				if (args_pos == std::string::npos) { error(id, -32602, "Missing arguments"); return; }
				const std::string code = fieldString(msg.substr(args_pos), "code");
				if (code.empty() || code.size() > 256 * 1024) { error(id, -32602, "Code must be nonempty and at most 256 KB"); return; }
				auto* evox = static_cast<EvoxSystem*>(app.getEngine().getSystemManager().getSystem("evox_system"));
				if (!evox) { error(id, -32000, "Evox system unavailable"); return; }
				String output(app.getAllocator());
				const bool ok = evox->executeSource(StringView(code.c_str(), code.size()), *world, output);
				const std::string text = output.length() == 0 ? (ok ? "Evox execution completed" : "Evox execution failed") : output.c_str();
				send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":" + jsonString(text) + "}],\"isError\":" + (ok ? "false" : "true") + "}}");
				return;
			}
			if (tool == "add_component" || tool == "set_property") {
				const size_t args_pos = msg.find("\"arguments\"");
				if (args_pos == std::string::npos) { error(id, -32602, "Missing arguments"); return; }
				const std::string args = msg.substr(args_pos);
				i32 index;
				if (!parseIndex(fieldRaw(args, "entity_id"), index) || !world->hasEntity(EntityRef(index))) {
					error(id, -32602, "Invalid entity_id"); return;
				}
				const EntityRef entity(index);
				const std::string name = fieldString(args, "component");
				if (name.empty() || !reflection::componentTypeExists(name.c_str())) {
					error(id, -32602, "Unknown component"); return;
				}
				const ComponentType component = reflection::getComponentType(name.c_str());
				if (tool == "add_component") {
					if (world->hasComponent(entity, component)) { error(id, -32602, "Entity already has component"); return; }
					app.getWorldEditor().addComponent(Span(&entity, 1), component);
					if (!world->hasComponent(entity, component)) { error(id, -32000, "Could not add component"); return; }
				} else {
					if (!world->hasComponent(entity, component)) { error(id, -32602, "Entity does not have component"); return; }
					const std::string property = fieldString(args, "property");
					const reflection::PropertyBase* desc = property.empty() ? nullptr : reflection::getProperty(component, property.c_str());
					if (!desc) { error(id, -32602, "Unknown property"); return; }
					const std::string raw = fieldValue(args, "value");
					if (raw.empty()) { error(id, -32602, "Missing or invalid value"); return; }
					const bool is_string = raw[0] == '"';
					const std::string decoded = is_string ? fieldString(args, "value") : "";
					SetPropertyVisitor visitor(app.getWorldEditor(), component, entity, property, raw, decoded, is_string);
					desc->visit(visitor);
					if (!visitor.success) { error(id, -32602, visitor.reason); return; }
				}
				send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":" + jsonString(tool == "add_component" ? "Component added" : "Property set") + "}]}}");
				return;
			}
			const EntityRef entity = world->createEntity(DVec3(0.0), Quat(0, 0, 0, 1));
			std::string args;
			size_t args_pos = msg.find("\"arguments\"");
			if (args_pos != std::string::npos) args = msg.substr(args_pos);
			const std::string name = fieldString(args, "name");
			if (!name.empty()) world->setEntityName(entity, StringView(name.c_str(), (u32)name.size()));
			send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"Created entity " + std::to_string(entity.index) + "\"}],\"structuredContent\":{\"entity_id\":" + std::to_string(entity.index) + "}}}");
		}
		else if (!id.empty()) error(id, -32601, "Method not found");
	}
	static std::string fieldValue(const std::string& s, const char* key) {
		const std::string needle = std::string("\"") + key + "\"";
		size_t p = s.find(needle);
		if (p == std::string::npos || (p = s.find(':', p + needle.size())) == std::string::npos) return "";
		++p;
		while (p < s.size() && std::isspace((unsigned char)s[p])) ++p;
		if (p >= s.size()) return "";
		const size_t start = p;
		if (s[p] == '[') {
			p = s.find(']', p + 1);
			return p == std::string::npos ? "" : s.substr(start, p - start + 1);
		}
		if (s[p] == '"') {
			for (++p; p < s.size(); ++p) {
				if (s[p] == '\\' && p + 1 < s.size()) { ++p; continue; }
				if (s[p] == '"') return s.substr(start, p - start + 1);
			}
			return "";
		}
		while (p < s.size() && s[p] != ',' && s[p] != '}' && !std::isspace((unsigned char)s[p])) ++p;
		return s.substr(start, p - start);
	}
	static std::string fieldRaw(const std::string& s, const char* key) {
		const std::string needle = std::string("\"") + key + "\"";
		size_t p = s.find(needle);
		if (p == std::string::npos || (p = s.find(':', p + needle.size())) == std::string::npos) return "null";
		++p; while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
		if (p < s.size() && s[p] == '"') { size_t e = s.find('"', p + 1); return e == std::string::npos ? "null" : s.substr(p, e - p + 1); }
		size_t e = s.find_first_of(",}", p); return s.substr(p, e == std::string::npos ? e : e - p);
	}
	void error(const std::string& id, int code, const char* message) {
		send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":" + jsonString(message) + "}}");
	}
};

// Socket I/O stays on the worker. All engine/editor work happens in update() on the main thread.
i32 RemoteControl::TcpThread::task() {
	os::NetworkListener* listener = os::createListener("127.0.0.1", owner.port, owner.app.getAllocator());
	if (!listener) {
		if (!owner.stop) logError("Remote MCP: could not listen on 127.0.0.1:", owner.port);
		return 0;
	}
	auto writeHttp = [](os::NetworkStream& stream, int code, const char* status, const std::string& body) {
		const std::string headers = "HTTP/1.1 " + std::to_string(code) + " " + status
			+ "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size())
			+ "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
		if (!os::write(stream, headers.data(), (u32)headers.size())) return;
		if (!body.empty()) os::write(stream, body.data(), (u32)body.size());
	};
	while (!owner.stop) {
		os::NetworkStream* stream = os::accept(*listener);
		if (!stream) {
			if (!owner.stop) logError("Remote MCP: accept failed on 127.0.0.1:", owner.port);
			break;
		}
		owner.client = stream;
		if (owner.stop) goto close_client;
		{
			std::string headers;
			char c;
			while (!owner.stop && headers.size() < 8192 && os::read(*stream, &c, 1) == os::NetworkReadResult::SUCCESS) {
				headers += c;
				if (headers.size() >= 4 && headers.compare(headers.size() - 4, 4, "\r\n\r\n") == 0) break;
			}
			if (owner.stop) goto close_client;
			if (headers.size() < 4 || headers.compare(headers.size() - 4, 4, "\r\n\r\n") != 0) {
				writeHttp(*stream, 400, "Bad Request", ""); goto close_client;
			}
			std::string lower = headers;
			for (char& ch : lower) ch = (char)std::tolower((unsigned char)ch);
			auto header = [&](const char* key) -> std::string {
				const size_t p = lower.find(std::string("\r\n") + key + ":");
				if (p == std::string::npos) return "";
				size_t start = p + std::strlen(key) + 3;
				while (start < lower.size() && lower[start] == ' ') ++start;
				const size_t end = lower.find("\r\n", start);
				return lower.substr(start, end - start);
			};
			const std::string host = header("host");
			const std::string expected_host = "127.0.0.1:" + std::to_string(owner.port);
			const std::string alt_host = "localhost:" + std::to_string(owner.port);
			const std::string origin = header("origin");
			if (headers.rfind("POST /mcp HTTP/1.1\r\n", 0) != 0) {
				writeHttp(*stream, 405, "Method Not Allowed", ""); goto close_client;
			}
			const std::string length_header = header("content-length");
			char* end = nullptr;
			const long length = std::strtol(length_header.c_str(), &end, 10);
			if (length_header.empty() || *end != 0 || length <= 0 || length > MAX_MESSAGE) {
				writeHttp(*stream, 400, "Bad Request", ""); goto close_client;
			}
			std::string request((size_t)length, '\0');
			if (os::read(*stream, request.data(), (u32)length) != os::NetworkReadResult::SUCCESS) goto close_client;
			// Drain the body before rejecting an origin, so closing does not reset the HTTP response.
			if ((host != expected_host && host != alt_host)
				|| (!origin.empty() && origin != "http://" + host)) {
				writeHttp(*stream, 403, "Forbidden", ""); goto close_client;
			}
			owner.mutex.enter();
			owner.pending = std::move(request);
			owner.has_pending = true;
			owner.mutex.exit();
			owner.done.wait();
			if (owner.stop) goto close_client;
			if (owner.response.empty()) writeHttp(*stream, 202, "Accepted", "");
			else writeHttp(*stream, 200, "OK", owner.response);
		}
	close_client:
		if (exchangePtr((void* volatile*)&owner.client, nullptr) == stream) os::close(*stream);
	}
	os::close(*listener);
	return 0;
}

} // namespace

LUMIX_STUDIO_ENTRY(remote) {
	return LUMIX_NEW(app.getAllocator(), RemoteControl)(app);
}
