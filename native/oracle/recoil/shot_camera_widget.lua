function widget:GetInfo()
	return {
		name = "BAR Metal Oracle Camera Lock",
		desc = "Locks camera to native/map/camera.cpp commander lock every frame",
		author = "bar-metal",
		date = "2026",
		license = "GNU GPL, v2 or later",
		layer = -1000000,
		enabled = true,
		handler = true,
	}
end

-- bar-metal lock-camera (native/map/camera.cpp):
--   tilt from vertical; yaw; eye = target - (sin yaw, ...) * sin(tilt)*dist
--   eye.y = cos(tilt)*dist
-- Spring spring-cam (SpringController.cpp):
--   rot.x clamped to [0.51π, 0.99π]; π≈straight down, 0.51π≈near-horizon
--   eye = focus - dir(rot)*dist
-- Map: spring_rx = π - tilt  (tilt 1 → rx ≈ 2.1416, inside clamp)

local DIST = 400
local TILT = 1.0
local YAW = math.pi
local SPRING_RX = math.pi - TILT
-- Azimuth: bar-metal yaw=π places eye at +Z (south). Spring rot.y=0 looks along -Z
-- from focus (dir forward); eye = focus - dir*dist → eye is behind forward = +Z if
-- forward is -Z. So ry=0 matches south eye when forward is north.
local SPRING_RY = 0
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

local function want(id)
	local x, y, z = Spring.GetUnitPosition(id)
	if not x then return nil end
	local tx, ty, tz = x, (y or 0) + 30, z
	local eye_lift = ((y or 0) > 66) and ((y or 0) - 66) or 0
	local horizontal = math.sin(TILT) * DIST
	local ex = tx - math.sin(YAW) * horizontal
	local ey = math.cos(TILT) * DIST + eye_lift
	local ez = tz - math.cos(YAW) * horizontal
	return {tx = tx, ty = ty, tz = tz, ex = ex, ey = ey, ez = ez}
end

local function lock()
	local id = commander()
	if not id then return end
	local p = want(id)
	if not p then return end

	Spring.SetCameraState({
		name = "spring",
		mode = 2,
		px = p.tx,
		py = p.ty,
		pz = p.tz,
		dist = DIST,
		rx = SPRING_RX,
		ry = SPRING_RY,
		rz = 0,
		fov = 45,
	}, 0)

	pcall(Spring.SelectUnitArray, {id})

	if not dumped and (Spring.GetGameFrame() or 0) > 40 then
		local cx, cy, cz = Spring.GetCameraPosition()
		local dx, dy, dz = Spring.GetCameraDirection()
		local err = 1e9
		if cx then
			err = math.sqrt((cx - p.ex) ^ 2 + (cy - p.ey) ^ 2 + (cz - p.ez) ^ 2)
		end
		Spring.Log("oracle_shot", "info", string.format(
			"oracle_cam err=%.1f want=%.0f,%.0f,%.0f got=%.0f,%.0f,%.0f dir=%.2f,%.2f,%.2f rx=%.3f ry=%.3f",
			err, p.ex, p.ey, p.ez, cx or -1, cy or -1, cz or -1,
			dx or 0, dy or 0, dz or 0, SPRING_RX, SPRING_RY
		))
		dumped = true
	end
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
	Spring.Log("oracle_shot", "info", string.format("oracle_cam_widget: spring rx=%.3f ry=%.3f dist=%.0f", SPRING_RX, SPRING_RY, DIST))
end

function widget:Update()
	lock()
end

function widget:DrawWorld()
	lock()
end
