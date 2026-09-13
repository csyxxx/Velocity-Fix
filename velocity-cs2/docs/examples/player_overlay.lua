local enabled = true
local overlay_alpha = 220

function on_menu()
    enabled = velocity.ui.checkbox("Player overlay", true)
    overlay_alpha = velocity.ui.slider_int("Overlay alpha", 220, 20, 255)

    velocity.ui.separator()
    if velocity.ui.button("Print player count") then
        velocity.log.info("players: " .. #velocity.entity.players())
    end
end

function on_paint()
    if not enabled then
        return
    end

    for _, player in ipairs(velocity.entity.players()) do
        if player.alive and player.enemy then
            local local_player = velocity.player.local()
            local visible_bone = velocity.trace.most_visible_bone(
                local_player.eye_position, player.index)
            local world_position = visible_bone and visible_bone.position or player.origin
            local projection = velocity.view.project(world_position)
            if projection.valid and projection.on_screen then
                local text = player.class_name .. " [" .. player.health .. "]"
                if visible_bone then
                    text = text .. " bone=" .. visible_bone.id
                end
                velocity.draw.text(projection.screen, text, velocity.color(255, 90, 90, overlay_alpha))
            end
        end
    end
end

velocity.on("create_move", function(command)
    if velocity.input.is_key_down(0x20) then
        command:set_button("jump", true)
    end
end)
