#pragma once

#include <AtlasNet/Core/CmdSig/Command.hpp>
#include <AtlasNet/Core/CmdSig/Signal.hpp>

namespace AtlasNetGMod
{
inline constexpr auto ChatCommand = AtlasNet::CmdSig::CommandNameToID("AtlasNetGMod.Chat");
// The Signal ID is an application-registered V1 ID, like the Command ID.
inline constexpr AtlasNet::CmdSig::SignalID ChatSignal =
    AtlasNet::CmdSig::CommandNameToID("AtlasNetGMod.ChatReply");
}
