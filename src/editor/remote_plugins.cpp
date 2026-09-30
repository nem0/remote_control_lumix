#include "editor/studio_app.h"
#include "editor/asset_compiler.h"
#include "editor/asset_browser.h"
#include "editor/world_editor.h"
#include "engine/component_uid.h"
#include "engine/engine.h"
#include "engine/file_system.h"
#include "engine/input_system.h"
#include "engine/world.h"
#include "engine/reflection.h"
#include "evox/evox_module.h"
#include "renderer/editor/scene_view.h"
#include <imgui/IconsFontAwesome5.h>
#include "renderer/editor/game_view.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/crt.h"
#include "core/math.h"
#include "core/path.h"
#include "core/sort.h"
#include "core/stream.h"
#include "core/string.h"
#include "core/os.h"
#include "core/thread.h"
#include "core/atomic.h"
#include "core/sync.h"
#include "core/log.h"

using namespace Lumix;

namespace {

static const char* skipWhitespaces(const char* c, const char* end) {
	while (c != end && isWhitespace(*c)) ++c;
	return c;
}

// Parse a decimal number (optional minus, digits, fraction, exponent) starting at `c`.
// Returns the first character after the number, or nullptr if there is no number or it does not fit in a double.
static const char* parseDouble(const char* c, const char* end, double& result) {
	const bool negative = c != end && *c == '-';
	if (negative) ++c;
	double value = 0;
	i32 exponent = 0;
	bool has_digits = false;
	for (; c != end && isNumeric(*c); ++c) {
		value = value * 10 + (*c - '0');
		has_digits = true;
	}
	if (c != end && *c == '.') {
		for (++c; c != end && isNumeric(*c); ++c) {
			value = value * 10 + (*c - '0');
			--exponent;
			has_digits = true;
		}
	}
	if (!has_digits) return nullptr;
	if (c != end && (*c == 'e' || *c == 'E')) {
		const char* e = c + 1;
		const bool negative_exponent = e != end && *e == '-';
		if (e != end && (*e == '-' || *e == '+')) ++e;
		// an `e` without digits is not a part of the number
		if (e != end && isNumeric(*e)) {
			i32 n = 0;
			for (; e != end && isNumeric(*e); ++e) {
				if (n < 100000) n = n * 10 + (*e - '0');
			}
			exponent += negative_exponent ? -n : n;
			c = e;
		}
	}
	if (exponent < 0) value /= pow(10.0, (double)-exponent);
	else if (exponent > 0) value *= pow(10.0, (double)exponent);
	// also catches NaN
	if (!(value <= DBL_MAX)) return nullptr;
	result = negative ? -value : value;
	return c;
}

// Decode a JSON number. Reject partial parses and values which do not fit in a double.
static bool parseNumber(StringView raw, double& result) {
	return !raw.empty() && parseDouble(raw.begin(), raw.end(), result) == raw.end();
}

static bool parseIndex(StringView raw, i32& result) {
	if (raw.empty()) return false;
	i64 value = 0;
	for (char c : raw) {
		if (!isNumeric(c)) return false;
		value = value * 10 + (c - '0');
		if (value > 2147483647) return false;
	}
	result = (i32)value;
	return true;
}

// Key names for send_input: a single letter or digit, a name of os::Keycode (SPACE, TAB, RETURN, ESCAPE, SHIFT, CTRL, ALT, arrows, F1..F12, ...)
// or the numeric virtual key code.
static bool parseKey(StringView raw, u8& result) {
	if (raw.empty()) return false;
	if (raw.size() == 1) {
		const char c = raw[0];
		if (c >= 'a' && c <= 'z') { result = (u8)(c - 'a' + 'A'); return true; }
		if (isUpperCase(c) || isNumeric(c)) { result = (u8)c; return true; }
	}
	static const struct { const char* name; os::Keycode code; } KEYS[] = {
		{"SPACE", os::Keycode::SPACE}, {"TAB", os::Keycode::TAB}, {"RETURN", os::Keycode::RETURN}, {"ENTER", os::Keycode::RETURN}, {"ESCAPE", os::Keycode::ESCAPE},
		{"ESC", os::Keycode::ESCAPE}, {"SHIFT", os::Keycode::SHIFT}, {"CTRL", os::Keycode::CTRL}, {"ALT", os::Keycode::ALT}, {"BACKSPACE", os::Keycode::BACKSPACE},
		{"LEFT", os::Keycode::LEFT}, {"UP", os::Keycode::UP}, {"RIGHT", os::Keycode::RIGHT}, {"DOWN", os::Keycode::DOWN}, {"PAGEUP", os::Keycode::PAGEUP},
		{"PAGEDOWN", os::Keycode::PAGEDOWN}, {"HOME", os::Keycode::HOME}, {"END", os::Keycode::END}, {"INSERT", os::Keycode::INSERT}, {"DELETE", os::Keycode::DEL},
		{"DEL", os::Keycode::DEL}, {"PAUSE", os::Keycode::PAUSE},
	};
	for (const auto& k : KEYS) if (equalIStrings(raw, k.name)) { result = (u8)k.code; return true; }
	if (raw.size() >= 2 && raw.size() <= 3 && toLower(raw[0]) == 'f') {
		i32 n;
		if (parseIndex(raw.withoutLeft(1), n) && n >= 1 && n <= 24) { result = (u8)((i32)os::Keycode::F1 + n - 1); return true; }
	}
	i32 code;
	if (parseIndex(raw, code) && code > 0 && code < 256) { result = (u8)code; return true; }
	return false;
}

// Decode a JSON array of `count` numbers, each of them has to fit in a float.
static bool parseVector(StringView raw, double* out, int count) {
	if (raw.size() < 2 || raw[0] != '[' || raw.back() != ']') return false;
	const char* end = raw.end();
	const char* p = raw.begin() + 1;
	for (int i = 0; i < count; ++i) {
		p = parseDouble(skipWhitespaces(p, end), end, out[i]);
		if (!p || fabs(out[i]) > FLT_MAX) return false;
		p = skipWhitespaces(p, end);
		if (p == end || *p != (i + 1 == count ? ']' : ',')) return false;
		++p;
	}
	return skipWhitespaces(p, end) == end;
}

// A project-relative path with the given extension, without drive letters, backslashes or traversal.
static bool isProjectPath(StringView path, StringView extension, u32 max_size) {
	return path.size() > extension.size() && path.size() < max_size && endsWith(path, extension)
		&& path[0] != '/' && !contains(path, ':') && !contains(path, '\\') && !find(path, "..");
}

static StringView toView(const OutputMemoryStream& blob) { return StringView((const char*)blob.data(), blob.size()); }

static void writeJSONString(OutputMemoryStream& out, StringView s) {
	const char* hex = "0123456789abcdef";
	out << "\"";
	for (const char ch : s) {
		const u8 c = (u8)ch;
		if (c == '"' || c == '\\') { out << "\\"; out.write(ch); }
		else if (c == '\n') out << "\\n";
		else if (c == '\r') out << "\\r";
		else if (c == '\t') out << "\\t";
		else if (c < 0x20) { out << "\\u00"; out.write(hex[c >> 4]); out.write(hex[c & 15]); }
		else out.write(ch);
	}
	out << "\"";
}

// IOutputStream's own float formatting has no room for huge values, and infinity is not a JSON number.
static void writeJSONNumber(OutputMemoryStream& out, float value) {
	char tmp[64];
	if (fabs(value) <= FLT_MAX && toCString(value, Span(tmp), 6)) out << tmp;
	else out << "0";
}

// Find `"key":` in `s`, returns the first character after the colon or nullptr.
static const char* findField(StringView s, const char* key) {
	const StaticString<64> needle("\"", key, "\"");
	const char* p = find(s, needle);
	if (!p) return nullptr;
	p = find(StringView(p + stringLength(needle), s.end()), ':');
	return p ? p + 1 : nullptr;
}

// The raw text of a value: a whole string (quotes included), a flat array or a bare token. Empty if there is none.
static StringView fieldValue(StringView s, const char* key) {
	const char* end = s.end();
	const char* p = findField(s, key);
	if (!p) return {};
	p = skipWhitespaces(p, end);
	if (p == end) return {};
	const char* start = p;
	if (*p == '[') {
		p = find(StringView(p, end), ']');
		return p ? StringView(start, p + 1) : StringView();
	}
	if (*p == '"') {
		for (++p; p != end; ++p) {
			if (*p == '\\' && p + 1 != end) { ++p; continue; }
			if (*p == '"') return StringView(start, p + 1);
		}
		return {};
	}
	while (p != end && *p != ',' && *p != '}' && !isWhitespace(*p)) ++p;
	return StringView(start, p);
}

// Like fieldValue, but meant to be echoed back as is (a JSON-RPC id); `null` if there is none.
static StringView fieldRaw(StringView s, const char* key) {
	const char* end = s.end();
	const char* p = findField(s, key);
	if (!p) return "null";
	while (p != end && (*p == ' ' || *p == '\t')) ++p;
	if (p != end && *p == '"') {
		const char* closing = find(StringView(p + 1, end), '"');
		return closing ? StringView(p, closing + 1) : StringView("null");
	}
	const char* token_end = p;
	while (token_end != end && *token_end != ',' && *token_end != '}') ++token_end;
	return StringView(p, token_end);
}

// Decoded value of a string field; empty if there is none or it contains an unsupported escape.
static String fieldString(StringView s, const char* key, IAllocator& allocator) {
	String out(allocator);
	const char* end = s.end();
	const char* p = findField(s, key);
	if (!p) return out;
	p = find(StringView(p, end), '"');
	if (!p) return out;
	const char* begin = p + 1;
	const char* closing = begin;
	while (closing != end && *closing != '"') closing += *closing == '\\' && closing + 1 != end ? 2 : 1;
	if (closing == end) return out;

	// decoding never makes the string longer
	out.resize(u32(closing - begin));
	char* dst = out.getMutableData();
	for (p = begin; p != closing; ++p) {
		char c = *p;
		if (c == '\\') {
			switch (*++p) {
				case 'n': c = '\n'; break;
				case 'r': c = '\r'; break;
				case 't': c = '\t'; break;
				case '"': c = '"'; break;
				case '\\': c = '\\'; break;
				default: return String(allocator); // Invalid/unsupported escape.
			}
		}
		*dst++ = c;
	}
	out.resize(u32(dst - out.getMutableData()));
	return out;
}

struct SetPropertyVisitor final : reflection::IEmptyPropertyVisitor {
	WorldEditor& editor;
	ComponentType component;
	EntityRef entity;
	const char* property;
	StringView raw;
	const char* decoded;
	bool is_string;
	bool success = false;
	const char* reason = "Unsupported property type or value";

