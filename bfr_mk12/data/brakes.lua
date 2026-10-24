function brakes(dt)    
    local brakePedalPos = car.brake  
    local brakes_ini = ac.INIConfig.carData(0, "brakes.ini")
    local pedalRatio = brakes_ini:get("DATA2", "PEDAL_RATIO", 4.5)
    local masterCylinderDia = brakes_ini:get("REAR", "MASTER_CYL_DIA_0", 0.0222)
    local pistonDia = brakes_ini:get("REAR", "PISTON_DIAMETER_0", 0.03175)
    local discEffRad = brakes_ini:get("REAR", "DISC_EFFECTIVE_RADIUS", 0.076835)
    local pedalFaceForce = brakes_ini:get("DATA2", "PEDAL_FORCE_REF", 45.35) * 9.81
    local boost = 0
    local pi = 3.14159
    local mu = 0.4
    local pistonCount = 4
    local carPh = ac.accessCarPhysics()
    local BB = ac.getCar(0).brakeBias
    
    F_in = brakePedalPos * pedalFaceForce * (1-BB)
    F_MC = F_in * pedalRatio * (1 + boost)
    P_line = F_MC / (pi*(masterCylinderDia/2)^2)
    F_clamp = pistonCount * P_line * (pi*(pistonDia/2)^2)*1
    F_friction = F_clamp * mu
    T_brake = F_friction * discEffRad
    T_brake_eff = T_brake / 1
    
    local wl = carPh.wheels[2].angularSpeed
    local wr = carPh.wheels[3].angularSpeed
    local w_rear = (wl + wr) /2

    local lock_thresh = 0.1

    if brakePedalPos > 0.001 then
        local speedMult = math.abs(w_rear) / lock_thresh
        if speedMult > 1.0 then 
            speedMult = 1.0 
        end 

        if w_rear > 0 then
            ac.addElectricTorque(ac.Wheel.Rear, -T_brake_eff * speedMult, true)
        elseif w_rear < 0 then
            ac.addElectricTorque(ac.Wheel.Rear, T_brake_eff * speedMult, true)
        else
            ac.addElectricTorque(ac.Wheel.Rear, 0, true)
        end
    else
        ac.addElectricTorque(ac.Wheel.Rear, 0, true)
    end
end