local api = atlasnet
if not api or not api.GatewayStart then return end

local hookName = "AtlasNetGModGateway"
local View = {Own = 0, Enter = 1, Exit = 3, Damage = 4, Presentation = 5, Destroy = 6}
local Kind = {Player = 0, Frag = 3, ImpactGrenade = 4, Rocket = 5}
local projectileKinds = {
    npc_grenade_frag = Kind.Frag, grenade_ar2 = Kind.ImpactGrenade, rpg_missile = Kind.Rocket
}
local ports, users = {}, {}

local distributed = api.GatewayInput ~= nil
local origin = Vector(api.WorldOrigin())
local holdNames = {"normal", "pistol", "smg", "grenade", "ar2", "shotgun", "rpg",
    "physgun", "crossbow", "melee", "slam", "fist", "melee2", "passive", "knife",
    "duel", "camera", "magic", "revolver"}
local holdIDs = {}
for i, hold in ipairs(holdNames) do holdIDs[hold] = i - 1 end
local holdActivities = {ACT_HL2MP_IDLE, ACT_HL2MP_IDLE_PISTOL, ACT_HL2MP_IDLE_SMG1,
    ACT_HL2MP_IDLE_GRENADE, ACT_HL2MP_IDLE_AR2, ACT_HL2MP_IDLE_SHOTGUN,
    ACT_HL2MP_IDLE_RPG, ACT_HL2MP_IDLE_PHYSGUN, ACT_HL2MP_IDLE_CROSSBOW,
    ACT_HL2MP_IDLE_MELEE, ACT_HL2MP_IDLE_SLAM, ACT_HL2MP_IDLE_FIST, ACT_HL2MP_IDLE_MELEE2,
    ACT_HL2MP_IDLE_PASSIVE, ACT_HL2MP_IDLE_KNIFE, ACT_HL2MP_IDLE_DUEL, ACT_HL2MP_IDLE_CAMERA,
    ACT_HL2MP_IDLE_MAGIC, ACT_HL2MP_IDLE_REVOLVER}
-- Source's stock multiplayer weapon activity table groups the gaits by hold type.
local gaitOffsets = {0, 1, 2, 7, 3, 4}
local gaitSequences = {"idle_all_01", "walk_all", "run_all", "jump_holding_glide",
    "cidle_all", "cwalk_all"}

local poseInterval = 0.05
local function applyAim(proxy, pitch)
    for _, parameter in ipairs({"aim_pitch", "body_pitch", "head_pitch"}) do
        if proxy:LookupPoseParameter(parameter) >= 0 then
            proxy:SetPoseParameter(parameter, math.NormalizeAngle(pitch))
            return
        end
    end
end

local function updateRemotePose(proxy, state)
    local target = origin + Vector(state.x, state.y, state.z)
    local angle = Angle(state.kind == Kind.Player and 0 or state.pitch, state.yaw, 0)
    local position = proxy:GetPos()
    if position:DistToSqr(target) > 128 * 128 then
        proxy:SetPos(target)
        proxy:SetAngles(angle)
        proxy.AtlasNetPose = nil
        proxy.AtlasNetAimPitch = state.pitch
        if state.kind == Kind.Player then applyAim(proxy, state.pitch) end
        return
    end
    proxy.AtlasNetPose = {from = position, to = target, start = CurTime(),
        fromAngle = proxy:GetAngles(), toAngle = angle, player = state.kind == Kind.Player,
        fromPitch = proxy.AtlasNetAimPitch or state.pitch, toPitch = state.pitch}
end

