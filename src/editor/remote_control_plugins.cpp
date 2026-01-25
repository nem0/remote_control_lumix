#include "core/allocator.h"
#include "core/array.h"
#include "core/atomic.h"
#include "core/log.h"
#include "core/os.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/thread.h"
#include "engine/engine.h"
#include "editor/studio_app.h"
#include "lua/lua_script_system.h"
#include "lua/lua_wrapper.h"

using namespace Lumix;

static const i32 REMOTE_CONTROL_PORT = 17123;

struct EditorPlugin : StudioApp::GUIPlugin {
	EditorPlugin(StudioApp& app)
		: m_app(app)
		, m_pending_scripts(app.getAllocator())
	{
		startServer();
	}

	~EditorPlugin() {
		stopServer();
	}

	void onGUI() override {}

	void update(float) override {
		m_mutex.enter();
		Array<String> scripts(m_app.getAllocator());
		scripts.swap(m_pending_scripts);
		m_mutex.exit();

		if (scripts.empty()) return;

		LuaScriptSystem* system = (LuaScriptSystem*)m_app.getEngine().getSystemManager().getSystem("lua_script");
		if (!system) {
			logError("Remote control: lua_script system not found");
			return;
		}

		lua_State* L = system->getState();
		for (const String& script : scripts) {
			bool errors = LuaWrapper::luaL_loadbuffer(L, script.c_str(), script.length(), "remote_control") != 0;
			errors = errors || lua_pcall(L, 0, 0, 0) != 0;
			if (errors) {
				logError("Remote control: ", lua_tostring(L, -1));
				lua_pop(L, 1);
			}
		}
	}
    
	const char* getName() const override { return "remote_control"; }

	void startServer() {
		if (!os::initNetwork()) {
			logError("Remote control: failed to initialize network system.");
			return;
		}

		IAllocator& allocator = m_app.getAllocator();
		m_thread = LUMIX_NEW(allocator, TcpThread)(allocator, this);
		if (!m_thread->create("remote_control_tcp", false)) {
			logError("Remote control: Failed to create listening thread.");
			LUMIX_DELETE(m_app.getAllocator(), m_thread);
			m_thread = nullptr;
			os::shutdownNetwork();
		}
	}

	void stopServer() {
		if (!m_thread) return;

		m_stop = 1;
		// Try to connect to self to unblock blocking accept/listen
		auto* s = os::connect("127.0.0.1", REMOTE_CONTROL_PORT, m_app.getAllocator());
		if (s) os::close(*s);

		if (m_thread) {
			m_thread->destroy();
			LUMIX_DELETE(m_app.getAllocator(), m_thread);
			m_thread = nullptr;
		}

		os::shutdownNetwork();
	}

	enum class ReadResult {
		SUCCESS,
		PENDING,
		FAILURE
	};


	struct TcpThread : Thread {
		EditorPlugin* owner;
		
		TcpThread(IAllocator& alloc, EditorPlugin* o) : Thread(alloc), owner(o) {}
		
		static ReadResult readString(StringView input, StringView& output) {
			const char* c = input.begin;
			while (c != input.end && isWhitespace(*c)) ++c;
			if (c == input.end) return ReadResult::PENDING;
			if (*c != '"') return ReadResult::PENDING;

			++c;
			output.begin = c;
			while (c != input.end && *c != '"') ++c;
			if (c == input.end) return ReadResult::PENDING;
			output.end = c;
			return ReadResult::SUCCESS;
		}

		static StringView skipWhitespace(StringView in) {
			StringView out = in;
			while (out.begin != out.end && isWhitespace(*out.begin)) ++out.begin;
			return out;
		}

		// argument `in` is a string enclosed in " (can contain escaped \")
		// argument `in` contains the lua code to execute
		ReadResult runLua(StringView in, StringView& out) {
			const char* c = in.begin;
			while (c != in.end && isWhitespace(*c)) ++c;
			if (c == in.end) return ReadResult::PENDING;
			if (*c != '"') return ReadResult::PENDING;

			++c;
			const char* start = c;
			String unescaped(owner->m_app.getAllocator());

			while (c != in.end) {
				if (*c == '\\' && c + 1 != in.end) {
					if (*(c + 1) == '"') {
						unescaped.append(StringView(start, c));
						unescaped.append("\"");
						c += 2;
						start = c;
						continue;
					} else if (*(c + 1) == '\\') {
						unescaped.append(StringView(start, c));
						unescaped.append("\\");
						c += 2;
						start = c;
						continue;
					}
				}
				if (*c == '"') {
					unescaped.append(StringView(start, c));
					out.begin = in.begin;
					out.end = c;
					// Queue the script to be executed on the main thread
					owner->m_mutex.enter();
					owner->m_pending_scripts.emplace(static_cast<String&&>(unescaped));
					owner->m_mutex.exit();
					return ReadResult::SUCCESS;
				}
				++c;
			}

			return ReadResult::PENDING;
		}

		i32 task() override {
			Array<char> buf(owner->m_app.getAllocator());
			const i32 MAX_LINE = 1024 * 1024; // 1 MB

			while (!owner->m_stop) {
				auto* client = os::listen("127.0.0.1", REMOTE_CONTROL_PORT, owner->m_app.getAllocator());
				if (!client) {
					logError("Remote control: failed to listen on 127.0.0.1.");
					break;
				}

				bool closed = false;
				while (!owner->m_stop && !closed) {
					char c;
					switch (os::read(*client, &c, 1)) {
						case os::NetworkReadResult::CLOSED: closed = true; break;
						case os::NetworkReadResult::FAILED:
							logError("Remote control: Failed to read data from the socket.");
							closed = true;
							break;
						case os::NetworkReadResult::SUCCESS: break;
					}

					if (closed) break;

					buf.push(c);
					if (buf.size() >= MAX_LINE) {
						logError("Remote control: message exceeded 1MB, closing connection");
						break;
					}

					StringView msg(buf.begin(), buf.end());
					msg = skipWhitespace(msg);
					if (msg.size() < 7) continue;
					
					if (startsWith(msg, "runLua")) {
						StringView luaStr;
						switch (runLua(msg.withoutLeft(6), luaStr)) {
							case ReadResult::SUCCESS: {
								++luaStr.end;
								const i32 remaining = i32(msg.end - luaStr.end);
								if (remaining > 0) {
									memmove(buf.begin(), luaStr.end, remaining);
								}
								buf.resize(remaining);
								break;
							}
							case ReadResult::PENDING: break;
							case ReadResult::FAILURE:
								closed = true;
								break;
						}
					}
					else if (startsWith(msg, "logError")) {
						StringView str;
						switch (readString(msg.withoutLeft(8), str)) {
							case ReadResult::SUCCESS: {
								logError(str);
								++str.end;
								const i32 remaining = i32(msg.end - str.end);
								if (remaining > 0) {
									memmove(buf.begin(), str.end, remaining);
								}
								buf.resize(remaining);
								break;
							}
							case ReadResult::PENDING: break;
							case ReadResult::FAILURE:
								closed = true;
								break;
						}
					}
				}

				os::close(*client);
			}

			return 0;
		}
	};

	StudioApp& m_app;
	TcpThread* m_thread = nullptr;
	AtomicI32 m_stop = 0;
	Mutex m_mutex;
	Array<String> m_pending_scripts;
};

LUMIX_STUDIO_ENTRY(remote_control) {
	auto* plugin = LUMIX_NEW(app.getAllocator(), EditorPlugin)(app);
	app.addPlugin(*plugin);
	return nullptr;
}
