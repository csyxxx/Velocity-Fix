local prefix = "ragebot.sniper."

velocity.schedule(0.5, function()
    local state = velocity.modules.state("ragebot")
    if state then
        velocity.log.info("ragebot state ready; dt=" .. state.doubletap_state)
    end
end)

function on_menu()
    local enabled = velocity.modules.get("ragebot.enabled") or false
    local requested = velocity.ui.checkbox("Control ragebot", enabled)
    if requested ~= enabled then
        velocity.modules.set("ragebot.enabled", requested)
    end

    local no_spread = velocity.modules.get(prefix .. "no_spread") or false
    local requested_no_spread = velocity.ui.checkbox("Sniper no spread", no_spread)
    if requested_no_spread ~= no_spread then
        velocity.modules.set(prefix .. "no_spread", requested_no_spread)
    end

    local hitchance = velocity.config.get(prefix .. "hitchance") or 80
    local requested_hitchance = velocity.ui.slider_int("Sniper hitchance", hitchance, 0, 100)
    if requested_hitchance ~= hitchance then
        velocity.config.set(prefix .. "hitchance", requested_hitchance)
    end

    local pointscale = velocity.config.get(prefix .. "pointscale") or 85.0
    local requested_pointscale = velocity.ui.slider_float("Sniper point scale", pointscale, 0.0, 100.0)
    if math.abs(requested_pointscale - pointscale) > 0.001 then
        velocity.config.set(prefix .. "pointscale", requested_pointscale)
    end

    local color_key = prefix .. "hittable_region_direct_color"
    local current_color = velocity.config.get(color_key) or velocity.color(75, 220, 145, 95)
    velocity.config.set(color_key, velocity.ui.color_picker("Hittable color", current_color))

    if velocity.ui.button("Toggle thirdperson") then
        velocity.modules.toggle("camera.thirdperson")
    end

    local state = velocity.modules.state("ragebot")
    if state then
        velocity.ui.text("Target: " .. tostring(state.target_index))
        velocity.ui.text("Firing: " .. tostring(state.firing_this_tick))
    end
end
