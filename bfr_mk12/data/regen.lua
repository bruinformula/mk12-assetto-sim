function regen(dt)   
    
    ac.overrideCarState('clutch',1,ac.Wheel.All)

    if car.gas == 0 and car.speedKmh > 5 then
        ac.overrideEngineTorque(-30)
        ac.overrideGasInput(1)
    else
        ac.overrideEngineTorque(math.nan)
        ac.overrideGasInput(math.nan)
    end     
end