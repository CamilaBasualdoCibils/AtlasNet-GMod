local function hasDiagnostics(module)
    return type(module) == "table" and type(module.Version) == "function"
        and type(module.IsInitialized) == "function"
end

if hasDiagnostics(atlasnet) and atlasnet.IsInitialized() then
    return
end

local ok, loadError = pcall(require, "atlasnet")
if not ok then
    ErrorNoHalt("[AtlasNet-GMod] Could not load atlasnet: " .. tostring(loadError) .. "\n")
    return
end

if not hasDiagnostics(atlasnet) then
    ErrorNoHalt("[AtlasNet-GMod] require succeeded without publishing the native diagnostics\n")
    return
end

print(("[AtlasNet-GMod] %s | initialized=%s"):format(
    atlasnet.Version(), tostring(atlasnet.IsInitialized())))