	SetPropertyVisitor(WorldEditor& editor, ComponentType component, EntityRef entity, const char* property,
		StringView raw, const char* decoded, bool is_string)
		: editor(editor), component(component), entity(entity), property(property), raw(raw), decoded(decoded), is_string(is_string) {}

	static bool isI32(double n) { return n >= -2147483648.0 && n <= 2147483647.0 && floor(n) == n; }

	template <typename T> void apply(const reflection::Property<T>& prop, T value) {
		if (prop.isReadonly()) { reason = "Property is read-only"; return; }
		editor.setProperty(component, "", -1, property, Span(&entity, 1), value);
		success = true;
	}
	void visit(const reflection::Property<float>& prop) override {
		double n; if (!is_string && parseNumber(raw, n) && fabs(n) <= FLT_MAX) apply(prop, (float)n);
	}
	void visit(const reflection::Property<int>& prop) override {
		double n; if (!is_string && parseNumber(raw, n) && isI32(n)) apply(prop, (int)n);
	}
	void visit(const reflection::Property<u32>& prop) override {
		double n; if (!is_string && parseNumber(raw, n) && n >= 0 && n <= 4294967295.0 && floor(n) == n) apply(prop, (u32)n);
	}
	void visit(const reflection::Property<EntityPtr>& prop) override {
		if (raw == "null") { apply(prop, INVALID_ENTITY); return; }
		i32 idx; if (!is_string && parseIndex(raw, idx) && editor.getWorld()->hasEntity(EntityRef(idx))) apply(prop, EntityPtr{idx});
	}
	void visit(const reflection::Property<bool>& prop) override {
		if (raw == "true") apply(prop, true);
		else if (raw == "false") apply(prop, false);
	}
	void visit(const reflection::Property<const char*>& prop) override { if (is_string) apply(prop, decoded); }
	void visit(const reflection::Property<Path>& prop) override { if (is_string) apply(prop, Path(decoded)); }
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
		double n[3]; if (!is_string && parseVector(raw, n, 3) && isI32(n[0]) && isI32(n[1]) && isI32(n[2]))
			apply(prop, IVec3((i32)n[0], (i32)n[1], (i32)n[2]));
	}
};

