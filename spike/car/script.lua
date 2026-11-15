-- Spike version of the car script: brakes as usual, spike.lua instead of regen.
require "brakes"
require "spike"

function script.update(dt)
    brakes(dt)
    spike(dt)
end
