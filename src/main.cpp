#include "ConfigLoader.hpp"
#include "IniSettings.hpp"
#include "MaterialMatcher.hpp"
#include "ProjectedGeometry.hpp"
#include "ProjectedLod.hpp"
#include "ProjectedTextures.hpp"

#include "PCH.h"

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

using namespace XPMF;

namespace {

/**
 * @brief Sets up the global log file for the plugin using spdlog
 */
void setupLog()
{
    // Resolve the SKSE log directory (Documents/My Games/.../SKSE)
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) {
        SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
    }

    // Create a truncating file sink named after the plugin and make it the default logger
    auto logFilePath = *logsFolder / (std::string(PLUGIN_NAME) + ".log");
    auto fileLogger = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto logger = std::make_shared<spdlog::logger>("log", std::move(fileLogger));

    // Log at info and above, and flush per message so crashes don't lose the tail of the log
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
}

} // namespace

//
// CommonLibSSE-NG / SKSE Exports
//

SKSEPluginInfo(.Version = REL::Version {PLUGIN_VERSION_MAJOR,
                                        PLUGIN_VERSION_MINOR,
                                        PLUGIN_VERSION_PATCH,
                                        0},
               .Name = PLUGIN_NAME,
               .Author = "hakasapl",
               .StructCompatibility = SKSE::StructCompatibility::Independent,
               .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

    SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    setupLog();

    const auto version = REL::Module::get().version();
    spdlog::info("{} ({}) {} (built {} {}) loading (runtime {})",
                 PLUGIN_NAME,
                 PLUGIN_DESCRIPTION,
                 PLUGIN_VERSION,
                 __DATE__,
                 __TIME__,
                 version.string("."));

    // Read the configuration once, then put the draw hook in while nothing is loading yet - and
    // before Community Shaders hooks the same lighting shader virtual at kPostPostLoad, so that its
    // hook wraps this plugin's. All hooks are vtable slots, so no trampoline is needed.
    ConfigLoader::loadConfig();
    if (ConfigLoader::isAnyMaterialPatched()) {
        ProjectedTextures::install();
    }
    if (ConfigLoader::isAnyLodMaterialNamed()) {
        ProjectedLod::install(); // a vtable slot too, on the one class object LOD shapes are
    }

    SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* message) -> void {
        if (message == nullptr) {
            return;
        }
        if (message->type == SKSE::MessagingInterface::kPostPostLoad) {
            // The Clone3D hooks go in here, the other way around: after Seasons of Skyrim's
            // (kPostLoad), whose winter snow they have to see on the clone
            ProjectedGeometry::install();
        } else if (message->type == SKSE::MessagingInterface::kDataLoaded) {
            // The two display settings the projection depends on go in first, whatever the INI
            // files say: MaterialMatcher reads one of them
            IniSettings::onDataLoaded();

            // Material objects exist once every plugin has been parsed, and po3's Tweaks - whose
            // EditorID cache tells them apart - is certain to be loaded by then; well before the
            // first cell attaches
            MaterialMatcher::onDataLoaded();
        }
    });

    spdlog::info("{} loaded", PLUGIN_NAME);
    return true;
}
