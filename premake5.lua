-- Runs inside the plugin project. Paths are relative to this file .
files { "src/**.cpp", "src/**.h" }
defines { "BUILDING_REMOTE_CONTROL" }

-- plugins link each other only in dynamic builds
if _OPTIONS["dynamic-plugins"] then
	links { "core", "engine", "renderer" }
end

filter "system:windows"
	links { "ws2_32" }
filter {}
