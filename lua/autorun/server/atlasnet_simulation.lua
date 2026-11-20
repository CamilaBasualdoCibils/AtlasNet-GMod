local api = atlasnet
if not api or not api.SimulationRead then return end

local hookName = "AtlasNetGModSimulation"
-- Values shared with the native module in Simulation/Protocol.hpp.
local Action = {
    Ready = 0, Spawn = 1, Spawned = 2, Resume = 3, Freeze = 4, Frozen = 5,
    Remove = 6, Sample = 7, Removed = 8, Input = 9, Damage = 10, Trace = 11,
    Traced = 12, Lost = 13, Blast = 14, Detonated = 15, Respawn = 16, PlayerModel = 17,
    PhysicsTool = 18, GuideRocket = 19
}
local Kind = {Player = 0, Prop = 1, Marker = 2, Frag = 3, ImpactGrenade = 4, Rocket = 5}
local Physics = {Grab = 1, Hold = 2, Release = 3, Freeze = 4, Punt = 5, Unfreeze = 6}
local projectileClasses = {
    [Kind.Frag] = "npc_grenade_frag", [Kind.ImpactGrenade] = "grenade_ar2", [Kind.Rocket] = "rpg_missile"
}
local origin = Vector(api.WorldOrigin())
local records, handles = {}, {}
local blastTarget
local ready = false
RunConsoleCommand("sv_hibernate_think", "1")

local function report(action, id, ok)
    return api.SimulationReport({ action = action, id = id, ok = ok })
end

local function readWorldBounds()
    local world = game.GetWorld()
    local mins, maxs = world:GetInternalVariable("m_WorldMins"), world:GetInternalVariable("m_WorldMaxs")
    if isvector(mins) and isvector(maxs) and mins:DistToSqr(maxs) > 0 then return mins, maxs end

    local bsp = file.Open("maps/" .. game.GetMap() .. ".bsp", "rb", "GAME")
    if not bsp then error("AtlasNet could not open the current Source map") end
    -- The entity lump is the first VBSP directory entry, after the magic and version.
    bsp:Seek(8)
    local offset, size = bsp:ReadLong(), bsp:ReadLong()
    bsp:Seek(offset)
    local entities = bsp:Read(size)
    bsp:Close()
    local worldspawn = entities and entities:match("{([^}]*)}")
    local minimum = worldspawn and worldspawn:match('"world_mins"%s+"([^"]+)"')
    local maximum = worldspawn and worldspawn:match('"world_maxs"%s+"([^"]+)"')
    if not minimum or not maximum then error("AtlasNet could not read the Source map bounds") end
    return Vector(minimum), Vector(maximum)
end

local function sample(record, action)
    local ent = record.ent
    local position, velocity = ent:GetPos() - origin, ent:GetVelocity()
    local mins, maxs = ent:OBBMins(), ent:OBBMaxs()
    local physics = record.kind == Kind.Prop and ent:GetPhysicsObject()
    local angles = ent:IsPlayer() and Angle(record.input.aimPitch, record.input.aimYaw, 0) or ent:GetAngles()
    record.position = position
    local gait = 0
    if ent:IsPlayer() and ent:Alive() then
        local speed = velocity:Length2D()
        if not ent:OnGround() then gait = 3
        elseif ent:Crouching() then gait = speed > 8 and 5 or 4
        elseif speed > ent:GetWalkSpeed() + 10 then gait = 2
        elseif speed > 8 then gait = 1 end
    end
    local fuse = 0
    if record.kind == Kind.Frag then
        fuse = record.frozen and record.fuse or math.max(0, record.detonatesAt - CurTime())
    elseif record.kind == Kind.ImpactGrenade then
        fuse = record.frozen and record.fuse or (ent:GetInternalVariable("m_bIsLive") == false and
            math.max(0.001, CurTime() - record.launchedAt) or 0)
    elseif record.kind == Kind.Rocket then
        fuse = record.frozen and record.fuse or
            (ent:GetMoveType() ~= MOVETYPE_FLY and math.max(0, record.igniteAt - CurTime()) or 0)
    end
    api.SimulationReport({
        action = action, id = record.id, ok = true,
        x = position.x, y = position.y, z = position.z,
        vx = velocity.x, vy = velocity.y, vz = velocity.z,
        minx = mins.x, miny = mins.y, minz = mins.z,
        maxx = maxs.x, maxy = maxs.y, maxz = maxs.z,
        pitch = angles.p, yaw = angles.y,
        eye = ent:IsPlayer() and (ent:EyePos().z - ent:GetPos().z) or 0,
        health = ent:Health(), armor = ent:IsPlayer() and ent:Armor() or 0,
        alive = (not ent:IsPlayer() or ent:Alive()) and 1 or 0,
        motion = (not IsValid(physics) or physics:IsMotionEnabled()) and 1 or 0,
        respawnDelay = ent:IsPlayer() and not ent:Alive() and
            math.max(0, (ent.NextSpawnTime or 0) - CurTime()) or 0,
        fuse = fuse,
        gait = gait, model = not ent:IsPlayer() and ent:GetModel() or nil
    })