// In-process, sessionless MCP over Streamable HTTP, bound to loopback only.
struct RemoteControl final : StudioApp::IPlugin {
	static constexpr u32 MAX_MESSAGE = 1024 * 1024;
	static constexpr u32 MAX_HEADERS = 8192;
	static constexpr u16 PORT = 17123;
	struct TcpThread final : Thread {
		RemoteControl& owner;
		TcpThread(IAllocator& allocator, RemoteControl& owner) : Thread(allocator), owner(owner) {}
		i32 task() override;
	};

	StudioApp& app;
	Mutex mutex;
	Semaphore done{0, 2};
	String pending;
	OutputMemoryStream response;
	bool has_pending = false;
	AtomicI32 stop{0};
	os::NetworkStream* volatile client = nullptr;
	TcpThread* thread = nullptr;
	bool network_initialized = false;
	// send_input spreads a click or a key tap over consecutive frames (move, down, up), the way a real device delivers them.
	struct PendingInput { i32 frames; InputSystem::Event event; };
	Array<PendingInput> pending_input;
	float mouse_x = 0;
	float mouse_y = 0;

	explicit RemoteControl(StudioApp& app)
		: app(app)
		, pending(app.getAllocator())
		, response(app.getAllocator())
		, pending_input(app.getAllocator())
	{
#ifdef _WIN32
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
			auto* wake = os::connect("127.0.0.1", PORT, app.getAllocator());
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
		if (!pending_input.empty()) {
			// Dropped when the game stops: a stale key-up must not leak into the next run.
			if (!app.getWorldEditor().isGameMode()) pending_input.clear();
			InputSystem& input = app.getEngine().getInputSystem();
			for (i32 i = 0; i < pending_input.size();) {
				if (--pending_input[i].frames > 0) { ++i; continue; }
				input.injectEvent(pending_input[i].event);
				pending_input.erase(i);
			}
		}
		String request(app.getAllocator());
		mutex.enter();
		if (has_pending) { request = static_cast<String&&>(pending); has_pending = false; }
		mutex.exit();
		if (request.length() == 0) return;
		response.clear();
		handle(request);
		done.signal();
	}

	// Result of a tool call: `text` is for the model, `structured` is optional JSON for structuredContent.
	void reply(StringView id, StringView text, StringView structured = {}) {
		response << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":";
		writeJSONString(response, text);
		response << "}]";
		if (!structured.empty()) response << ",\"structuredContent\":" << structured;
		response << "}}";
	}

