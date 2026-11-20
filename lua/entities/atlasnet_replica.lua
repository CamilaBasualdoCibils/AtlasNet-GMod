if SERVER then AddCSLuaFile() end

ENT.Type = "anim"
ENT.Base = "base_anim"
ENT.AutomaticFrameAdvance = true

function ENT:Think()
    if SERVER then
        self:NextThink(CurTime())
        return true
    end
end
