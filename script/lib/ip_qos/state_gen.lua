local uci = require "luci.model.uci"
local sys = require "luci.sys"
local dbg = require "luci.tools.debug"
local uci_r = uci.cursor()

local MODULE_UCI = "qos"
local STYPE_RULE = "rule"
local state_file = "/tmp/ip_qos/state.data"

local state_fp = io.open (state_file, "a+")

function state_add_info()
    uci_r:foreach(MODULE_UCI, STYPE_RULE,
        function(section)
            state_fp:write(section.name..".enable="..section.enable.."\n")
            state_fp:write(section.name..".if_ping="..section.if_ping.."\n")
            state_fp:write(section.name..".if_pong="..section.if_pong.."\n")
            state_fp:write(section.name..".rate_max="..section.rate_max.."\n")
            state_fp:write(section.name..".rate_max_mate="..section.rate_max_mate.."\n")
            state_fp:write(section.name..".rate_min="..section.rate_min.."\n")
            state_fp:write(section.name..".rate_min_mate="..section.rate_min_mate.."\n")
            state_fp:write(section.name..".ip_group="..section.ip_group.."\n")
            state_fp:write(section.name..".mode="..section.mode.."\n")
            state_fp:write(section.name..".time="..section.time.."\n")
            state_fp:write(section.name..".ip_type="..section.ip_type.."\n")
        end
    )
    state_fp:close()
end

state_add_info()