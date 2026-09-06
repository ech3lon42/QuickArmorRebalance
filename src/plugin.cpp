#include "Config.h"
#include "ConsoleCommands.h"
#include "Data.h"
#include "ImGUIIntegration.h"
#include "UI.h"
#include "LootLists.h"
#include "WarmthHook.h"
#include "ModelArmorSlotFix.h"
#include "ArmorSetBuilder.h"
#include "Enchantments.h"
#include "ModIntegrations.h"
#include "Serialization.h"

namespace QuickArmorRebalance {
    std::mt19937 RNG;

    void OnDataLoaded();
    bool BindPapyrusFunctions(RE::BSScript::IVirtualMachine* vm);
    
    SKSEPluginLoad(const SKSE::LoadInterface* skse) {
        SKSE::Init(skse);
        SKSE::GetPapyrusInterface()->Register(BindPapyrusFunctions);
        SetupLog(REL::Module::GetRuntime() == REL::Module::Runtime::VR ? spdlog::level::trace : spdlog::level::info);

        // Initialize cosave serialization for enhanced armor persistence
        InitializeSerialization();

        ImGuiIntegration::Start(RenderUI);

        SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* message) {
            switch (message->type) {
                case SKSE::MessagingInterface::kDataLoaded:
                    OnDataLoaded();
                    break;
                case SKSE::MessagingInterface::kPostLoadGame:
                case SKSE::MessagingInterface::kNewGame:
                    // Load item tracking for the current character
                    if (auto player = RE::PlayerCharacter::GetSingleton()) {
                        const char* name = player->GetName();
                        if (name && name[0]) {
                            logger::info("Loading item tracking for character: {}", name);
                            LoadItemTracking(name);
                            // Clean up orphaned enchantment effects from dynamic forms that didn't persist
                            // Run cleanup immediately and also with delays to catch effects loaded later
                            logger::info("Running immediate enchantment cleanup...");
                            CleanupOrphanedEnchantments();

                            // Also schedule delayed cleanups in case effects load later
                            logger::info("Scheduling delayed cleanup of orphaned enchantment effects...");
                            SKSE::GetTaskInterface()->AddTask([]() {
                                logger::info("Running delayed enchantment cleanup (pass 1)...");
                                CleanupOrphanedEnchantments();
                            });
                        }
                    }
                    break;
            }
        });

        RNG.seed((unsigned int)std::time(0));

        return true;
    }

    void OnDataLoaded() {
        g_Data.loot = std::make_unique<decltype(g_Data.loot)::element_type>();

        logger::trace("Data loaded - loading files");

        logger::trace("Loading configuration files");
        if (!g_Config.Load()) {
            logger::error("Failed to load configuration files, aborting");
            return;
        }

        logger::trace("Processing Skyrim data");
        ProcessData();

        logger::trace("Loading changes");
        LoadChangesFromFiles();

        logger::trace("Loading favorites");
        LoadFavorites();

        logger::trace("Loading outfits");
        LoadOutfits();

        logger::trace("Loading item tags");
        LoadItemTags();

        logger::trace("Building armor enchantment cache");
        BuildArmorEnchantmentCache();

        ImportFromBOS();

        logger::trace("Setting up loot");
        SetupLootLists();

        std::erase_if(g_Config.mapPrefVariants, [](auto& v) { return !v.second.hash; });

        g_Data.loot.release();

        InstallConsoleCommands();

        if (g_Config.bEnableSkyrimWarmthHook) {
            logger::trace("Installing warmth hook");
            InstallWarmthHooks();
        }

        if (g_Config.bEnableArmorSlotModelFixHook) {
            logger::trace("Installing model slot fix hook");
            InstallModelArmorSlotFixHooks();
        }

        InstallEnchantmentHooks();

        //AnalyzeAllArmor();
    }

    void Papyrus_OpenUI(RE::StaticFunctionTag*) {
        logger::trace("[Papyrus] OpenUI called");
        ImGuiIntegration::Show(true);
        logger::trace("[Papyrus] ImGuiIntegration::Show(true) called");
    }

    bool BindPapyrusFunctions(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("Open", "QuickArmorRebalance", Papyrus_OpenUI);
        return true;
    }
}
