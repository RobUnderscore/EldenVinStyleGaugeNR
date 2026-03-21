-- STYLE RANK MONITOR
-- Paste this into CE Lua Engine (Table -> Show Cheat Table Lua Script)
-- It will continuously print your style rank every 200ms
-- Press Execute to start, close the Lua window to stop

if StyleRankRunning then
  StyleRankRunning = false
  print("Stopped.")
  return
end

StyleRankRunning = true
print("Style Rank Monitor started! Execute again to stop.")
print("Rank: 0=None 1=D 2=C 3=B 4=A 5=S 6=SS 7=SSS 8=V")
print("")

local RANK_NAMES = {"D", "C", "B", "A", "S", "SS", "SSS", "V"}
local last_rank = -1

createThread(function(thread)
  while StyleRankRunning do
    local ok, err = pcall(function()
      local wc = readQword(getAddress("WorldChrMan"))
      if not wc or wc == 0 then return end
      local ChrIns = readQword(wc + 0x174e8)
      if not ChrIns or ChrIns == 0 then return end
      local ctrl = readQword(ChrIns + 0x190)
      if not ctrl or ctrl == 0 then return end
      local head = readQword(ctrl + 0x8)
      if not head or head == 0 then return end

      local node = head
      local count = 0
      local style_count = 0
      local min_dur = 999
      local max_dur = 0

      repeat
        local id = readInteger(node + 0x08)
        if id and id ~= 0 then
          if id >= 999320410 and id <= 999320429 then
            style_count = style_count + 1
            local dur = readFloat(node + 0x48)
            if dur and dur >= 0 then
              if dur > max_dur then max_dur = dur end
              if dur < min_dur then min_dur = dur end
            end
          end
        end
        count = count + 1
        local nxt = readQword(node + 0x38)
        if nxt and nxt ~= 0 and nxt ~= head and nxt ~= node then
          node = nxt
        else
          node = nil
        end
      until not node or count > 500

      local rank = style_count
      if rank > 8 then rank = 8 end

      if rank ~= last_rank then
        local name = rank > 0 and RANK_NAMES[rank] or "None"
        local dur_str = ""
        if rank > 0 then
          dur_str = string.format(" (min=%.1fs max=%.1fs)", min_dur, max_dur)
        end
        print(string.format("[%s] Rank: %s (%d effects)%s", os.date("%H:%M:%S"), name, style_count, dur_str))
        last_rank = rank
      end
    end)
    sleep(200)
  end
  print("Monitor stopped.")
end)