end

local function freeze(record, enabled)
    record.frozen = enabled
    local ent = record.ent
    if ent:IsPlayer() then
        if not enabled then record.committed = true end
        ent:Freeze(enabled)
    elseif record.kind == Kind.Rocket then
        if enabled then
            record.velocity = ent:GetVelocity()
            if record.igniteAt then
                record.fuse = ent:GetMoveType() ~= MOVETYPE_FLY and math.max(0, record.igniteAt - CurTime()) or 0
            end
            ent:NextThink(CurTime() + 3600)
            ent:SetSolid(SOLID_NONE)
            ent:SetMoveType(MOVETYPE_NONE)
        else
            record.igniteAt = CurTime() + record.fuse
            ent:NextThink(CurTime() + math.max(engine.TickInterval(), record.fuse))
            ent:SetSolid(SOLID_BBOX)
            ent:SetMoveType(record.fuse == 0 and MOVETYPE_FLY or MOVETYPE_FLYGRAVITY, MOVECOLLIDE_FLY_BOUNCE)
            ent:SetLocalVelocity(record.velocity)
        end
    elseif record.kind == Kind.ImpactGrenade then
        if enabled then
            record.velocity = ent:GetVelocity()
            if record.launchedAt then
                record.fuse = ent:GetInternalVariable("m_bIsLive") == false and
                    math.max(0.001, CurTime() - record.launchedAt) or 0
            end
            ent:SetSaveValue("m_bIsLive", false)
            ent:SetSaveValue("m_fSpawnTime", math.huge)
            ent:SetSolid(SOLID_NONE)
            ent:SetMoveType(MOVETYPE_NONE)
        else
            record.launchedAt = CurTime() - record.fuse
            ent:SetSaveValue("m_fSpawnTime", record.launchedAt)
            ent:SetSaveValue("m_bIsLive", record.fuse == 0)
            ent:SetSolid(SOLID_BBOX)
            ent:SetMoveType(MOVETYPE_FLYGRAVITY, MOVECOLLIDE_FLY_BOUNCE)
            ent:SetLocalVelocity(record.velocity)
        end
    else
        local physics = ent:GetPhysicsObject()
        if record.kind == Kind.Prop and IsValid(physics) then
            if enabled then
                record.motion = physics:IsMotionEnabled()
                record.velocity = physics:GetVelocity()
            end
            physics:EnableMotion(not enabled and record.motion)
            if not enabled and record.motion then
                physics:SetVelocity(record.velocity)
                physics:Wake()
            end
            return
        end
        if record.kind == Kind.Frag then
            if enabled then
                if record.detonatesAt then record.fuse = math.max(0, record.detonatesAt - CurTime()) end
                if IsValid(physics) then record.velocity = physics:GetVelocity() end
                ent:Input("SetTimer", game.GetWorld(), game.GetWorld(), "3600")
            else
                local fuse = math.max(0.02, record.fuse)
                ent:Input("SetTimer", game.GetWorld(), game.GetWorld(), tostring(fuse))
                record.detonatesAt = CurTime() + fuse
            end
        end
        if IsValid(physics) then
            physics:EnableMotion(not enabled and record.kind ~= Kind.Marker)
            if record.kind == Kind.Frag and not enabled then physics:SetVelocity(record.velocity) end
        end
    end
