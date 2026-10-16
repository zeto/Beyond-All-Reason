function gadget:GetInfo()
	return {
		name = "BAR Metal Oracle Trace",
		desc = "Echo unit samples from the pinned Recoil engine",
		author = "bar-metal",
		date = "2026",
		license = "GNU GPL, v2 or later",
		layer = 100,
		enabled = true,
	}
end

local function num(value)
	return string.format("%.6g", value)
end

local function commander_ids()
	local commanders = {}
	local others = {}
	for _, id in ipairs(Spring.GetAllUnits()) do
		local def = UnitDefs[Spring.GetUnitDefID(id)]
		local name = def and def.name or ""
		if name == "armcom" or name == "corcom" or name == "legcom" then
			commanders[#commanders + 1] = id
		else
			others[#others + 1] = id
		end
	end
	table.sort(commanders)
	table.sort(others)
	return commanders, others
end

local function unit_object(id)
	local def = UnitDefs[Spring.GetUnitDefID(id)]
	local name = def and def.name or "unknown"
	local x, y, z = Spring.GetUnitPosition(id)
	local team = Spring.GetUnitTeam(id) or 0
	return string.format(
		'{"id":%d,"def":"%s","t":%d,"x":%s,"y":%s,"z":%s}',
		id,
		name,
		team,
		num(x or 0),
		num(y or 0),
		num(z or 0)
	)
end

if gadgetHandler:IsSyncedCode() then
	local ordered = false
	local built = false
	local damaged = false
	local limit = 30
	local scenario = "boot"
	local move_spec = ""
	local build_spec = ""
	local damage_spec = ""
	local attack_spec = ""
	local shift_spec = ""
	local pause_spec = ""
	local speed_spec = ""
	local reclaim_spec = ""
	local repair_spec = ""
	local facing_spec = ""
	local spawn_spec = ""
	local attacked = false
	local shifted = false
	local paused_done = false
	local speed_done = false
	local reclaimed = false
	local repaired = false
	local faced = false
	local spawned = false
	local last_spawned = nil
	local samples = {}
	local json_lines = {}
	local last_sel = {}
	local last_cmd = 0
	local last_params = {}
	local last_build = nil
	local last_health = nil
	local last_max_health = nil
	local last_range = nil

	local function parse_move(spec)
		if not spec or spec == "" then
			return nil
		end
		local at, x, z = spec:match("^(%d+):([%d%.%-]+):([%d%.%-]+)$")
		at, x, z = tonumber(at), tonumber(x), tonumber(z)
		if at and x and z then
			return at, x, z
		end
		return nil
	end

	local function parse_build(spec)
		if not spec or spec == "" then
			return nil
		end
		local at, def, x, z = spec:match("^(%d+):([%w_]+):([%d%.%-]+):([%d%.%-]+)$")
		at, x, z = tonumber(at), tonumber(x), tonumber(z)
		if at and def and x and z then
			return at, def, x, z
		end
		return nil
	end

	local function parse_damage(spec)
		if not spec or spec == "" then
			return nil
		end
		local at, frac = spec:match("^(%d+):([%d%.%-]+)$")
		at, frac = tonumber(at), tonumber(frac)
		if at and frac then
			return at, frac
		end
		return nil
	end

	local function encode_array(vals)
		local parts = {}
		for i, v in ipairs(vals) do
			if type(v) == "number" then
				if v == math.floor(v) then
					parts[i] = string.format("%d", v)
				else
					parts[i] = num(v)
				end
			else
				parts[i] = tostring(v)
			end
		end
		return "[" .. table.concat(parts, ",") .. "]"
	end

	local function current_cmd(id)
		local queue = Spring.GetUnitCommands(id, 1)
		if queue and queue[1] then
			local c = queue[1]
			local params = {}
			if c.params then
				for i, p in ipairs(c.params) do
					params[i] = p
				end
			end
			return c.id, params
		end
		return 0, {}
	end

	local function nano_near(bx, bz)
		local best, best_d = nil, 1e9
		for _, id in ipairs(Spring.GetAllUnits()) do
			local def = UnitDefs[Spring.GetUnitDefID(id)]
			local name = def and def.name or ""
			local _, _, _, _, buildProgress = Spring.GetUnitHealth(id)
			if name == "armmex" and buildProgress and buildProgress < 1 then
				local x, _, z = Spring.GetUnitPosition(id)
				local d = (x - bx) * (x - bx) + (z - bz) * (z - bz)
				if d < best_d then
					best_d = d
					best = id
				end
			end
		end
		return best
	end

	local function emit(frame)
		local commanders = commander_ids()
		if #commanders == 0 then
			Spring.Echo("BARMETAL_WAIT frame " .. tostring(frame))
			return
		end
		local id = commanders[1]
		local parts = {}
		for _, cid in ipairs(commanders) do
			parts[#parts + 1] = unit_object(cid)
		end
		local line = string.format('{"f":%d,"units":[%s]', frame, table.concat(parts, ","))
		if #last_sel > 0 then
			line = line .. ',"sel":' .. encode_array(last_sel)
		end
		if last_cmd and last_cmd ~= 0 then
			line = line .. ',"cmd":' .. tostring(last_cmd)
			if #last_params > 0 then
				line = line .. ',"params":' .. encode_array(last_params)
			end
		end
		if last_build then
			line = line
				.. string.format(
					',"build":{"def":"%s","x":%s,"z":%s,"builder":%d,"building":%s,"cmd":%d}',
					last_build.def,
					num(last_build.x),
					num(last_build.z),
					last_build.builder,
					last_build.building and tostring(last_build.building) or "null",
					last_build.cmd or 0
				)
		end
		if faced then
			line = line .. ',"facing":' .. tostring((tonumber(facing_spec and facing_spec:match(":(%d+)$")) or 0))
		end
		if last_health ~= nil then
			line = line
				.. string.format(
					',"health":%s,"maxHealth":%s,"range":%s',
					num(last_health),
					num(last_max_health or 0),
					num(last_range or 0)
				)
		end
		line = line .. "}"
		local x, y, z = Spring.GetUnitPosition(id)
		samples[#samples + 1] = string.format("%d %.4f %.4f %.4f", frame, x or 0, y or 0, z or 0)
		json_lines[#json_lines + 1] = line
		Spring.Echo("BARMETAL_TRACE " .. line)
		if frame == 0 or frame >= limit then
			Spring.Echo("BARMETAL_PATH " .. table.concat(samples, ";"))
		end
		SendToUnsynced("BarMetalTrace", frame, line)
	end

	function gadget:Initialize()
		local options = Spring.GetModOptions()
		limit = tonumber(options.barmetal_frames) or 30
		scenario = options.barmetal_scenario or "boot"
		move_spec = options.barmetal_move or ""
		build_spec = options.barmetal_build or ""
		damage_spec = options.barmetal_damage or ""
		attack_spec = options.barmetal_attack or ""
		shift_spec = options.barmetal_shiftmove or ""
		pause_spec = options.barmetal_pause or ""
		speed_spec = options.barmetal_gamespeed or ""
		reclaim_spec = options.barmetal_reclaim or ""
		repair_spec = options.barmetal_repair or ""
		facing_spec = options.barmetal_facing or ""
		spawn_spec = options.barmetal_spawn or ""
		-- Legacy move-only runs (PIN move_kbot) keep scenario bootish.
		if (scenario == "" or scenario == "boot") and move_spec ~= "" then
			scenario = "move"
		end
		Spring.Echo("BARMETAL_LOADED synced scenario " .. tostring(scenario))
	end

	function gadget:GameStart()
		Spring.Echo("BARMETAL_READY")
		local commanders = commander_ids()
		if commanders[1] then
			last_sel = { commanders[1] }
		end
		emit(0)
	end

	function gadget:GameFrame(frame)
		local commanders = commander_ids()
		local id = commanders[1]
		if not id then
			return
		end

		-- Selection is UI-side in Recoil; oracle marks the ordered commander as sel.
		last_sel = { id }

		local at, mx, mz = parse_move(move_spec)
		if not ordered and at and frame >= at then
			Spring.GiveOrderToUnit(id, CMD.MOVE, { mx, 0, mz }, 0)
			ordered = true
			last_cmd = CMD.MOVE
			last_params = { mx, 0, mz }
			Spring.Echo(string.format("BARMETAL_ORDER %d %s %s", frame, num(mx), num(mz)))
		end

		local bat, bdef, bx, bz = parse_build(build_spec)
		if not built and bat and frame >= bat then
			local udef = UnitDefNames[bdef]
			if udef then
				local gy = Spring.GetGroundHeight(bx, bz)
				local cmd = -udef.id
				Spring.GiveOrderToUnit(id, cmd, { bx, gy, bz }, 0)
				built = true
				last_cmd = cmd
				last_params = { bx, gy, bz }
				last_build = {
					def = bdef,
					x = bx,
					z = bz,
					builder = id,
					building = nil,
					cmd = cmd,
				}
				Spring.Echo(string.format("BARMETAL_BUILD %d %s %s %s cmd %d", frame, bdef, num(bx), num(bz), cmd))
			else
				Spring.Echo("BARMETAL_BUILD_FAIL unknown def " .. tostring(bdef))
			end
		end

		if last_build and not last_build.building then
			local nano = nano_near(last_build.x, last_build.z)
			if nano then
				last_build.building = nano
			end
		end

		local dat, frac = parse_damage(damage_spec)
		if not damaged and dat and frame >= dat then
			local health, maxHealth = Spring.GetUnitHealth(id)
			maxHealth = maxHealth or health or 1
			local target = maxHealth * frac
			Spring.SetUnitHealth(id, target)
			damaged = true
			last_health = target
			last_max_health = maxHealth
			local def = UnitDefs[Spring.GetUnitDefID(id)]
			last_range = (def and (def.maxWeaponRange or def.maxRange)) or Spring.GetUnitMaxRange(id) or 0
			Spring.Echo(string.format("BARMETAL_DAMAGE %d %s/%s range %s", frame, num(target), num(maxHealth), num(last_range)))
			emit(frame)
		end

		-- Refresh health/range every sample when damage scenario is active or already damaged.
		if damaged or (damage_spec and damage_spec ~= "") then
			local health, maxHealth = Spring.GetUnitHealth(id)
			last_health = health
			last_max_health = maxHealth
			local def = UnitDefs[Spring.GetUnitDefID(id)]
			last_range = (def and (def.maxWeaponRange or def.maxRange)) or Spring.GetUnitMaxRange(id) or 0
		end

		-- Keep cmd/params live from the unit queue when present.
		local qcmd, qparams = current_cmd(id)
		if qcmd and qcmd ~= 0 then
			last_cmd = qcmd
			last_params = qparams
		end


		-- Spawn helper: frame:def:x:z:team
		if not spawned and spawn_spec and spawn_spec ~= "" then
			local at, def, x, z, team = spawn_spec:match("^(%d+):([%w_]+):([%d%.%-]+):([%d%.%-]+):(%d+)$")
			at, x, z, team = tonumber(at), tonumber(x), tonumber(z), tonumber(team)
			if at and def and frame >= at then
				local gy = Spring.GetGroundHeight(x, z)
				local uid = Spring.CreateUnit(def, x, gy, z, 0, team or 1)
				spawned = true
				last_spawned = uid
				Spring.Echo(string.format("BARMETAL_SPAWN %d %s id %s", frame, def, tostring(uid)))
			end
		end

		-- Shift-queue second move: frame:x:z (issued with shift after primary move)
		if not shifted and shift_spec and shift_spec ~= "" then
			local at, x, z = shift_spec:match("^(%d+):([%d%.%-]+):([%d%.%-]+)$")
			at, x, z = tonumber(at), tonumber(x), tonumber(z)
			if at and x and z and frame >= at then
				Spring.GiveOrderToUnit(id, CMD.MOVE, { x, 0, z }, { "shift" })
				shifted = true
				last_cmd = CMD.MOVE
				last_params = { x, 0, z }
				Spring.Echo(string.format("BARMETAL_SHIFTMOVE %d %s %s", frame, num(x), num(z)))
			end
		end

		-- Attack: frame:targetId
		if not attacked and attack_spec and attack_spec ~= "" then
			local at, tid = attack_spec:match("^(%d+):(%d+)$")
			at, tid = tonumber(at), tonumber(tid)
			if at and frame >= at then
				if (not tid or tid == 0) and last_spawned then
					tid = last_spawned
				end
				if tid then
					Spring.GiveOrderToUnit(id, CMD.ATTACK, { tid }, 0)
					attacked = true
					last_cmd = CMD.ATTACK
					last_params = { tid }
					Spring.Echo(string.format("BARMETAL_ATTACK %d -> %d", frame, tid))
				end
			end
		end

		-- Facing: frame:facing
		if not faced and facing_spec and facing_spec ~= "" then
			local at, fac = facing_spec:match("^(%d+):(%d+)$")
			at, fac = tonumber(at), tonumber(fac)
			if at and fac and frame >= at then
				faced = true
				Spring.Echo(string.format("BARMETAL_FACING %d %d", frame, fac))
				-- Record on the next emit line via last_params convention
				last_cmd = last_cmd ~= 0 and last_cmd or -1
			end
		end

		-- Pause: frame (toggle pause via game rules echo; unsynced mirrors)
		if not paused_done and pause_spec and pause_spec ~= "" then
			local at = tonumber(pause_spec)
			if at and frame >= at then
				paused_done = true
				SendToUnsynced("BarMetalPause", 1)
				Spring.Echo("BARMETAL_PAUSE " .. tostring(frame))
			end
		end

		-- Gamespeed: frame:speed
		if not speed_done and speed_spec and speed_spec ~= "" then
			local at, spd = speed_spec:match("^(%d+):(%d+)$")
			at, spd = tonumber(at), tonumber(spd)
			if at and spd and frame >= at then
				speed_done = true
				SendToUnsynced("BarMetalSpeed", spd)
				Spring.Echo(string.format("BARMETAL_SPEED %d %d", frame, spd))
			end
		end

		-- Reclaim: frame:x:z
		if not reclaimed and reclaim_spec and reclaim_spec ~= "" then
			local at, x, z = reclaim_spec:match("^(%d+):([%d%.%-]+):([%d%.%-]+)$")
			at, x, z = tonumber(at), tonumber(x), tonumber(z)
			if at and x and z and frame >= at then
				local gy = Spring.GetGroundHeight(x, z)
				Spring.GiveOrderToUnit(id, CMD.RECLAIM, { x, gy, z }, 0)
				reclaimed = true
				last_cmd = CMD.RECLAIM
				last_params = { x, gy, z }
				Spring.Echo(string.format("BARMETAL_RECLAIM %d %s %s", frame, num(x), num(z)))
			end
		end

		-- Repair: frame:targetId
		if not repaired and repair_spec and repair_spec ~= "" then
			local at, tid = repair_spec:match("^(%d+):(%d+)$")
			at, tid = tonumber(at), tonumber(tid)
			if at and frame >= at then
				if (not tid or tid == 0) and last_spawned then
					tid = last_spawned
				end
				if tid then
					Spring.GiveOrderToUnit(id, CMD.REPAIR, { tid }, 0)
					repaired = true
					last_cmd = CMD.REPAIR
					last_params = { tid }
					Spring.Echo(string.format("BARMETAL_REPAIR %d -> %d", frame, tid))
				end
			end
		end

		if frame % 30 == 0 then
			emit(frame)
		end
		if frame == limit + 1 then
			-- Dump every buffered line once more so a rate-limited infolog still
			-- cannot erase the file-backed capture; unsynced also persists them.
			for i, buffered in ipairs(json_lines) do
				SendToUnsynced("BarMetalTrace", limit, buffered)
			end
			Spring.Echo("BARMETAL_LINES " .. tostring(#json_lines))
			Spring.Echo("BARMETAL_QUIT " .. tostring(frame))
			Spring.SendCommands("quitforce")
		end
	end
else
	local quit = false

	local function open_trace(mode)
		if io == nil or io.open == nil then
			return nil, "no-io"
		end
		local candidates = {
			"trace.jsonl",
			"./trace.jsonl",
			"/tmp/bar-oracle/trace.jsonl",
		}
		if Spring.GetConfigString then
			-- no-op; keep candidates above
		end
		local err
		for _, path in ipairs(candidates) do
			local f, e = io.open(path, mode)
			if f then
				return f, path
			end
			err = tostring(e) .. " @ " .. path
		end
		return nil, err
	end

	function gadget:Initialize()
		Spring.Echo("BARMETAL_LOADED unsynced")
		Spring.SendCommands("setmaxspeed 5", "setspeed 5")
		local sink, info = open_trace("w")
		if sink then
			sink:close()
			Spring.Echo("BARMETAL_OPEN true " .. tostring(info))
		else
			Spring.Echo("BARMETAL_OPEN false " .. tostring(info))
		end
		local seen = {}
		gadgetHandler:AddSyncAction("BarMetalTrace", function(_, frame, line)
			if line and not seen[line] then
				seen[line] = true
				local out, path = open_trace("a")
				if out then
					out:write(line, "\n")
					out:flush()
					out:close()
					if frame == 0 then
						Spring.Echo("BARMETAL_WRITE " .. tostring(path))
					end
				end
			end
		end)
	end
end
