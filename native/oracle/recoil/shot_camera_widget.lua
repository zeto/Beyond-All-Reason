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
local last_logged = -1

-- Camera checks go to <write-dir>/oracle_cam.log, not the console, so the log
-- text never lands in the reference screenshot.
local function camlog(s)
	local fh = io and io.open and io.open("oracle_cam.log", "a")
	if fh then
		fh:write(s, "\n")
		fh:close()
	else
		Spring.Log("oracle_shot", LOG.INFO, s)
	end
end

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

-- barmetal_cam "x,y,z,distance,tilt,yaw": a fixed bookmark camera (bar-metal --fixed-camera),
-- posed like native/map/camera.cpp camera_pose (eye height cos(tilt)*distance above y=0).
local function fixed_cam()
	local spec = (Spring.GetModOptions() or {}).barmetal_cam or ""
	if spec == "" then return nil end
	local v = {}
	for n in spec:gmatch("[^,]+") do v[#v + 1] = tonumber(n) end
	if #v ~= 6 then return nil end
	local tx, ty, tz, dist, tilt, yaw = v[1], v[2], v[3], v[4], v[5], v[6]
	local horizontal = math.sin(tilt) * dist
	return {tx = tx, ty = ty, tz = tz,
		ex = tx - math.sin(yaw) * horizontal, ey = math.cos(tilt) * dist, ez = tz - math.cos(yaw) * horizontal}
end

local function lock()
	local fixed = fixed_cam()
	local id = commander()
	if not id and not fixed then return end
	local p = fixed or want(id)
	if not p then return end

	-- camera.cpp eye.y is absolute (cos(tilt)*dist + lift), not target-relative, while
	-- Spring puts the eye at focus - dir*dist. Solve dist/rx from the camera.cpp
	-- eye->target vector so the Recoil eye lands exactly on the bar-metal eye.
	local vx, vy, vz = p.ex - p.tx, p.ey - p.ty, p.ez - p.tz
	local horiz = math.sqrt(vx * vx + vz * vz)
	local dist = math.sqrt(horiz * horiz + vy * vy)
	local rx = math.pi - math.atan2(horiz, vy)
	p.rx = rx
	-- Spring cam pins its focus to the ground (py came back 57.7, not ty), so the eye
	-- drifts by the 30-elmo target offset. The FPS controller takes the eye position
	-- and rotation directly, so it can hold camera.cpp's eye and look-at exactly.
	local len = dist
	local fx, fy, fz = -vx / len, -vy / len, -vz / len
	-- CCamera::GetFwdFromRot: fwd = (sin rx sin ry, cos rx, -sin rx cos ry). The commander lock
	-- (yaw pi) gives ry 0; a fixed bookmark needs its own heading or FPS faces north.
	local ry = fixed and math.atan2(fx, -fz) or SPRING_RY
	Spring.SetCameraState({
		name = "fps",
		mode = 0,
		px = p.ex,
		py = p.ey,
		pz = p.ez,
		dx = fx,
		dy = fy,
		dz = fz,
		rx = rx,
		ry = ry,
		rz = 0,
		oldHeight = p.ey,
		fov = 45,
	}, 0)
	p.fx, p.fy, p.fz = fx, fy, fz

	if not fixed then
		pcall(Spring.SelectUnitArray, {id})
	end

	local f = Spring.GetGameFrame() or 0
	if last_logged < 0 and f >= 20 then
		last_logged = f
		local cx, cy, cz = Spring.GetCameraPosition()
		local st = Spring.GetCameraState() or {}
		local gx, gy, gz = Spring.GetCameraDirection()
		local err = cx and math.sqrt((cx - p.ex) ^ 2 + (cy - p.ey) ^ 2 + (cz - p.ez) ^ 2) or 1e9
		camlog(string.format(
			"oracle_cam f=%d err=%.2f want=%.1f,%.1f,%.1f got=%.1f,%.1f,%.1f req dist=%.1f rx=%.4f | state mode=%s name=%s dist=%.1f rx=%.4f ry=%.4f p=%.1f,%.1f,%.1f",
			f, err, p.ex, p.ey, p.ez, cx or -1, cy or -1, cz or -1, dist, rx,
			tostring(st.mode), tostring(st.name), st.dist or -1, st.rx or -1, st.ry or -1,
			st.px or -1, st.py or -1, st.pz or -1))
		camlog(string.format("oracle_cam_dir f=%d want=%.4f,%.4f,%.4f got=%.4f,%.4f,%.4f",
			f, p.fx, p.fy, p.fz, gx or 0, gy or 0, gz or 0))
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
	camlog(string.format("oracle_cam_widget: spring rx=%.3f ry=%.3f dist=%.0f", SPRING_RX, SPRING_RY, DIST))
end

function widget:Update()
	lock()
end

function widget:DrawWorld()
	lock()
end