end

local function spawn(msg)
    if records[msg.id] then return end
    local ent
    if msg.kind == Kind.Player then
        ent = player.CreateNextBot("AtlasNet-" .. msg.id)
    elseif projectileClasses[msg.kind] then
        ent = ents.Create(projectileClasses[msg.kind])
        if IsValid(ent) then
            local owner = records[msg.target]
            if owner and IsValid(owner.ent) then ent:SetOwner(owner.ent) end
            ent:SetPos(origin + Vector(msg.x, msg.y, msg.z))
            ent:SetAngles(Angle(msg.pitch, msg.yaw, 0))
            ent:Spawn()
        end
    else
        ent = ents.Create("prop_physics")
        if IsValid(ent) then
            local model = msg.model
            if msg.kind == Kind.Marker and model == "" then model = "models/props_junk/wood_crate001a.mdl" end
            if not util.IsValidProp(model) then
                ent:Remove()
                report(Action.Spawned, msg.id, false)
                return
            end
            ent:SetModel(model)
            ent:SetPos(origin + Vector(msg.x, msg.y, msg.z))
            ent:SetAngles(Angle(msg.pitch, msg.yaw, 0))
            ent:Spawn()
        end
    end
    if not IsValid(ent) then
        report(Action.Spawned, msg.id, false)
        return
    end
    local record = { id = msg.id, ent = ent, kind = msg.kind, input = msg,
        position = Vector(msg.x, msg.y, msg.z), fuse = msg.fuse }
    records[msg.id], handles[ent] = record, record
    ent:SetPos(origin + Vector(msg.x, msg.y, msg.z))
    local velocity = Vector(msg.vx, msg.vy, msg.vz)
    if ent:IsPlayer() then
        if msg.model ~= "" and util.IsValidModel(msg.model) then
            record.model = msg.model
            ent:SetModel(record.model)
        end
        ent:SetEyeAngles(Angle(msg.pitch, msg.yaw, 0))
        ent:SetVelocity(velocity - ent:GetVelocity())
        ent:SetArmor(msg.armor)
        if msg.alive == 0 then
            ent:KillSilent()
            ent.NextSpawnTime = CurTime() + msg.respawnDelay
        end
    else
        ent:SetAngles(Angle(msg.pitch, msg.yaw, 0))
        if msg.kind == Kind.ImpactGrenade or msg.kind == Kind.Rocket then
            ent:SetLocalVelocity(velocity)
        else
            local physics = ent:GetPhysicsObject()
            if IsValid(physics) then
                if msg.kind == Kind.Prop then physics:EnableMotion(msg.motion ~= 0) end
                physics:SetVelocity(velocity)
            end
        end
    end
    if not projectileClasses[msg.kind] then ent:SetHealth(msg.health) end
    freeze(record, true)
    report(Action.Spawned, msg.id, true)
end

local function remove(record)
    local ent = record.ent
    records[record.id], handles[ent] = nil, nil
    if not IsValid(ent) then return end
    if ent:IsPlayer() then ent:Kick("AtlasNet entity removed") else ent:Remove() end
end

hook.Add("EntityRemoved", hookName, function(ent)
    local record = handles[ent]
    if not record then return end
    records[record.id], handles[ent] = nil, nil
    if projectileClasses[record.kind] and not record.frozen then
        local exploded = record.exploded or (record.detonatesAt or math.huge) <= CurTime() + 0.1
        if record.kind == Kind.ImpactGrenade and ent:GetInternalVariable("m_takedamage") == 0 then
            exploded = true
        end
        if record.kind == Kind.Rocket and ent:GetInternalVariable("m_takedamage") == 0 then
            local start = ent:GetPos()
            local trace = util.TraceLine({ start = start, endpos = start + ent:GetForward() * 16,
                filter = ent, mask = MASK_SHOT })
            exploded = not trace.HitSky
        end
        if exploded then
            local p = ent:GetPos() - origin
            api.SimulationReport({ action = Action.Detonated, id = record.id, ok = true, x = p.x, y = p.y, z = p.z })
            return
        end
    end
    report(Action.Removed, record.id, true)
end)