local function updateWeapon(session, proxy, model)
    if proxy.AtlasNetWeaponModel == model then return end
    if IsValid(proxy.AtlasNetWeapon) then proxy.AtlasNetWeapon:Remove() end
    if model == "" or not util.IsValidModel(model) then
        proxy.AtlasNetWeaponModel = model
        return
    end
    local handAttachment = proxy:LookupAttachment("anim_attachment_RH")
    local handBone = proxy:LookupBone("ValveBiped.Bip01_R_Hand")
    if handAttachment <= 0 and not handBone then return end
    local weapon = ents.Create("prop_dynamic")
    if not IsValid(weapon) then return end
    weapon:SetModel(model)
    weapon:SetPos(proxy:GetPos())
    weapon:SetKeyValue("DisableBoneFollowers", "1")
    weapon:Spawn()
    weapon:SetSolid(SOLID_NONE)
    weapon:SetMoveType(MOVETYPE_NONE)
    if handBone and weapon:LookupBone("ValveBiped.Bip01_R_Hand") then
        weapon:SetParent(proxy)
        weapon:AddEffects(EF_BONEMERGE)
    else
        if handAttachment > 0 then
            weapon:SetParent(proxy, handAttachment)
        else
            weapon:FollowBone(proxy, handBone)
        end
        weapon:SetLocalPos(vector_origin)
        weapon:SetLocalAngles(angle_zero)
    end
    proxy:DeleteOnRemove(weapon)
    proxy.AtlasNetWeapon = weapon
    proxy.AtlasNetWeaponModel = model
    for _, other in ipairs(player.GetHumans()) do
        if other ~= session.player then weapon:SetPreventTransmit(other, true) end
    end
    session.weapons = (session.weapons or 0) + 1
end

local function applyView(session, state)
    local ply = session.player
    if not IsValid(ply) then return false end
    if state.action == View.Own or state.action == View.Damage then
        if session.revision and state.revision < session.revision then return true end
        session.revision = state.revision
        local first = session.entity == nil
        if first or session.shard ~= state.shard then
            local summary = ("[AtlasNet] client=%s entity=%s shard=%s region=%d role=session proxy")
                :format(session.client, state.id, state.shard, state.region)
            ply:PrintMessage(HUD_PRINTCONSOLE, summary .. "\n")
            print(summary)
        end
        session.entity, session.shard, session.region = state.id, state.shard, state.region
        if state.alive == 0 then
            if ply:Alive() then ply:Kill() end
            return true
        end
        local respawned = not ply:Alive()
        if respawned then ply:Spawn() end
        local position = origin + Vector(state.x, state.y, state.z)
        if first or respawned or ply:GetPos():DistToSqr(position) > 28 * 28 then
            ply:SetPos(position)
            ply:SetVelocity(Vector(state.vx, state.vy, state.vz) - ply:GetVelocity())
        end
        if first then
            ply:SetEyeAngles(Angle(state.pitch, state.yaw, 0))
            ply:Freeze(false)
        end
        if ply:Health() ~= math.floor(state.health) then ply:SetHealth(math.floor(state.health)) end
        if ply:Armor() ~= math.floor(state.armor) then ply:SetArmor(math.floor(state.armor)) end
        return true
    end

    local proxy = session.proxies[state.id]
    if state.action == View.Destroy then
        session.destroyed = session.destroyed or {}
        session.destroyed[state.id] = SysTime() + 30
        if IsValid(proxy) then proxy:Remove() end
        session.proxies[state.id] = nil
        print("[AtlasNet-GMod] Proxy destroy observer=" .. tostring(session.entity) .. " entity=" .. state.id)
        return true
    end
    if session.destroyed and session.destroyed[state.id] then return true end
    if state.action == View.Presentation then
        if not IsValid(proxy) or (proxy.AtlasNetRevision or -1) >= state.revision then return true end
        proxy.AtlasNetRevision = state.revision
        session.presentations = (session.presentations or 0) + 1
        proxy.AtlasNetPoseDriven = true
        updateRemotePose(proxy, state)
        if state.kind ~= Kind.Player then
            if state.model ~= "" and proxy:GetModel() ~= state.model then
                proxy:SetModel(state.model)
                proxy:SetNoDraw(false)
            end
            return true
        end
        if state.model == "" then return true end
        if proxy:GetModel() ~= state.model then
            proxy:SetModel(state.model)
            proxy.AtlasNetGait = nil
            proxy.AtlasNetWeaponModel = nil
            return true
        end
        proxy:SetNoDraw(state.alive == 0)
        updateWeapon(session, proxy, state.alive ~= 0 and state.weapon or "")
        if proxy.AtlasNetGait ~= state.gait or proxy.AtlasNetHold ~= state.hold then
            proxy.AtlasNetGait = state.gait
            proxy.AtlasNetHold = state.hold
            local idle = holdActivities[state.hold + 1] or ACT_HL2MP_IDLE
            local activity = idle + (gaitOffsets[state.gait + 1] or 0)
            if state.hold == 0 and state.gait == 3 then activity = ACT_HL2MP_JUMP_SLAM end
            local sequence = proxy:SelectWeightedSequence(activity)
            if not sequence or sequence < 0 then
                sequence = proxy:LookupSequence(gaitSequences[state.gait + 1] or gaitSequences[1])
            end
            if sequence and sequence >= 0 then
                proxy:ResetSequence(sequence)
                session.gaits = bit.bor(session.gaits or 0, bit.lshift(1, state.gait))
            end
        end
        if state.gait == 1 or state.gait == 2 or state.gait == 5 then
            local velocity = Vector(state.vx, state.vy, 0)
            local heading = Angle(0, state.yaw, 0)
            local speed = velocity:Length2D()
            proxy:SetPoseParameter("move_x", velocity:Dot(heading:Forward()) / math.max(speed, 1))
            proxy:SetPoseParameter("move_y", -velocity:Dot(heading:Right()) / math.max(speed, 1))
            local sequenceSpeed = proxy:GetSequenceGroundSpeed(proxy:GetSequence())
            if sequenceSpeed > 0 then proxy:SetPlaybackRate(math.Clamp(speed / sequenceSpeed, 0.1, 2.5)) end
        else
            proxy:SetPlaybackRate(1)
        end
        return true
    end
    if state.action == View.Exit then
        if IsValid(proxy) then proxy:Remove() end
        session.proxies[state.id] = nil
        print("[AtlasNet-GMod] Proxy exit observer=" .. tostring(session.entity) .. " entity=" .. state.id)
        return true
    end
    if state.action == View.Enter and not IsValid(proxy) then
        proxy = ents.Create("atlasnet_replica")
        if not IsValid(proxy) then return false end
        proxy:SetPos(origin + Vector(state.x, state.y, state.z))
        proxy:Spawn()
        proxy:SetNoDraw(true)
        proxy:SetSolid(SOLID_NONE)
        for _, other in ipairs(player.GetHumans()) do
            if other ~= ply then proxy:SetPreventTransmit(other, true) end
        end
        session.proxies[state.id] = proxy
        print("[AtlasNet-GMod] Proxy enter observer=" .. tostring(session.entity) .. " entity=" .. state.id)
    end
    if IsValid(proxy) and not proxy.AtlasNetPoseDriven then
        local position = origin + Vector(state.x, state.y, state.z)
        local movement = position - proxy:GetPos()
        if state.kind == Kind.Player and not proxy.AtlasNetRevision and movement:Length2DSqr() > 1 then
            proxy:SetAngles(Angle(0, movement:Angle().y, 0))
        end
        proxy:SetPos(position)
    end
    return true
