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
		// Close client socket to unblock os::read
		os::NetworkStream* client = (os::NetworkStream*)exchangePtr((void*volatile*)&m_client, nullptr);
		if (client) os::close(*client);
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

	// Message format: <N><space><N bytes of lua code>
	// where N is an ASCII decimal number representing the length of the lua code
	struct TcpThread : Thread {
		EditorPlugin* owner;
		
		TcpThread(IAllocator& alloc, EditorPlugin* o) : Thread(alloc), owner(o) {}

		i32 task() override {
			const i32 MAX_SIZE = 1024 * 1024; // 1 MB

			while (!owner->m_stop) {
				auto* client = os::listen("127.0.0.1", REMOTE_CONTROL_PORT, owner->m_app.getAllocator());
				if (!client) {
					logError("Remote control: failed to listen on 127.0.0.1.");
					break;
				}
				owner->m_client = client;

				bool closed = false;
				while (!owner->m_stop && !closed) {
					// Read length prefix until we find a space
					i32 length = 0;
					bool found_space = false;
					while (!found_space && !closed) {
						char c;
						switch (os::read(*client, &c, 1)) {
							case os::NetworkReadResult::CLOSED: closed = true; break;
							case os::NetworkReadResult::FAILED:
								if (!owner->m_stop) logError("Remote control: Failed to read data from the socket.");
								closed = true;
								break;
							case os::NetworkReadResult::SUCCESS:
								if (c == ' ') {
									found_space = true;
								} else if (c >= '0' && c <= '9') {
									length = length * 10 + (c - '0');
									if (length > MAX_SIZE) {
										logError("Remote control: message exceeded 1MB, closing connection");
										closed = true;
									}
								} else {
									logError("Remote control: invalid message format");
									closed = true;
								}
								break;
						}
					}

					if (closed) break;

					if (length == 0) {
						logError("Remote control: invalid message length");
						break;
					}

					// Read the lua code in one go
					IAllocator& allocator = owner->m_app.getAllocator();
					char* buf = (char*)allocator.allocate(length, 1);
					switch (os::read(*client, buf, length)) {
						case os::NetworkReadResult::CLOSED: closed = true; break;
						case os::NetworkReadResult::FAILED:
							if (!owner->m_stop) logError("Remote control: Failed to read data from the socket.");
							closed = true;
							break;
						case os::NetworkReadResult::SUCCESS:
							owner->m_mutex.enter();
							owner->m_pending_scripts.emplace(StringView(buf, buf + length), allocator);
							owner->m_mutex.exit();
							break;
					}
					allocator.deallocate(buf);
				}

				// Only close if stopServer didn't already take it
				if (exchangePtr((void*volatile*)&owner->m_client, nullptr)) {
					os::close(*client);
				}
			}

			return 0;
		}
	};

	StudioApp& m_app;
	TcpThread* m_thread = nullptr;
	os::NetworkStream* volatile m_client = nullptr;
	AtomicI32 m_stop = 0;
	Mutex m_mutex;
	Array<String> m_pending_scripts;
};

LUMIX_STUDIO_ENTRY(remote_control) {
	auto* plugin = LUMIX_NEW(app.getAllocator(), EditorPlugin)(app);
	app.addPlugin(*plugin);
	return nullptr;
}
