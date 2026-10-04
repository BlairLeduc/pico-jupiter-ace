-- ace-dump.lua — MAME's jupace with a .ace loaded, dumped for
-- tools/ace-reference.sh (design.md §13.4, M11).
--
-- MAME loads the snapshot a second after power-on. This waits
-- ACE_DUMP_FIELDS frames from then, so both machines have run the same
-- number of fields past the load, then writes the CPU's registers and
-- every RAM byte the Ace can read back, $2400-$27FF and $3C00-$7FFF, as
-- hex lines to ACE_DUMP_OUT, and exits.

local out_path = os.getenv("ACE_DUMP_OUT")
local fields = tonumber(os.getenv("ACE_DUMP_FIELDS") or "100")
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local frame = 0
-- The snapshot device's delay is 1.0 s (jupace.cpp, set_delay): 50.08
-- fields at 6.5 MHz / (416 x 312). The load lands within field 51.
local load_frame = 51

local function hexrange(f, first, last)
    for a = first, last, 32 do
        local t = {}
        for i = 0, 31 do t[#t + 1] = string.format("%02X", mem:read_u8(a + i)) end
        f:write(string.format("%04X %s\n", a, table.concat(t)))
    end
end

emu.register_frame_done(function()
    frame = frame + 1
    if frame < load_frame + fields then return end
    local f = assert(io.open(out_path, "w"))
    local s = cpu.state
    for _, r in ipairs({ "AF", "BC", "DE", "HL", "IX", "IY", "SP", "PC",
                         "AF2", "BC2", "DE2", "HL2", "I", "IM", "IFF1", "IFF2" }) do
        f:write(string.format("%s %04X\n", r, s[r].value))
    end
    hexrange(f, 0x2400, 0x27FF)
    hexrange(f, 0x3C00, 0x7FFF)
    f:close()
    manager.machine:exit()
end)
