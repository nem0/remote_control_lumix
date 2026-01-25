if plugin "remote_control" then
	files { 
		"src/**.c",
		"src/**.cpp",
		"src/**.h",
		"genie.lua"
	}
	defines { "BUILDING_REMOTE_CONTROL" }
	dynamic_link_plugin { "engine" }

	configuration { "windows" }
		links { "ws2_32" }
	configuration {}
end