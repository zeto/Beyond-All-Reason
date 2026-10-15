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

local function trace_line(frame)
	local commanders, others = commander_ids()
	if #commanders == 0 then
		return nil
	end
	local parts = {}
	for _, id in ipairs(commanders) do
		parts[#parts + 1] = unit_object(id)
	end
	return string.format('{"f":%d,"units":[%s]}', frame, table.concat(parts, ","))
end

if gadgetHandler:IsSyncedCode() then
	local ordered = false
	local limit = 30
	local samples = {}

	local function emit(frame)
		local line = trace_line(frame)
		if line == nil then
			Spring.Echo("BARMETAL_WAIT frame " .. tostring(frame))
			return
		end
		local commanders = commander_ids()
		local x, y, z = Spring.GetUnitPosition(commanders[1])
		samples[#samples + 1] = string.format("%d %.4f %.4f %.4f", frame, x or 0, y or 0, z or 0)
		Spring.Echo("BARMETAL_TRACE " .. line)
		if frame == 0 or frame >= limit then
			Spring.Echo("BARMETAL_PATH " .. table.concat(samples, ";"))
		end
		SendToUnsynced("BarMetalTrace", frame, line)
	end

	function gadget:Initialize()
		local options = Spring.GetModOptions()
		limit = tonumber(options.barmetal_frames) or 30
		Spring.Echo("BARMETAL_LOADED synced")
	end

	function gadget:GameStart()
		Spring.Echo("BARMETAL_READY")
		emit(0)
	end

	function gadget:GameFrame(frame)
		local options = Spring.GetModOptions()
		local move = options.barmetal_move
		if not ordered and move and move ~= "" then
			local at, x, z = move:match("^(%d+):([%d%.%-]+):([%d%.%-]+)$")
			at = tonumber(at)
			x = tonumber(x)
			z = tonumber(z)
			if at and x and z and frame >= at then
				local commanders = commander_ids()
				if commanders[1] then
					Spring.GiveOrderToUnit(commanders[1], CMD.MOVE, { x, 0, z }, 0)
					ordered = true
					Spring.Echo(string.format("BARMETAL_ORDER %d %s %s", frame, num(x), num(z)))
				end
			end
		end
		if frame % 30 == 0 then
			emit(frame)
		end
	end
else
	local quit = false

	function gadget:Initialize()
		Spring.Echo("BARMETAL_LOADED unsynced")
		Spring.SendCommands("setmaxspeed 5", "setspeed 5")
		local can_write = io ~= nil and io.open ~= nil
		if can_write then
			local sink, err = io.open("/tmp/bar-oracle/trace.jsonl", "w")
			if sink then
				sink:close()
			end
			Spring.Echo("BARMETAL_OPEN " .. tostring(sink ~= nil) .. " " .. tostring(err))
		end
		Spring.Echo("BARMETAL_IO " .. tostring(can_write))
		gadgetHandler:AddSyncAction("BarMetalTrace", function(_, frame, line)
			if can_write and line then
				local out = io.open("/tmp/bar-oracle/trace.jsonl", "a")
				if out then
					out:write(line, "\n")
					out:flush()
					out:close()
				end
			end
			local limit = tonumber(Spring.GetModOptions().barmetal_frames) or 30
			if not quit and frame >= limit then
				quit = true
				Spring.Echo("BARMETAL_QUIT " .. tostring(frame))
				Spring.SendCommands("quitforce")
			end
		end)
	end
end
