local uci = require "luci.model.uci"
local sys = require "luci.sys"
local dbg = require "luci.tools.debug"
local uci_r = uci.cursor()

local MODULE_UCI = "qos"
local STYPE_RULE = "rule"
local state_file = "/tmp/ip_qos/.insert_index"

local insert_index_fp = io.open(state_file, "w+")

function find_insert_index(insert_rule)
    local position = 0
    local found = false
    uci_r:foreach(MODULE_UCI, STYPE_RULE,
        function(section)
            if found == true then
                return
            end
            if section.name == insert_rule then
                position = position + 1
                found = true
                return
            end

            if section.enable == "on" then
                if (tonumber(section.rate_max) ~= 0) or (tonumber(section.rate_min) ~= 0) then
                    position = position + 1
                end
                if (tonumber(section.rate_max_mate) ~= 0) or (tonumber(section.rate_min_mate) ~= 0) then
                    position = position + 1
                end
            end
        end
        )
    --dbg(position)
    insert_index_fp:write(position)
    insert_index_fp:close()
end

if arg[1] then
    find_insert_index(arg[1])
end