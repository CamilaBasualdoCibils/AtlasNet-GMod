-- Install only on disposable GMod test servers.
if SERVER then
    AddCSLuaFile()
    return
end

concommand.Add("atlasnet_task6_watch", function()
    timer.Create("AtlasNetTestObservations", 0.05, 200, function()
        for _, ent in ipairs(ents.FindByClass("atlasnet_replica")) do
            if not ent:GetNoDraw() then
                local pos = ent:GetPos()
                print(("[AtlasNet QA] replica %d x=%.2f y=%.2f z=%.2f seq=%s cycle=%.3f model=%s")
                    :format(ent:EntIndex(), pos.x, pos.y, pos.z,
                        ent:GetSequenceName(ent:GetSequence()), ent:GetCycle(), ent:GetModel() or ""))
                if ent:GetModel():StartWith("models/player/") then
                    local pose = {}
                    for _, parameter in ipairs({"aim_pitch", "body_pitch", "head_pitch"}) do
                        if ent:LookupPoseParameter(parameter) >= 0 then
                            pose[#pose + 1] = ("%s=%.3f"):format(parameter, ent:GetPoseParameter(parameter))
                        end
                    end
                    local attachment = ent:LookupAttachment("anim_attachment_RH")
                    local hand = attachment > 0 and ent:GetAttachment(attachment)
                    for _, weapon in ipairs(ent:GetChildren()) do
                        if IsValid(weapon) and hand then
                            local bone = weapon:LookupBone("ValveBiped.Bip01_R_Hand")
                            local weaponPos = bone and weapon:GetBonePosition(bone) or weapon:GetPos()
                            print(("[AtlasNet QA] aim entity=%d yaw=%.2f %s weapon=%s hand_error=%.2f root_dist=%.2f")
                                :format(ent:EntIndex(), ent:GetAngles().y, table.concat(pose, " "),
                                    weapon:GetModel(), weaponPos:Distance(hand.Pos), weaponPos:Distance(pos)))
                        end
                    end
                end
            end
        end
    end)
end)
