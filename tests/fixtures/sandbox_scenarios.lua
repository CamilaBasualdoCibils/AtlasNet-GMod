-- Install only into a disposable loopback GMod gateway used for integration tests.
local function isGatewayPlayer(ply)
    return atlasnet and atlasnet.GatewayBlast and IsValid(ply)
end

concommand.Add("atlasnet_task6_blast", function(ply)
    if not isGatewayPlayer(ply) then return end
    local dynamite = ents.Create("gmod_dynamite")
    if not IsValid(dynamite) then return end
    dynamite:SetModel("models/dav0r/tnt/tnt.mdl")
    dynamite:SetPos(Vector(atlasnet.WorldOrigin()) + Vector(0, 0, 38))
    dynamite:Spawn()
    dynamite:Setup(100)
    dynamite:SetShouldRemove(true)
    timer.Simple(0.25, function()
        if IsValid(dynamite) and IsValid(ply) then dynamite:Explode(0, ply) end
    end)
end)

concommand.Add("atlasnet_task6_frag", function(ply)
    if not isGatewayPlayer(ply) or not ply:Alive() then return end
    ply:Give("weapon_frag")
    ply:SelectWeapon("weapon_frag")
end)

concommand.Add("atlasnet_task6_smggrenade", function(ply)
    if not isGatewayPlayer(ply) or not ply:Alive() then return end
    ply:Give("weapon_smg1")
    ply:GiveAmmo(2, "SMG1_Grenade")
    ply:SelectWeapon("weapon_smg1")
end)

concommand.Add("atlasnet_qa_rpg", function(ply)
    if not isGatewayPlayer(ply) or not ply:Alive() then return end
    ply:Give("weapon_rpg")
    ply:GiveAmmo(2, "RPG_Round")
    ply:SelectWeapon("weapon_rpg")
end)

concommand.Add("atlasnet_task6_aim", function(ply, _, args)
    if not isGatewayPlayer(ply) or #args ~= 3 then return end
    ply:SetEyeAngles((Vector(tonumber(args[1]), tonumber(args[2]), tonumber(args[3])) - ply:EyePos()):Angle())
end)

local function aimAfter(ply, delay, target)
    timer.Simple(delay, function()
        if IsValid(ply) and ply:Alive() then
            ply:SetEyeAngles((target - ply:EyePos()):Angle())
        end
    end)
end

concommand.Add("atlasnet_qa_turnrocket", function(ply, _, args)
    if not isGatewayPlayer(ply) or #args ~= 4 then return end
    ply.AtlasNetTestRocketAim = {delay = tonumber(args[1]),
        target = Vector(tonumber(args[2]), tonumber(args[3]), tonumber(args[4]))}
end)

hook.Add("OnEntityCreated", "AtlasNetTestAimRocket", function(ent)
    if ent:GetClass() ~= "rpg_missile" then return end
    timer.Simple(0, function()
        if not IsValid(ent) then return end
        local ply = ent:GetOwner()
        local change = IsValid(ply) and ply.AtlasNetTestRocketAim
        if not change then return end
        ply.AtlasNetTestRocketAim = nil
        aimAfter(ply, change.delay, change.target)
    end)
end)

concommand.Add("atlasnet_qa_turnhold", function(ply, _, args)
    if not isGatewayPlayer(ply) or #args ~= 4 then return end
    ply.AtlasNetTestToolAim = {delay = tonumber(args[1]),
        target = Vector(tonumber(args[2]), tonumber(args[3]), tonumber(args[4]))}
end)

hook.Add("StartCommand", "AtlasNetTestAimTool", function(ply, cmd)
    local change = ply.AtlasNetTestToolAim
    local weapon = ply:GetActiveWeapon()
    if change and IsValid(weapon) and weapon:GetClass() == "weapon_physgun" and cmd:KeyDown(IN_ATTACK) then
        ply.AtlasNetTestToolAim = nil
        aimAfter(ply, change.delay, change.target)
    end
end)

concommand.Add("atlasnet_qa_prop", function(ply, _, args)
    if not isGatewayPlayer(ply) or #args ~= 4 or not util.IsValidProp(args[1]) then return end
    local prop = ents.Create("prop_physics")
    if not IsValid(prop) then return end
    prop:SetModel(args[1])
    prop:SetPos(Vector(tonumber(args[2]), tonumber(args[3]), tonumber(args[4])))
    prop:Spawn()
    hook.Run("PlayerSpawnedProp", ply, prop:GetModel(), prop)
end)

concommand.Add("atlasnet_task6_remover", function(ply)
    if not isGatewayPlayer(ply) or not ply:Alive() then return end
    local model = "models/props_c17/oildrum001.mdl"
    local prop = ents.Create("prop_physics")
    if not IsValid(prop) then return end
    prop:SetModel(model)
    local target = Vector(atlasnet.WorldOrigin()) + Vector(25, 0, 20)
    prop:SetPos(target)
    prop:Spawn()
    hook.Run("PlayerSpawnedProp", ply, model, prop)
    ply:SelectWeapon("gmod_tool")
    ply:ConCommand("gmod_toolmode remover")
    ply:SetEyeAngles((target - ply:EyePos()):Angle())
end)
