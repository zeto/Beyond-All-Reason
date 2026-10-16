function gadget:GetInfo()
	return {
		name = "BAR Metal Oracle Shot",
		desc = "Headed HUD bookmark screenshots for M7 FLIP",
		author = "bar-metal",
		date = "2026",
		license = "GNU GPL, v2 or later",
		layer = 100,
		enabled = true,
	}
end

if gadgetHandler:IsSyncedCode() then
	local limit = tonumber(Spring.GetModOptions().barmetal_frames) or 120
	local damage_frac = tonumber(Spring.GetModOptions().barmetal_damage) or 0
	local damaged = false

	local function commander()
		for _, id in ipairs(Spring.GetTeamUnits(0) or {}) do
			local def = UnitDefs[Spring.GetUnitDefID(id)]
			local name = def and def.name or ""
			if name == "armcom" or name == "corcom" or name == "legcom" then
				return id
			end
		end
		local units = Spring.GetTeamUnits(0) or {}
		return units[1]
	end

	function gadget:GameFrame(f)
		if damage_frac > 0 and damage_frac < 1 and not damaged and f >= 35 then
			local id = commander()
			if id then
				local hp, maxhp = Spring.GetUnitHealth(id)
				if hp and maxhp and maxhp > 0 then
					Spring.SetUnitHealth(id, maxhp * damage_frac)
					damaged = true
					Spring.Log("oracle_shot", "info", "oracle_shot: damage " .. damage_frac)
				end
			end
		end
		if f >= limit then
			Spring.Log("oracle_shot", "info", "oracle_shot: quit at frame " .. f)
			Spring.GameOver({})
		end
	end
else
	local shot_name = Spring.GetModOptions().barmetal_shot or "select"
	local build_def = Spring.GetModOptions().barmetal_builddef or ""
	local do_select = (Spring.GetModOptions().barmetal_select or "1") ~= "0"
	local shot_frame = tonumber(Spring.GetModOptions().barmetal_shotframe) or 60
	local shot_done = false

	local function commander()
		for _, id in ipairs(Spring.GetTeamUnits(0) or {}) do
			local def = UnitDefs[Spring.GetUnitDefID(id)]
			local name = def and def.name or ""
			if name == "armcom" or name == "corcom" or name == "legcom" then
				return id
			end
		end
		local units = Spring.GetTeamUnits(0) or {}
		return units[1]
	end

	local function apply_hud(id)
		if do_select and id then
			pcall(Spring.SelectUnitArray, {id})
		end
		if build_def ~= "" then
			local udef = UnitDefNames and UnitDefNames[build_def]
			if udef then
				pcall(Spring.SetActiveCommand, "buildunit_" .. build_def)
				pcall(Spring.SetActiveCommand, -udef.id)
			end
		end
	end

	function gadget:Update()
		local f = Spring.GetGameFrame() or 0
		local id = commander()
		if id then
			apply_hud(id)
		end
		if f < shot_frame or shot_done then
			return
		end
		shot_done = true
		local path = "screenshots/" .. shot_name .. ".png"
		local vsx, vsy = Spring.GetViewGeometry()
		local got = Spring.GetCameraState()
		if got then
			Spring.Log("oracle_shot", "info", string.format(
				"oracle_shot: gotcam px=%.1f py=%.1f pz=%.1f dx=%.3f dy=%.3f dz=%.3f mode=%s %dx%d",
				got.px or 0, got.py or 0, got.pz or 0, got.dx or 0, got.dy or 0, got.dz or 0,
				tostring(got.mode), vsx or 0, vsy or 0
			))
		end
		local ok, err = pcall(gl.SaveImage, 0, 0, vsx, vsy, path)
		Spring.Log("oracle_shot", "info", "oracle_shot: SaveImage " .. path .. " " .. tostring(ok) .. " " .. tostring(err))
		pcall(Spring.SendCommands, "screenshot")
	end
end