	void error(StringView id, int code, const char* message) {
		response << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"error\":{\"code\":" << code << ",\"message\":";
		writeJSONString(response, message);
		response << "}}";
	}

	// A notification gets no reply: `response` stays empty and the worker answers with 202.
	void handle(StringView msg) {
		IAllocator& allocator = app.getAllocator();
		const StringView id = fieldRaw(msg, "id");
		const String method = fieldString(msg, "method", allocator);
		if (method == "initialize") {
			response << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"result\":{\"protocolVersion\":\"2025-03-26\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"lumix-remote\",\"version\":\"0.1.0\"}}}";
		}
		else if (method == "notifications/initialized") {}
		else if (method == "tools/list") {
			response << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"result\":{\"tools\":["
				"{\"name\":\"create_entity\",\"description\":\"Create an entity in the active world.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"additionalProperties\":false}},"
				"{\"name\":\"new_world\",\"description\":\"Create a new world in Studio. If the current world has unsaved changes, Studio asks for confirmation instead.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
				"{\"name\":\"save_world\",\"description\":\"Save all named world partitions. For a new unnamed world, provide a .unv path relative to the project.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"additionalProperties\":false}},"
				"{\"name\":\"make_screenshot\",\"description\":\"Queue a TGA screenshot of Studio's scene view to a project-relative path. The file is written asynchronously.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"],\"additionalProperties\":false}},"
				"{\"name\":\"make_game_screenshot\",\"description\":\"Queue a TGA screenshot of Studio's game view (in-game UI included) to a project-relative path. The game view window must be visible; the file is written asynchronously.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"],\"additionalProperties\":false}},"
				"{\"name\":\"start_game\",\"description\":\"Enter game mode in Studio (the same as the Game Mode toggle). Does nothing if the game is already running.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
				"{\"name\":\"stop_game\",\"description\":\"Leave game mode in Studio; the world is restored to its state before the game started. Takes effect on Studio's next frame.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
				"{\"name\":\"send_input\",\"description\":\"Inject keyboard or mouse input into the running game (game mode only). type: key (key, optional down; without down it is a tap), text (text), mouse_move (x, y in game view pixels, or dx, dy), mouse_button (button left|right|middle, optional x, y, optional down; without down it is a click), mouse_wheel (amount).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\",\"enum\":[\"key\",\"text\",\"mouse_move\",\"mouse_button\",\"mouse_wheel\"]},\"key\":{\"type\":\"string\"},\"down\":{\"type\":\"boolean\"},\"text\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"dx\":{\"type\":\"number\"},\"dy\":{\"type\":\"number\"},\"button\":{\"type\":\"string\",\"enum\":[\"left\",\"right\",\"middle\"]},\"amount\":{\"type\":\"number\"}},\"required\":[\"type\"],\"additionalProperties\":false}},"
				"{\"name\":\"load_world\",\"description\":\"Load a project-relative .unv world. Set additive to true to load it as another partition; otherwise replace the current world (Studio prompts if there are unsaved changes).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"additive\":{\"type\":\"boolean\"}},\"required\":[\"path\"],\"additionalProperties\":false}},"
				"{\"name\":\"list_assets\",\"description\":\"List indexed project assets, sorted by path, with optional path prefix and pagination.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prefix\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\",\"minimum\":0},\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}},\"additionalProperties\":false}},"
				"{\"name\":\"add_component\",\"description\":\"Add a reflected component to an entity.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"entity_id\":{\"type\":\"integer\"},\"component\":{\"type\":\"string\"}},\"required\":[\"entity_id\",\"component\"],\"additionalProperties\":false}},"
				"{\"name\":\"set_property\",\"description\":\"Set a top-level reflected component property; vectors are numeric arrays.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"entity_id\":{\"type\":\"integer\"},\"component\":{\"type\":\"string\"},\"property\":{\"type\":\"string\"},\"value\":{}},\"required\":[\"entity_id\",\"component\",\"property\",\"value\"],\"additionalProperties\":false}},"
				"{\"name\":\"evox_execute\",\"description\":\"Compile and execute Evox source in a fresh runtime against the current world. Source must define fn main(world : World) : void.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"}},\"required\":[\"code\"],\"additionalProperties\":false}}]}}";
		}
		else if (method == "tools/call") {
			const String tool = fieldString(msg, "name", allocator);
			if (tool != "create_entity" && tool != "new_world" && tool != "save_world" && tool != "make_screenshot" && tool != "make_game_screenshot" && tool != "start_game" && tool != "stop_game" && tool != "send_input" && tool != "load_world" && tool != "evox_execute" && tool != "add_component" && tool != "set_property" && tool != "list_assets") { error(id, -32602, "Unknown tool"); return; }
			// empty if the call has no arguments
			const char* args_begin = find(msg, "\"arguments\"");
			const StringView args = args_begin ? StringView(args_begin, msg.end()) : StringView();
			if (tool == "list_assets") {
				const String prefix = fieldString(args, "prefix", allocator);
				i32 offset = 0;
				i32 limit = 100;
				const StringView offset_raw = fieldValue(args, "offset");
				const StringView limit_raw = fieldValue(args, "limit");
				if ((!offset_raw.empty() && !parseIndex(offset_raw, offset))
					|| (!limit_raw.empty() && (!parseIndex(limit_raw, limit) || limit < 1 || limit > 500))) {
					error(id, -32602, "Invalid offset or limit (limit must be 1..500)"); return;
				}
				struct AssetInfo {
					Path path;
					ResourceType type;
				};
				Array<AssetInfo> assets(allocator);
				AssetCompiler& compiler = app.getAssetCompiler();
				const auto& resources = compiler.lockResources();
				for (const AssetCompiler::ResourceItem& item : resources) {
					if (startsWith(item.path, prefix)) assets.push(AssetInfo{item.path, item.type});
				}
				compiler.unlockResources();
				sort(assets.begin(), assets.end(), [](const AssetInfo& a, const AssetInfo& b) { return compareString(a.path, b.path) < 0; });
				OutputMemoryStream payload(allocator);
				payload << "{\"assets\":[";
				const u32 total = (u32)assets.size();
				const u32 start = minimum((u32)offset, total);
				const u32 end = minimum(start + (u32)limit, total);
				for (u32 i = start; i < end; ++i) {
					if (i != start) payload << ",";
					const char* label = "";
					for (AssetBrowser::IPlugin* plugin : app.getAssetBrowser().getPlugins()) {
						if (plugin->getResourceType() == assets[i].type) { label = plugin->getLabel(); break; }
					}
					payload << "{\"path\":";
					writeJSONString(payload, assets[i].path);
					payload << ",\"type\":";
					writeJSONString(payload, label);
					payload << ",\"type_hash\":\"" << assets[i].type.type.getHashValue() << "\"}";
				}
				payload << "],\"total\":" << total;
				if (end < total) payload << ",\"next_offset\":" << end;
				payload << "}";
				reply(id, toView(payload), toView(payload));
				return;
			}
			if (tool == "new_world") {
				const bool needs_confirmation = app.getWorldEditor().isWorldChanged();
				app.newWorld();
				const StaticString<96> payload("{\"created\":", needs_confirmation ? "false" : "true", ",\"confirmation_required\":", needs_confirmation ? "true" : "false", "}");
				reply(id, needs_confirmation ? "Studio is waiting for confirmation to discard unsaved changes" : "New world created", payload);
				return;
			}
			if (tool == "make_screenshot" || tool == "make_game_screenshot") {
				const String path = fieldString(args, "path", allocator);
				if (!isProjectPath(path, ".tga", MAX_PATH)) { error(id, -32602, "Expected a project-relative .tga path without traversal"); return; }
				const bool is_game = tool == "make_game_screenshot";
				if (is_game) {
					auto* plugin = app.getGUIPlugin("game_view");
					if (!plugin) { error(id, -32000, "Game view unavailable"); return; }
					auto* view = static_cast<GameView*>(plugin);
					if (!view->makeScreenshot(path)) {
						error(id, -32000, "Game view has no renderable size (open the Game View window, or enable merging it with the scene view)"); return;
					}
				}
				else {
					auto* plugin = app.getGUIPlugin("scene_view");
					if (!plugin) { error(id, -32000, "Scene view unavailable"); return; }
					auto* view = static_cast<SceneView*>(plugin);
					if (!view->getPipeline() || view->getPipeline()->getDisplaySize().x <= 0 || view->getPipeline()->getDisplaySize().y <= 0) {
						error(id, -32000, "Scene view has no renderable size"); return;
					}
					view->makeScreenshot(path);
				}
				OutputMemoryStream payload(allocator);
				payload << "{\"path\":";
				writeJSONString(payload, path);
				payload << ",\"queued\":true}";
				const StaticString<MAX_PATH + 96> text(is_game ? "Game view screenshot queued: " : "Screenshot queued: ", path.c_str(), " (check Studio logs for write errors)");
				reply(id, text, toView(payload));
				return;
			}
			World* world = app.getWorldEditor().getWorld();
			if (!world) { error(id, -32000, "No active world"); return; }
			if (tool == "send_input") {
				if (!app.getWorldEditor().isGameMode()) { error(id, -32000, "The game is not running (call start_game first)"); return; }
				if (args.empty()) { error(id, -32602, "Missing arguments"); return; }
				const String type = fieldString(args, "type", allocator);
				InputSystem& input = app.getEngine().getInputSystem();
				InputSystem::Device* mouse = nullptr;
				InputSystem::Device* keyboard = nullptr;
				for (InputSystem::Device* device : input.getDevices()) {
					if (device->type == InputDeviceType::MOUSE && !mouse) mouse = device;
					if (device->type == InputDeviceType::KEYBOARD && !keyboard) keyboard = device;
				}
				if (!mouse || !keyboard) { error(id, -32000, "Input devices unavailable"); return; }
				// frame 0 = visible to the game's next update, frame n = n updates later
				auto push = [&](const InputSystem::Event& event, i32 frame) {
					if (frame == 0) input.injectEvent(event);
					else pending_input.push(PendingInput{frame, event});
				};
				auto moveTo = [&](float x, float y, float dx, float dy) {
					InputSystem::Event e;
					memset(&e, 0, sizeof(e));
					e.type = InputEventType::AXIS;
					e.device = mouse;
					e.data.axis.x_abs = x;
					e.data.axis.y_abs = y;
					e.data.axis.x = dx;
					e.data.axis.y = dy;
					push(e, 0);
					mouse_x = x;
					mouse_y = y;
				};
				const StringView down_raw = fieldValue(args, "down");
				// The flat field lookup can land on a string value such as "key":"down"; only a real boolean counts.
				const bool has_down = down_raw == "true" || down_raw == "false";
				const bool down = down_raw == "true";
				double x = 0, y = 0;
				const bool has_x = parseNumber(fieldValue(args, "x"), x);
				const bool has_y = parseNumber(fieldValue(args, "y"), y);
				if (type == "key") {
					u8 code;
					if (!parseKey(fieldString(args, "key", allocator), code)) { error(id, -32602, "Unknown key (use a letter, a digit, a key name such as SPACE or F5, or a virtual key code)"); return; }
					InputSystem::Event e;
					memset(&e, 0, sizeof(e));
					e.type = InputEventType::KEYBOARD;
					e.device = keyboard;
					e.data.keyboard.keycode = (os::Keycode)code;
					e.data.keyboard.down = has_down ? down : true;
					push(e, 0);
					if (!has_down) { e.data.keyboard.down = false; push(e, 1); }
				}
				else if (type == "text") {
					const String text = fieldString(args, "text", allocator);
					if (text.length() == 0) { error(id, -32602, "text must be a nonempty string"); return; }
					for (u32 i = 0; i < text.length();) {
						// one event per UTF-8 sequence, its bytes packed the way os::Event::text_input delivers them
						const u8 c = (u8)text[i];
						const u32 len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
						u32 utf8 = 0;
						for (u32 k = 0; k < len && i + k < text.length(); ++k) utf8 |= (u32)(u8)text[i + k] << (8 * k);
						i += len;
						InputSystem::Event e;
						memset(&e, 0, sizeof(e));
						e.type = InputEventType::TEXT_INPUT;
						e.device = keyboard;
						e.data.text.utf8 = utf8;
						push(e, 0);
					}
				}
				else if (type == "mouse_move") {
					double dx = 0, dy = 0;
					const bool relative = parseNumber(fieldValue(args, "dx"), dx) | parseNumber(fieldValue(args, "dy"), dy);
					if (has_x && has_y) moveTo((float)x, (float)y, (float)x - mouse_x, (float)y - mouse_y);
					else if (relative) moveTo(mouse_x + (float)dx, mouse_y + (float)dy, (float)dx, (float)dy);
					else { error(id, -32602, "mouse_move needs x and y (game view pixels) or dx / dy"); return; }
				}
				else if (type == "mouse_button") {
					const String name = fieldString(args, "button", allocator);
					os::MouseButton button = os::MouseButton::LEFT;
					if (name == "right") button = os::MouseButton::RIGHT;
					else if (name == "middle") button = os::MouseButton::MIDDLE;
					else if (name.length() != 0 && name != "left") { error(id, -32602, "button must be left, right or middle"); return; }
					if (has_x != has_y) { error(id, -32602, "Give both x and y, or neither"); return; }
					i32 frame = 0;
					// move first so hover state is up to date before the button changes
					if (has_x) { moveTo((float)x, (float)y, (float)x - mouse_x, (float)y - mouse_y); frame = 1; }
					InputSystem::Event e;
					memset(&e, 0, sizeof(e));
					e.type = InputEventType::MOUSE_BUTTON;
					e.device = mouse;
					e.data.mouse_button.button = button;
					e.data.mouse_button.x = mouse_x;
					e.data.mouse_button.y = mouse_y;
					e.data.mouse_button.down = has_down ? down : true;
					push(e, frame);
					if (!has_down) { e.data.mouse_button.down = false; push(e, frame + 1); }
				}
				else if (type == "mouse_wheel") {
					double amount = 0;
					if (!parseNumber(fieldValue(args, "amount"), amount)) { error(id, -32602, "mouse_wheel needs a numeric amount"); return; }
					InputSystem::Event e;
					memset(&e, 0, sizeof(e));
					e.type = InputEventType::MOUSE_WHEEL;
					e.device = mouse;
					e.data.mouse_wheel.y = (float)amount;
					push(e, 0);
				}
				else { error(id, -32602, "type must be key, text, mouse_move, mouse_button or mouse_wheel"); return; }
				OutputMemoryStream payload(allocator);
				payload << "{\"type\":";
				writeJSONString(payload, type);
				payload << ",\"mouse\":[";
				writeJSONNumber(payload, mouse_x);
				payload << ",";
				writeJSONNumber(payload, mouse_y);
				payload << "]}";
				reply(id, StaticString<64>("Input queued: ", type.c_str()), toView(payload));
				return;
			}
			if (tool == "start_game" || tool == "stop_game") {
				WorldEditor& editor = app.getWorldEditor();
				const bool start = tool == "start_game";
				const bool was_running = editor.isGameMode();
				if (start && !was_running) {
					if (editor.isLoading()) { error(id, -32000, "Cannot start the game while a world is loading"); return; }
					editor.toggleGameMode();
				}
				// Stopping reloads the world, so it is deferred to Studio's next frame like the editor's own exit path.
				else if (!start && was_running) app.exitGameMode();
				const bool changed = start != was_running;
				const char* text = start ? (changed ? "Game mode started" : "Game mode is already running")
					: (changed ? "Game mode stop requested" : "Game mode is not running");
				const StaticString<64> payload("{\"game_mode\":", start ? "true" : "false", ",\"changed\":", changed ? "true" : "false", "}");
				reply(id, text, payload);
				return;
			}
			if (tool == "load_world") {
				WorldEditor& editor = app.getWorldEditor();
				if (editor.isGameMode() || editor.isLoading()) { error(id, -32000, "Cannot load world while playing or loading"); return; }
				const String path = fieldString(args, "path", allocator);
				if (!isProjectPath(path, ".unv", sizeof(world->getPartitions()[0].name))) {
					error(id, -32602, "Expected a project-relative .unv path (max 63 characters) without traversal"); return;
				}
				// Path normalizes, partitions are named by the normalized path
				const Path world_path(path.c_str());
				const StringView additive_raw = fieldValue(args, "additive");
				if (!additive_raw.empty() && !(additive_raw == "true") && !(additive_raw == "false")) {
					error(id, -32602, "additive must be a boolean"); return;
				}
				const bool additive = additive_raw == "true";
				const auto& partitions = world->getPartitions();
				if (additive) {
					if (partitions.size() == 1 && !partitions[0].name[0]) {
						error(id, -32602, "Save the current world before loading additively"); return;
					}
					for (const World::Partition& partition : partitions) {
						if (world_path == partition.name) { error(id, -32602, "World is already loaded"); return; }
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
						if (world_path == partition.name) { loaded = true; break; }
					}
					if (!loaded) { error(id, -32000, "World failed to load; check Studio logs"); return; }
				}
				OutputMemoryStream payload(allocator);
				payload << "{\"path\":";
				writeJSONString(payload, world_path);
				payload << ",\"additive\":" << (additive ? "true" : "false") << ",\"loaded\":" << (loaded ? "true" : "false")
					<< ",\"confirmation_required\":" << (confirmation_required ? "true" : "false") << "}";
				reply(id, confirmation_required ? "Studio is waiting for confirmation to discard unsaved changes" : "World loaded", toView(payload));
				return;
			}
			if (tool == "save_world") {
				WorldEditor& editor = app.getWorldEditor();
				if (editor.isGameMode()) { error(id, -32000, "Cannot save while the game is running"); return; }
				const String path = fieldString(args, "path", allocator);
				const auto& partitions = world->getPartitions();
				const bool unnamed = partitions.size() == 1 && !partitions[0].name[0];
				if (unnamed) {
					if (path.length() == 0) { error(id, -32602, "New world requires a .unv path relative to the project"); return; }
					if (!isProjectPath(path, ".unv", sizeof(partitions[0].name))) {
						error(id, -32602, "Invalid project-relative .unv path (max 63 characters)"); return;
					}
					copyString(world->getPartition(partitions[0].handle).name, path);
				}
				else if (path.length() != 0) { error(id, -32602, "Path is only supported for a new unnamed world"); return; }
				for (const World::Partition& partition : partitions) editor.savePartition(partition.handle);
				reply(id, "World save requested");
				return;
			}
			if (tool == "evox_execute") {
				if (args.empty()) { error(id, -32602, "Missing arguments"); return; }
				const String code = fieldString(args, "code", allocator);
				if (code.length() == 0 || code.length() > 256 * 1024) { error(id, -32602, "Code must be nonempty and at most 256 KB"); return; }
				auto* evox = static_cast<EvoxSystem*>(app.getEngine().getSystemManager().getSystem("evox_system"));
				if (!evox) { error(id, -32000, "Evox system unavailable"); return; }
				String output(allocator);
				const bool ok = evox->executeSource(code, *world, output);
				const StringView text = output.length() != 0 ? StringView(output) : StringView(ok ? "Evox execution completed" : "Evox execution failed");
				response << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":";
				writeJSONString(response, text);
				response << "}],\"isError\":" << (ok ? "false" : "true") << "}}";
				return;
			}
			if (tool == "add_component" || tool == "set_property") {
				if (args.empty()) { error(id, -32602, "Missing arguments"); return; }
				i32 index;
				if (!parseIndex(fieldRaw(args, "entity_id"), index) || !world->hasEntity(EntityRef(index))) {
					error(id, -32602, "Invalid entity_id"); return;
				}
				const EntityRef entity(index);
				const String name = fieldString(args, "component", allocator);
				if (name.length() == 0 || !reflection::componentTypeExists(name.c_str())) {
					error(id, -32602, "Unknown component"); return;
				}
				const ComponentType component = reflection::getComponentType(name.c_str());
				if (tool == "add_component") {
					if (world->hasComponent(entity, component)) { error(id, -32602, "Entity already has component"); return; }
					app.getWorldEditor().addComponent(Span(&entity, 1), component);
					if (!world->hasComponent(entity, component)) { error(id, -32000, "Could not add component"); return; }
				} else {
					if (!world->hasComponent(entity, component)) { error(id, -32602, "Entity does not have component"); return; }
					const String property = fieldString(args, "property", allocator);
					const reflection::PropertyBase* desc = property.length() == 0 ? nullptr : reflection::getProperty(component, property.c_str());
					if (!desc) { error(id, -32602, "Unknown property"); return; }
					const StringView raw = fieldValue(args, "value");
					if (raw.empty()) { error(id, -32602, "Missing or invalid value"); return; }
					const bool is_string = raw[0] == '"';
					String decoded(allocator);
					if (is_string) decoded = fieldString(args, "value", allocator);
					SetPropertyVisitor visitor(app.getWorldEditor(), component, entity, property.c_str(), raw, decoded.c_str(), is_string);
					desc->visit(visitor);
					if (!visitor.success) { error(id, -32602, visitor.reason); return; }
				}
				reply(id, tool == "add_component" ? "Component added" : "Property set");
				return;
			}
			const EntityRef entity = world->createEntity(DVec3(0.0), Quat(0, 0, 0, 1));
			const String name = fieldString(args, "name", allocator);
			if (name.length() != 0) world->setEntityName(entity, name);
			reply(id, StaticString<64>("Created entity ", entity.index), StaticString<64>("{\"entity_id\":", entity.index, "}"));
		}
		else if (!id.empty()) error(id, -32601, "Method not found");
	}
};

