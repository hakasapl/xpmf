#include "IniSettings.hpp"

#include "PCH.h"

using namespace XPMF;

void IniSettings::onDataLoaded()
{
    for (const Hold& hold : K_HOLDS) {
        auto* const setting = RE::GetINISetting(hold.name);
        if (setting == nullptr || setting->GetType() != RE::Setting::Type::kBool) {
            spdlog::warn("Display setting {} was not found as a boolean, so it stays as the game has it; {}",
                         hold.name,
                         hold.reason);
            continue;
        }
        if (setting->GetBool() == hold.value) {
            spdlog::info("Display setting {} is {} already", hold.name, hold.value);
            continue;
        }
        setting->SetBool(hold.value);
        spdlog::info("Display setting {} changed from {} to {} for this session: {}",
                     hold.name,
                     !hold.value,
                     hold.value,
                     hold.reason);
    }
}
