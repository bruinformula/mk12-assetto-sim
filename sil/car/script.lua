-- MK12 SIL car script entry point: rear electric brake balance + VCU bridge.
require "brakes"
require "vcu_bridge"

function script.update(dt)
    brakes(dt)
    vcu_bridge(dt)
end
