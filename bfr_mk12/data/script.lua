require "brakes"
require "regen"

function script.update(dt)
    brakes(dt)
	regen(dt)
end