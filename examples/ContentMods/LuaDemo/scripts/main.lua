-- LuaDemo: the smallest useful Lua mod (docs/sdk/LUA.md). The player's Jab (attack row 004) deals 3x damage; every result is logged.
-- Also shows the event bus between scripts (abs.emit / abs.on), a timer and the per-mod key/value store.
local SCALE = 3

abs.log("luademo: v1 loaded, " .. abs.lua_version)
if io or os or load or loadfile or dofile or require then abs.log("luademo: FAIL sandbox: a blocked library is visible")
else abs.log("luademo: PASS sandbox") end

abs.on("hit_pre", function(h)                       -- before the game resolves an impact: change the scales
  if h.attack == "004" and h.attacker == abs.player() then h.damage_scale = h.damage_scale * SCALE end
end)

abs.on("hit", function(h)                           -- after: what really happened
  if h.attack == "004" and h.attacker == abs.player() then
    abs.log(string.format("luademo: v1 hit %s outcome %s damage %.1f", h.attack, h.outcome, h.damage))
    if h.outcome == "hit" and h.damage >= 100 then abs.log("luademo: PASS v1 scaled damage") end
  end
end)

abs.on("ping", function(v) abs.log("luademo: PASS bus ping " .. tostring(v)) end)
abs.timer(2, false, function() abs.emit("ping", 42) end)

local boots = (tonumber(abs.kv_get("loads")) or 0) + 1
abs.kv_set("loads", tostring(boots))
abs.log("luademo: loaded " .. boots .. " time(s) so far")

-- a key (P4 keybind registry): players can move it in Mods\LuaHost\keybinds.txt ("LuaDemo.toggle = J"); conflicts are listed in keybinds_report.txt
local key = abs.bind("toggle", "K", function() abs.log("luademo: PASS key toggle pressed") end, "Log a line")
abs.log("luademo: toggle is on key " .. key)
