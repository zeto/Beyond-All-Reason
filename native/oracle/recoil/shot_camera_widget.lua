function widget:GetInfo()
	return {
		name = "BAR Metal Oracle Camera Lock",
		desc = "Locks camera to bar-metal commander lock pose for FLIP shots",
		author = "bar-metal",
		date = "2026",
		license = "GNU GPL, v2 or later",
		layer = -1000000,
		enabled = true,
		handler = true,
	}
end

local dist = 400
local tilt = 1.0
local yaw = math.pi
local dumped = false

local function commander()
	for _, id in ipairs(Spring.GetTeamUnits(0) or {}) do
		local def = UnitDefs[Spring.GetUnitDefID(id)]
		local name = def and def.name or ""
		if name == "armcom" or name == "corcom" or name == "legcom" then
			return id
		end
	end
	local u = Spring.GetTeamUnits(0) or {}
	return u[1]
end

local function lock()
	local id = commander()
	if not id then return end
	local x, y, z = Spring.GetUnitPosition(id)
	if not x then return end
	local tx, ty, tz = x, (y or 0) + 30, z
	local horizontal = math.sin(tilt) * dist
	local ex = tx - math.sin(yaw) * horizontal
	local ey = math.cos(tilt) * dist
	local ez = tz - math.cos(yaw) * horizontal
	local dx, dy, dz = tx - ex, ty - ey, tz - ez
	local len = math.sqrt(dx * dx + dy * dy + dz * dz)
	if len > 1e-6 then
		dx, dy, dz = dx / len, dy / len, dz / len
	end
	Spring.SetCameraState({
		name = "free",
		mode = 4,
		px = ex, py = ey, pz = ez,
		dx = dx, dy = dy, dz = dz,
		fov = 45,
		gndOffset = 0,
		gravity = 0,
		slide = 0,
		scrollSpeed = 0,
		tiltSpeed = 0,
		velTime = 0,
		avelTime = 0,
		vx = 0, vy = 0, vz = 0,
		avx = 0, avy = 0, avz = 0,
	}, 0)
end

function widget:Initialize()
	if widgetHandler and widgetHandler.DisableWidget then
		for _, n in ipairs({
			"CameraFlip", "Camera Shake", "Camera remember", "SmoothCam",
			"Keep camera focused on unit selection", "Unit follow cam",
			"COFC wrapper", "Combo Overhead/Free Camera (experimental)",
			"OverrideCamera",
		}) do
			pcall(widgetHandler.DisableWidget, widgetHandler, n)
		end
	end
	Spring.Log("oracle_shot", "info", "oracle_cam_widget: free mode=4")
end

function widget:Update()
	lock()
	local id = commander()
	if id then
		pcall(Spring.SelectUnitArray, {id})
	end
	if not dumped and (Spring.GetGameFrame() or 0) > 25 then
		local got = Spring.GetCameraState()
		if got then
			Spring.Log("oracle_shot", "info", string.format(
				"oracle_cam_widget: px=%.1f py=%.1f pz=%.1f dx=%.3f dy=%.3f dz=%.3f mode=%s",
				got.px or 0, got.py or 0, got.pz or 0, got.dx or 0, got.dy or 0, got.dz or 0, tostring(got.mode)
			))
			dumped = true
		end
	end
end

function widget:DrawScreen()
	lock()
end