// Socket I/O stays on the worker. All engine/editor work happens in update() on the main thread.
i32 RemoteControl::TcpThread::task() {
	IAllocator& allocator = owner.app.getAllocator();
	os::NetworkListener* listener = os::createListener("127.0.0.1", PORT, allocator);
	if (!listener) {
		if (!owner.stop) logError("Remote MCP: could not listen on 127.0.0.1:", PORT);
		return 0;
	}
	auto writeHttp = [](os::NetworkStream& stream, int code, const char* status, StringView body) {
		const StaticString<256> head("HTTP/1.1 ", code, " ", status
			, "\r\nContent-Type: application/json\r\nContent-Length: ", body.size()
			, "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n");
		if (!os::write(stream, head.data, (u32)stringLength(head.data))) return;
		if (!body.empty()) os::write(stream, body.data, (u32)body.size());
	};
	while (!owner.stop) {
		os::NetworkStream* stream = os::accept(*listener);
		if (!stream) {
			if (!owner.stop) logError("Remote MCP: accept failed on 127.0.0.1:", PORT);
			break;
		}
		owner.client = stream;
		if (owner.stop) goto close_client;
		{
			char buffer[MAX_HEADERS];
			u32 size = 0;
			char c;
			while (!owner.stop && size < MAX_HEADERS && os::read(*stream, &c, 1) == os::NetworkReadResult::SUCCESS) {
				buffer[size] = c;
				++size;
				if (endsWith(StringView(buffer, size), "\r\n\r\n")) break;
			}
			if (owner.stop) goto close_client;
			const StringView headers(buffer, size);
			if (!endsWith(headers, "\r\n\r\n")) {
				writeHttp(*stream, 400, "Bad Request", ""); goto close_client;
			}
			const bool is_post = startsWith(headers, "POST /mcp HTTP/1.1\r\n");
			// header names are case-insensitive
			for (u32 i = 0; i < size; ++i) buffer[i] = toLower(buffer[i]);
			auto header = [&](const char* key) -> StringView {
				const StaticString<32> needle("\r\n", key, ":");
				const char* value = find(headers, needle);
				if (!value) return {};
				value += stringLength(needle);
				while (value != headers.end() && *value == ' ') ++value;
				const char* value_end = find(StringView(value, headers.end()), "\r\n");
				return StringView(value, value_end ? value_end : headers.end());
			};
			const StringView host = header("host");
			const StaticString<32> expected_host("127.0.0.1:", (u32)PORT);
			const StaticString<32> alt_host("localhost:", (u32)PORT);
			const StringView origin = header("origin");
			if (!is_post) {
				writeHttp(*stream, 405, "Method Not Allowed", ""); goto close_client;
			}
			i32 length;
			if (!parseIndex(header("content-length"), length) || length <= 0 || length > (i32)MAX_MESSAGE) {
				writeHttp(*stream, 400, "Bad Request", ""); goto close_client;
			}
			String request(allocator);
			request.resize((u32)length);
			if (os::read(*stream, request.getMutableData(), (u32)length) != os::NetworkReadResult::SUCCESS) goto close_client;
			// Drain the body before rejecting an origin, so closing does not reset the HTTP response.
			const bool is_local_host = host == expected_host || host == alt_host;
			const bool is_same_origin = origin.empty() || (startsWith(origin, "http://") && origin.withoutLeft(7) == host);
			if (!is_local_host || !is_same_origin) {
				writeHttp(*stream, 403, "Forbidden", ""); goto close_client;
			}
			owner.mutex.enter();
			owner.pending = static_cast<String&&>(request);
			owner.has_pending = true;
			owner.mutex.exit();
			owner.done.wait();
			if (owner.stop) goto close_client;
			if (owner.response.empty()) writeHttp(*stream, 202, "Accepted", "");
			else writeHttp(*stream, 200, "OK", toView(owner.response));
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