hook.Add("EntityTakeDamage", hookName, function(ent, damage)
    if blastTarget and blastTarget ~= ent then return true end
    local inflictor = handles[damage:GetInflictor()]
    if inflictor and projectileClasses[inflictor.kind] then
        inflictor.exploded = true
        inflictor.position = inflictor.ent:GetPos() - origin
        return true
    end
    local record = handles[ent]
    if record and record.frozen then return true end
end)

local movementButtons = bit.bor(IN_FORWARD, IN_BACK, IN_MOVELEFT, IN_MOVERIGHT,
    IN_JUMP, IN_DUCK, IN_SPEED, IN_WALK)
hook.Add("PlayerDeathThink", hookName, function(ply)
    -- The base game automatically respawns bots. This bot represents a human
    -- who must be able to choose when to spawn from their own Source session.
    if handles[ply] then return false end
end)
hook.Add("StartCommand", hookName, function(ply, cmd)
    local record = handles[ply]
    if not record then return end
    cmd:ClearMovement()
    cmd:ClearButtons()
    if record.frozen then return end
    local input = record.input
    cmd:SetViewAngles(Angle(input.aimPitch, input.aimYaw, 0))
    if (record.lastInput or 0) + 0.25 < SysTime() then return end
    cmd:SetForwardMove(input.forward)
    cmd:SetSideMove(input.side)
    if ply:Alive() then cmd:SetButtons(bit.band(input.buttons, movementButtons)) end
end)

local function controlPhysics(record, msg)
    if record.kind ~= Kind.Prop or record.frozen then return end
    local physics = record.ent:GetPhysicsObject()
    if not IsValid(physics) then return end
    local operation = msg.operation
    if msg.tool == 2 then
        local maxMass = GetConVar("physcannon_maxmass")
        if maxMass and physics:GetMass() > maxMass:GetFloat() then return end
    end
    if operation == Physics.Grab or operation == Physics.Hold then
        if not record.shadow then
            physics:EnableMotion(true)
            record.shadow = {
                angle = physics:GetAngles(), secondstoarrive = 0.09,
                maxangular = 2000, maxangulardamp = 8000, maxspeed = 1800,
                maxspeeddamp = 5000, dampfactor = 0.8, teleportdistance = 0,
                delta = engine.TickInterval()
            }
        end
        record.shadow.pos = origin + Vector(msg.x, msg.y, msg.z)
        record.shadowExpires = SysTime() + 0.5
        return
    end
    record.shadow = nil
    if operation == Physics.Freeze then
        physics:EnableMotion(false)
    elseif operation == Physics.Unfreeze then
        physics:EnableMotion(true)
        physics:Wake()
    elseif operation == Physics.Punt then
        if not physics:IsMotionEnabled() then return end
        local direction = Vector(msg.dx, msg.dy, msg.dz)
        local maxForce = GetConVar("physcannon_maxforce")
        physics:Wake()
        physics:ApplyForceCenter(direction * physics:GetMass() * (maxForce and maxForce:GetFloat() or 1500))
    end
end

local function steerRocket(record, now)
    if not record.laser or now >= record.laserExpires or record.frozen then return end
    local ent = record.ent
    if ent:GetMoveType() ~= MOVETYPE_FLY then return end
    local velocity = ent:GetVelocity()
    local speed = velocity:Length()
    local position = ent:GetPos()
    local target = record.laser - position
    if speed <= 0 or target:LengthSqr() < 1 then return end
    -- Source's RPG blends the current and desired directions at each flight tick.
    local direction = LerpVector(0.125, velocity / speed, target:GetNormalized()):GetNormalized()
    ent:SetAngles(direction:Angle())
    ent:SetLocalVelocity(direction * speed)
end