end

if distributed then
    local respawnButtons = bit.bor(IN_ATTACK, IN_ATTACK2, IN_JUMP)
    local physicsWeapons = {
        weapon_physgun = {id = 1, range = "physgun_maxrange", defaultRange = 4096},
        weapon_physcannon = {id = 2, range = "physcannon_tracelength", defaultRange = 250}
    }
    local function requestRespawn(session)
        if session.entity and (session.nextRespawnRequest or 0) <= SysTime() then
            session.nextRespawnRequest = SysTime() + 0.2
            api.GatewaySend("respawn", session.port, session.token)
        end
    end

    hook.Add("StartCommand", hookName, function(ply, cmd)
        local session = users[ply:UserID()]
        if not session or not IsValid(session.player) or (session.nextInput or 0) > SysTime() then return end
        session.nextInput = SysTime() + 0.033
        if not session.entity then
            api.GatewayInput(session.port, session.token, 0, 0, 0, 0, 0)
            return
        end
        if not ply:Alive() and bit.band(cmd:GetButtons(), respawnButtons) ~= 0 then requestRespawn(session) end
        local weapon = ply:Alive() and ply:GetActiveWeapon()
        local playerModel = ply:GetModel()
        if session.activeWeapon ~= weapon or session.playerModel ~= playerModel then
            local model = IsValid(weapon) and (weapon:GetWeaponWorldModel() or weapon:GetModel() or "") or ""
            if model == "" and IsValid(weapon) then model = weapon:GetModel() or "" end
            local hold = IsValid(weapon) and holdIDs[weapon:GetHoldType()] or 0
            if playerModel and api.GatewayWeaponView(session.port, session.token, model, hold or 0, playerModel) then
                session.activeWeapon = weapon
                session.playerModel = playerModel
            end
        end
        local aim = cmd:GetViewAngles()
        local tool = IsValid(weapon) and physicsWeapons[weapon:GetClass()]
        local range = tool and GetConVar(tool.range)
        local laser
        if IsValid(weapon) and weapon:GetClass() == "weapon_rpg" then
            local secondary = cmd:KeyDown(IN_ATTACK2)
            if session.rpg ~= weapon then
                session.rpg, session.rpgGuiding, session.rpgSecondary = weapon, true, false
            end
            if secondary and not session.rpgSecondary then session.rpgGuiding = not session.rpgGuiding end
            session.rpgSecondary = secondary
            if session.rpgGuiding then
                local dot = weapon:GetInternalVariable("m_hLaserDot")
                if IsValid(dot) then
                    laser = dot:GetPos() - origin
                else
                    local query = util.GetPlayerTrace(ply, aim:Forward())
                    query.mask = MASK_SHOT
                    local trace = util.TraceLine(query)
                    laser = trace.HitPos - origin
                end
            end
        else
            session.rpg = nil
        end
        api.GatewayInput(session.port, session.token, cmd:GetForwardMove(), cmd:GetSideMove(),
            aim.p, aim.y, cmd:GetButtons(), tool and tool.id or 0,
            tool and ((range and range:GetFloat()) or tool.defaultRange) or 0,
            laser and 1 or 0, laser and laser.x or 0, laser and laser.y or 0, laser and laser.z or 0)
    end)

    hook.Add("PostEntityFireBullets", hookName, function(ply, data)
        if not ply:IsPlayer() then return end
        local session = users[ply:UserID()]
        if not session or not session.entity or not ply:Alive() then return end
        local trace = data.Trace
        if not trace or trace.Fraction <= 0 then return end
        local distance = trace.HitPos:Distance(trace.StartPos) / trace.Fraction
        local damage = data.Damage or 0
        local ammo = data.AmmoType
        if isstring(ammo) then ammo = game.GetAmmoID(ammo) end
        if not isnumber(ammo) or ammo < 0 then
            local weapon = ply:GetActiveWeapon()
            ammo = IsValid(weapon) and weapon:GetPrimaryAmmoType() or -1
        end
        if damage <= 0 and ammo >= 0 then damage = game.GetAmmoPlayerDamage(ammo) end
        if damage <= 0 then return end
        local direction = trace.Normal
        api.GatewayBullet(session.port, session.token, direction.x, direction.y, direction.z,
            distance, damage, bit.bor(DMG_BULLET, ammo >= 0 and game.GetAmmoDamageType(ammo) or 0))
    end)

    hook.Add("EntityTakeDamage", hookName, function(ent)
        if ent:IsPlayer() then return true end
    end)

    hook.Add("PlayerDeathThink", hookName, function(ply)
        local session = users[ply:UserID()]
        if not session then return end
        if ply:KeyDown(IN_ATTACK) or ply:KeyDown(IN_ATTACK2) or ply:KeyDown(IN_JUMP) then requestRespawn(session) end
        return false
    end)

    local function forwardSound(session, name, volume, channel, level, pitch, dsp)
        local tick = engine.TickCount()
        if session.soundTick ~= tick then
            session.soundTick = tick
            session.soundNames = {}
        end
        if session.soundNames[name] then return end
        session.soundNames[name] = true
        api.GatewaySound(session.port, session.token, name, volume, channel, level, pitch, dsp)
    end

    hook.Add("PlayerFootstep", hookName, function(ply, _, _, name, volume)
        local session = users[ply:UserID()]
        if session and session.entity then forwardSound(session, name, volume, CHAN_BODY, 75, 100, 0) end
    end)

    hook.Add("EntityEmitSound", hookName, function(data)
        local ent = data.Entity
        if not IsValid(ent) then return end
        if ent:IsWeapon() then ent = ent:GetOwner() end
        if not IsValid(ent) or not ent:IsPlayer() then return end
        local session = users[ent:UserID()]
        if not session or not session.entity or not data.SoundName or
            bit.band(data.Flags or 0, SND_STOP) ~= 0 then return end
        forwardSound(session, data.SoundName, data.Volume or 1,
            data.Channel or CHAN_AUTO, data.SoundLevel or 75, data.Pitch or 100, data.DSP or 0)
    end)

    hook.Add("OnEntityCreated", hookName, function(ent)
        local class = ent:GetClass()
        local kind = projectileKinds[class]
        if class ~= "gmod_dynamite" and not kind then return end
        timer.Simple(0, function()
            if not IsValid(ent) then return end
            if kind then
                local data = ent:GetSaveTable()
                local owner = ent:GetOwner()
                if not IsValid(owner) or not owner:IsPlayer() then
                    owner = data.m_hThrower
                end
                if kind == Kind.Rocket and (not IsValid(owner) or not owner:IsPlayer()) and IsValid(data.m_hOwner) then
                    owner = data.m_hOwner:GetOwner()
                end
                local session = IsValid(owner) and owner:IsPlayer() and users[owner:UserID()]
                if not session or not session.entity then return end
                -- FIELD_TIME is relative in the Source save table.
                local fuse = data.m_flDetonateTime
                local impact = kind == Kind.ImpactGrenade
                local rocket = kind == Kind.Rocket
                if impact then
                    fuse = data.m_bIsLive == false and math.max(0.001, -(data.m_fSpawnTime or 0)) or 0
                elseif rocket then
                    fuse = ent:GetMoveType() == MOVETYPE_FLY and 0 or math.max(0, 0.3 - engine.TickInterval())
                end
                local radius, damage = data.m_DmgRadius, data.m_flDamage
                local radiusCvar = GetConVar(impact and "sk_smg1_grenade_radius" or "sk_fraggrenade_radius")
                local damageCvar = GetConVar(impact and "sk_plr_dmg_smg1_grenade" or "sk_plr_dmg_fraggrenade")
                if rocket then
                    -- CMissile::EXPLOSION_RADIUS from Source's RPG implementation.
                    radius = 200
                else
                    radius = radius or (radiusCvar and radiusCvar:GetFloat())
                    damage = damage or (damageCvar and damageCvar:GetFloat())
                end
                if not fuse or (kind == Kind.Frag and fuse <= 0) or not radius or radius <= 0 or not damage then return end
                local p, v = ent:GetPos() - origin, ent:GetVelocity()
                local angle = ent:GetAngles()
                if api.GatewayProjectile(session.port, session.token, p.x, p.y, p.z, v.x, v.y, v.z,
                    radius, damage, fuse, kind, angle.p, angle.y) then ent:Remove() end
                return
            end
            if not ent.Explode then return end
            local nativeExplode = ent.Explode
            function ent:Explode(delay, ply)
                if (delay or self:GetDelay()) == 0 then
                    local owner = IsValid(ply) and ply:IsPlayer() and ply or self:GetPlayer()
                    local session = IsValid(owner) and owner:IsPlayer() and users[owner:UserID()]
                    if session and session.entity then
                        local position = self:GetPos() - origin
                        api.GatewayBlast(session.port, session.token, position.x, position.y, position.z,
                            300, math.Clamp(self:GetDamage(), 0, 1500))
                    end
                end
                return nativeExplode(self, delay, ply)
            end
        end)
    end)

    hook.Add("PlayerSpawnedProp", hookName, function(ply, model, prop)
        local session = users[ply:UserID()]
        if not session or not session.entity or not IsValid(prop) then return end
        local p, ang = prop:GetPos() - origin, prop:GetAngles()
        if api.GatewayProp(session.port, session.token, model, p.x, p.y, p.z, ang.p, ang.y) then
            timer.Simple(0, function() if IsValid(prop) then prop:Remove() end end)
        end
    end)

    hook.Add("InitPostEntity", hookName, function()
        local toolgun = weapons.GetStored("gmod_tool")
        local remover = toolgun and toolgun.Tool and toolgun.Tool.remover
        if not remover then return end
        local nativeLeftClick = remover.LeftClick
        function remover:LeftClick(trace)
            local owner = self:GetOwner()
            local session = IsValid(owner) and users[owner:UserID()]
            if session and session.entity and (trace.HitWorld or not IsValid(trace.Entity)) then
                return api.GatewaySend("removeProp", session.port, session.token)
            end
            return nativeLeftClick(self, trace)
        end
    end)

    concommand.Add("atlasnet_marker", function(ply)
        if not IsValid(ply) then return end
        local session = users[ply:UserID()]
        if session and session.entity then api.GatewaySend("marker", session.port, session.token) end
    end)

    concommand.Add("atlasnet_diag", function(ply)
        if not IsValid(ply) then return end
        local s = users[ply:UserID()]
        if s and s.entity then
            ply:PrintMessage(HUD_PRINTCONSOLE,
                ("[AtlasNet] client=%s entity=%s shard=%s region=%d role=session proxy\n")
                    :format(s.client, s.entity, s.shard, s.region))
            local weapon = ply:GetActiveWeapon()
            ply:PrintMessage(HUD_PRINTCONSOLE,
                ("[AtlasNet] vitals health=%d armor=%d alive=%s weapon=%s clip=%d reserve=%d sounds=%d gaits=%d blasts=%d presentations=%d weapons=%d tracers=%d\n")
                    :format(ply:Health(), ply:Armor(), tostring(ply:Alive()),
                        IsValid(weapon) and weapon:GetClass() or "none",
                        IsValid(weapon) and weapon:Clip1() or -1,
                        IsValid(weapon) and ply:GetAmmoCount(weapon:GetPrimaryAmmoType()) or 0,
                        s.sounds or 0, s.gaits or 0, s.blasts or 0, s.presentations or 0,
                        s.weapons or 0, s.tracers or 0))
        end
    end)
end

local function release(session)
    for _, proxy in pairs(session.proxies or {}) do
        if IsValid(proxy) then proxy:Remove() end
    end
    if ports[session.port] == session then ports[session.port] = nil end
    if session.userID and users[session.userID] == session then users[session.userID] = nil end
    api.GatewaySend("disconnect", session.port, session.token)
end

hook.Add("CheckPassword", hookName, function(steamID, address, serverPassword, clientPassword)
    if serverPassword ~= "" and serverPassword ~= clientPassword then
        return false, "#GameUI_ServerRejectBadPassword"
    end
    local token, client = api.GatewayAdmit(address, steamID)
    if not token then
        return false, client
    end
    local port = api.GatewayPort(address)
    local previous = ports[port]
    if not previous or previous.token ~= token then
        if previous then release(previous) end
        ports[port] = { port = port, token = token, client = client, expires = SysTime() + 45 }
    end
end)

gameevent.Listen("player_connect")
hook.Add("player_connect", hookName, function(data)
    local session = ports[api.GatewayPort(data.address or "")]
    if session then
        session.userID = data.userid
        users[data.userid] = session
    end
end)

hook.Add("PlayerInitialSpawn", hookName, function(ply)
    if ply:IsBot() then return end
    local session = ports[api.GatewayPort(ply:IPAddress())]
    if not session or not api.GatewaySend("established", session.port, session.token) then
        ply:Kick("Connect through the AtlasNet ingress address")
        return
    end
    session.player, session.userID = ply, ply:UserID()
    users[session.userID] = session
    if distributed then
        ply:Freeze(true)
        session.proxies = {}
        for _, other in pairs(users) do
            if other ~= session and IsValid(other.player) then
                ply:SetPreventTransmit(other.player, true)
                other.player:SetPreventTransmit(ply, true)
                for _, proxy in pairs(other.proxies or {}) do
                    if IsValid(proxy) then
                        proxy:SetPreventTransmit(ply, true)
                        if IsValid(proxy.AtlasNetWeapon) then
                            proxy.AtlasNetWeapon:SetPreventTransmit(ply, true)
                        end
                    end
                end
            end
        end
    end
    print("[AtlasNet-GMod] Source player associated with AtlasNet client " .. session.client)
end)