local function trace(msg)
    local start = origin + Vector(msg.x, msg.y, msg.z)
    local direction = Vector(msg.dx, msg.dy, msg.dz)
    local query = {
        start = start, endpos = start + direction * msg.distance, mask = MASK_SHOT,
        filter = function(ent)
            local record = handles[ent]
            return record and (record.committed or not record.frozen) and record.id ~= msg.id and
                (not ent:IsPlayer() or ent:Alive())
        end
    }
    local hit
    if msg.traceHull > 0 then
        local extent = Vector(msg.traceHull, msg.traceHull, msg.traceHull)
        query.mins, query.maxs, query.mask = -extent, extent, MASK_SOLID
        hit = util.TraceHull(query)
    else
        hit = util.TraceLine(query)
    end
    local target = handles[hit.Entity]
    api.SimulationReport({ action = Action.Traced, id = msg.id, ok = hit.Hit,
        distance = hit.Fraction * msg.distance,
        target = target and target.id or "0" })
end

hook.Add("Think", hookName, function()
    if not ready then
        local mins, maxs = readWorldBounds()
        mins, maxs = mins - origin, maxs - origin
        ready = api.SimulationReport({action = Action.Ready, id = "0", ok = true,
            minx = mins.x, miny = mins.y, minz = mins.z,
            maxx = maxs.x, maxy = maxs.y, maxz = maxs.z})
        if not ready then return end
    end
    for _ = 1, 128 do
        local msg = api.SimulationRead()
        if not msg then break end
        local record = records[msg.id]
        if msg.action == Action.Lost then engine.CloseServer() return end
        if msg.action == Action.Spawn then spawn(msg)
        elseif msg.action == Action.Trace then trace(msg)
        elseif record then
            if msg.action == Action.Resume then freeze(record, false)
            elseif msg.action == Action.Freeze then
                sample(record, Action.Frozen)
                freeze(record, true)
            elseif msg.action == Action.Remove then remove(record)
            elseif msg.action == Action.Input then record.input, record.lastInput = msg, SysTime()
            elseif msg.action == Action.PhysicsTool then controlPhysics(record, msg)
            elseif msg.action == Action.GuideRocket and record.kind == Kind.Rocket then
                record.laser = origin + Vector(msg.x, msg.y, msg.z)
                record.laserExpires = SysTime() + 0.2
            elseif msg.action == Action.PlayerModel and record.ent:IsPlayer() and util.IsValidModel(msg.model) then
                record.model = msg.model
                record.ent:SetModel(record.model)
            elseif msg.action == Action.Respawn then
                local ply = record.ent
                if not record.frozen and not ply:Alive() and (ply.NextSpawnTime or 0) <= CurTime() then
                    ply:Spawn()
                    if record.model then ply:SetModel(record.model) end
                    sample(record, Action.Sample)
                end
            elseif msg.action == Action.Damage or msg.action == Action.Blast then
                local source = records[msg.target]
                local attacker = source and source.ent or game.GetWorld()
                if msg.action == Action.Damage then
                    local damage = DamageInfo()
                    damage:SetAttacker(attacker)
                    damage:SetInflictor(attacker)
                    damage:SetDamage(msg.damage)
                    damage:SetDamageType(msg.damageType)
                    record.ent:TakeDamageInfo(damage)
                else
                    blastTarget = record.ent
                    local ok, err = pcall(util.BlastDamage, game.GetWorld(), attacker,
                        origin + Vector(msg.x, msg.y, msg.z), msg.radius, msg.damage)
                    blastTarget = nil
                    if not ok then ErrorNoHaltWithStack(err) end
                end
                sample(record, Action.Sample)
            end
        end
    end
end)

hook.Add("Think", hookName .. "Physics", function()
    local now = SysTime()
    for _, record in pairs(records) do
        if record.kind == Kind.Rocket and IsValid(record.ent) then steerRocket(record, now) end
        if record.shadow and not record.frozen then
            local physics = record.ent:GetPhysicsObject()
            if now >= record.shadowExpires or not IsValid(physics) then
                record.shadow = nil
            else
                physics:Wake()
                physics:ComputeShadowControl(record.shadow)
            end
        end
    end
end)

timer.Create(hookName, 0.05, 0, function()
    for _, record in pairs(records) do
        if not record.frozen and IsValid(record.ent) then sample(record, Action.Sample) end
    end
end)
print("[AtlasNet-GMod] Shard simulation hooks loaded")