gameevent.Listen("player_disconnect")
hook.Add("player_disconnect", hookName, function(data)
    local session = users[data.userid]
    if session then release(session) end
end)

hook.Add("PlayerSay", hookName, function(ply, message, teamChat)
    if message == "" or teamChat or ply:IsBot() then return end
    local session = users[ply:UserID()]
    if session and session.player == ply then
        if not api.GatewaySend("chat", session.port, session.token, message) then
            ply:Kick("AtlasNet gateway is unavailable")
        end
        return ""
    end
end)

hook.Add("Think", hookName, function()
    for _ = 1, 32 do
        local event, port, token, text, instance = api.GatewayPoll()
        if not event then break end
        if event == "lost" then
            for _, session in pairs(ports) do
                release(session)
                if IsValid(session.player) then session.player:Kick("AtlasNet gateway disconnected") end
            end
            break
        end
        local session = ports[port]
        if session and token == session.token then
            local ply = session.player
            if event == "kick" then
                release(session)
                if IsValid(ply) then ply:Kick(text) end
            elseif event == "sound" and distributed and IsValid(ply) then
                local recipients = RecipientFilter()
                recipients:AddPlayer(ply)
                local sound = text
                local position = origin + Vector(sound.x, sound.y, sound.z)
                if sound.tracer then
                    local effect = EffectData()
                    effect:SetStart(position)
                    effect:SetOrigin(origin + Vector(sound.endX, sound.endY, sound.endZ))
                    util.Effect("Tracer", effect, true, recipients)
                    session.tracers = (session.tracers or 0) + 1
                elseif sound.explosion then
                    local effect = EffectData()
                    effect:SetOrigin(position)
                    util.Effect("Explosion", effect, true, recipients)
                    session.blasts = (session.blasts or 0) + 1
                else
                    EmitSound(sound.name, position, 0,
                        sound.channel, sound.volume, sound.level, 0, sound.pitch, sound.dsp, recipients)
                    session.sounds = (session.sounds or 0) + 1
                end
            elseif event == "world" or event == "worldReliable" then
                local delivered = distributed and applyView(session, text)
                if event == "worldReliable" then
                    api.GatewaySend("delivered", port, token, delivered and "world" or "0", instance)
                end
            elseif event == "signal" then
                local delivered = IsValid(ply) and pcall(ply.ChatPrint, ply, "[AtlasNet] " .. text)
                if delivered then
                    print("[AtlasNet-GMod] Chat Signal delivered to AtlasNet client " .. session.client)
                end
                api.GatewaySend("delivered", port, token, delivered and "1" or "0", instance)
            end
        end
    end
    for _, session in pairs(ports) do
        if session.proxies then
            for _, proxy in pairs(session.proxies) do
                local pose = IsValid(proxy) and proxy.AtlasNetPose
                if pose then
                    local fraction = math.Clamp((CurTime() - pose.start) / poseInterval, 0, 1)
                    proxy:SetPos(LerpVector(fraction, pose.from, pose.to))
                    proxy:SetAngles(LerpAngle(fraction, pose.fromAngle, pose.toAngle))
                    if pose.player then
                        proxy.AtlasNetAimPitch = Lerp(fraction, pose.fromPitch, pose.toPitch)
                        applyAim(proxy, proxy.AtlasNetAimPitch)
                    end
                    if fraction == 1 then proxy.AtlasNetPose = nil end
                end
            end
        end
    end
end)

timer.Create(hookName, 1, 0, function()
    local now = SysTime()
    for _, session in pairs(ports) do
        if not session.player and now >= session.expires then release(session) end
        if session.destroyed then
            for id, deadline in pairs(session.destroyed) do
                if now >= deadline then session.destroyed[id] = nil end
            end
        end
    end
end)

if not api.GatewayStart() then
    ErrorNoHalt("[AtlasNet-GMod] Source gateway unavailable; player admission disabled\n")
end
