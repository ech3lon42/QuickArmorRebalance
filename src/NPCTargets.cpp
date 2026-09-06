#include "NPCTargets.h"
#include "NPCOutfitRules.h"

#include "Config.h"
#include "UI.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <fstream>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <Windows.h>
#undef GetObject

namespace QuickArmorRebalance::NPCTargets {
    namespace {
        using Clock = std::chrono::steady_clock;
        constexpr auto kInterval = std::chrono::seconds(1);
        constexpr std::size_t kApplyBudget = 2;

        struct Candidate {
            RE::ActorHandle handle;
            RE::FormID id = 0;
            std::string label;
            float distance = 0;
        };
        struct SavedItem {
            std::string form;
            std::string slot;  // armor, left, right, ammo
            EnhancedItemConfig enhancement;
        };
        struct SavedNPC {
            std::string reference;
            std::string base;
            std::string name;
            std::vector<SavedItem> items;
        };
        struct Session {
            RE::ActorHandle handle;
            std::vector<std::pair<int, RE::FormID>> given;
            std::vector<RE::FormID> stripped;
            bool edited = false;
            int equipFrame = -1;
            std::uint32_t equipSlots = 0;
        };

        std::atomic_bool ready = false, detection = false, persistence = false, queued = false, visible = false, saveRequested = false;
        std::atomic_uint64_t epoch = 0, revision = 0, worldGeneration = 0;
        std::atomic_int trackedCount = 0;
        std::atomic_int detectionRange = Rules::defaultRadius;
        std::mutex uiMutex;
        std::vector<Candidate> candidates;
        RE::ActorHandle selected;
        bool npcSelected = false;  // Invalid selection stays an NPC, never silently falls back to player.
        std::string selectedLabel, status;
        thread_local RE::NiPointer<RE::Actor> frameActor;
        thread_local bool frameNPC = false;
        Clock::time_point lastTick{};  // Render thread only.
        bool rangeDirty = false;  // Render thread only, including Tick when the UI is closed.

        // The following state is game-thread-only, except recipes which also have UI producers.
        std::unordered_map<std::string, SavedNPC> saved;
        std::unordered_map<RE::FormID, Session> sessions;
        std::size_t applyCursor = 0;
        struct PendingSave { RE::ActorHandle handle; std::uint64_t epoch; Clock::time_point after; };
        std::optional<PendingSave> pendingSave;
        std::mutex recipeMutex;
        std::unordered_map<RE::FormID, SavedItem> recipes;

        void Status(std::string message) {
            std::scoped_lock lock(uiMutex);
            status = std::move(message);
        }

        bool Valid(RE::Actor* actor) {
            auto player = RE::PlayerCharacter::GetSingleton();
            if (!ready || !actor || !player || actor == player || actor->IsDeleted() || actor->IsDisabled() ||
                actor->IsDead() || actor->IsInKillMove() || !actor->Is3DLoaded() || !actor->GetActorRuntimeData().currentProcess ||
                !actor->GetActorBase() || !actor->HasKeywordString("ActorTypeNPC")) return false;
            auto cell = actor->GetParentCell();
            auto playerCell = player->GetParentCell();
            if (!cell || !playerCell) return false;
            auto world = actor->GetWorldspace();
            auto playerWorld = player->GetWorldspace();
            return Rules::SameSpace(cell->IsInteriorCell(), playerCell->IsInteriorCell(), cell->GetFormID(), playerCell->GetFormID(),
                                    world ? world->GetFormID() : 0, playerWorld ? playerWorld->GetFormID() : 0) &&
                   Rules::InRange(actor->GetPosition().GetSquaredDistance(player->GetPosition()), detectionRange.load());
        }

        bool Stable(RE::TESForm* form) {
            return form && !form->IsDynamicForm() && form->GetFile(0);
        }

        const char* DisplayName(RE::Actor* actor) {
            const auto name = actor ? actor->GetName() : nullptr;
            return name && name[0] ? name : "<Unnamed NPC>";
        }

        bool Worn(RE::Actor* actor, RE::TESBoundObject* item) {
            if (auto armor = item->As<RE::TESObjectARMO>()) {
                for (unsigned slot = 0; slot < 32; ++slot) {
                    if (actor->GetWornArmor(static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << slot)) == armor) return true;
                }
            }
            return actor->GetEquippedObject(false) == item || actor->GetEquippedObject(true) == item || GetEquippedAmmo(actor) == item;
        }

        std::vector<RE::TESObjectARMO*> WornArmor(RE::Actor* actor) {
            std::vector<RE::TESObjectARMO*> result;
            for (unsigned slot = 0; slot < 32; ++slot) {
                auto armor = actor->GetWornArmor(static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << slot));
                if (armor && std::find(result.begin(), result.end(), armor) == result.end()) result.push_back(armor);
            }
            return result;
        }

        std::filesystem::path Directory() { return std::filesystem::current_path() / PATH_ROOT "npc_outfits"; }

        std::filesystem::path FileFor(const std::string& key) {
            return Directory() / Rules::FileName(key);
        }

        bool Describe(RE::TESBoundObject* item, const char* slot, SavedItem& result) {
            result.slot = slot;
            {
                std::scoped_lock lock(recipeMutex);
                if (auto found = recipes.find(item->GetFormID()); found != recipes.end()) {
                    result.form = found->second.form;
                    result.enhancement = found->second.enhancement;
                    return true;
                }
            }
            if (!Stable(item)) return false;
            result.form = QARFormID(item);
            return true;
        }

        bool WriteSaved(const SavedNPC& npc) {
            using rapidjson::Value;
            rapidjson::Document doc;
            doc.SetObject();
            auto& al = doc.GetAllocator();
            doc.AddMember("version", 1, al);
            doc.AddMember("reference", Value(npc.reference.c_str(), al), al);
            doc.AddMember("base", Value(npc.base.c_str(), al), al);
            doc.AddMember("name", Value(npc.name.c_str(), al), al);
            Value list(rapidjson::kArrayType);
            for (const auto& item : npc.items) {
                Value v(rapidjson::kObjectType);
                v.AddMember("form", Value(item.form.c_str(), al), al);
                v.AddMember("slot", Value(item.slot.c_str(), al), al);
                const auto& e = item.enhancement;
                v.AddMember("enchantment", Value(e.enchantmentFormID.c_str(), al), al);
                v.AddMember("magnitude", e.enchantmentMagnitude, al);
                v.AddMember("statsSource", Value(e.statsSourceFormID.c_str(), al), al);
                if (e.armorRating) v.AddMember("armorRating", *e.armorRating, al);
                if (e.weight) v.AddMember("weight", *e.weight, al);
                if (e.value) v.AddMember("value", *e.value, al);
                list.PushBack(v, al);
            }
            doc.AddMember("items", list, al);
            std::filesystem::create_directories(Directory());
            auto path = FileFor(npc.reference);
            // Defend even against a filename hash collision: never overwrite another reference.
            if (std::filesystem::exists(path)) {
                rapidjson::Document old;
                if (!ReadJSONFile(path, old, false) || !old.IsObject() || !old.HasMember("reference") ||
                    !old["reference"].IsString() || npc.reference != old["reference"].GetString()) return false;
            }
            auto temporary = path;
            temporary += ".tmp";
            rapidjson::StringBuffer buffer;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
            if (!doc.Accept(writer)) return false;
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
            stream.flush();
            if (!stream) return false;
            stream.close();
            if (stream.fail()) return false;
            return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        }

        void LoadSaved() {
            saved.clear();
            if (!std::filesystem::exists(Directory())) return;
            for (const auto& file : std::filesystem::directory_iterator(Directory())) {
                if (!file.is_regular_file() || file.path().extension() != ".json" || file.file_size() > 128 * 1024) continue;
                rapidjson::Document doc;
                if (!ReadJSONFile(file.path(), doc, false) || !doc.IsObject()) continue;
                if (!doc.HasMember("version") || !doc["version"].IsInt() || doc["version"].GetInt() != 1 ||
                    !doc.HasMember("reference") || !doc["reference"].IsString() ||
                    !doc.HasMember("base") || !doc["base"].IsString() ||
                    !doc.HasMember("items") || !doc["items"].IsArray() || doc["items"].Size() > 35) {
                    logger::warn("NPC outfits: invalid file {}", file.path().string());
                    continue;
                }
                SavedNPC npc;
                npc.reference = doc["reference"].GetString();
                npc.base = doc["base"].GetString();
                if (doc.HasMember("name") && doc["name"].IsString()) npc.name = doc["name"].GetString();
                bool valid = true;
                for (auto& v : doc["items"].GetArray()) {
                    if (!v.IsObject() || !v.HasMember("form") || !v["form"].IsString() ||
                        !v.HasMember("slot") || !v["slot"].IsString()) { valid = false; break; }
                    SavedItem item;
                    item.form = v["form"].GetString();
                    item.slot = v["slot"].GetString();
                    if (item.slot != "armor" && item.slot != "left" && item.slot != "right" && item.slot != "ammo") { valid = false; break; }
                    auto& e = item.enhancement;
                    // Reject malformed optional fields, rather than partially applying a damaged file.
                    for (auto field : {"enchantment", "statsSource"}) {
                        if (v.HasMember(field) && !v[field].IsString()) valid = false;
                    }
                    if (!valid) break;
                    if (v.HasMember("enchantment")) e.enchantmentFormID = v["enchantment"].GetString();
                    if (v.HasMember("statsSource")) e.statsSourceFormID = v["statsSource"].GetString();
                    if (v.HasMember("magnitude")) {
                        if (!v["magnitude"].IsNumber()) { valid = false; break; }
                        e.enchantmentMagnitude = v["magnitude"].GetFloat();
                        if (!std::isfinite(e.enchantmentMagnitude) || e.enchantmentMagnitude < 0) { valid = false; break; }
                    }
                    if (v.HasMember("armorRating")) {
                        if (!v["armorRating"].IsUint()) { valid = false; break; }
                        e.armorRating = v["armorRating"].GetUint();
                    }
                    if (v.HasMember("weight")) {
                        if (!v["weight"].IsNumber() || !std::isfinite(v["weight"].GetFloat()) || v["weight"].GetFloat() < 0) { valid = false; break; }
                        e.weight = v["weight"].GetFloat();
                    }
                    if (v.HasMember("value")) {
                        if (!v["value"].IsInt()) { valid = false; break; }
                        e.value = v["value"].GetInt();
                    }
                    npc.items.push_back(std::move(item));
                }
                if (valid) saved[npc.reference] = std::move(npc);
                else logger::warn("NPC outfits: malformed items in {}", file.path().string());
            }
            logger::info("NPC outfits: loaded {} shared NPC outfits", saved.size());
        }

        void SaveActor(RE::Actor* actor) {
            if (!Stable(actor) || !Stable(actor->GetActorBase())) {
                Status("Temporary NPCs can be dressed, but cannot be saved across characters.");
                return;
            }
            SavedNPC npc{QARFormID(actor), QARFormID(actor->GetActorBase()), DisplayName(actor), {}};
            auto append = [&](RE::TESBoundObject* item, const char* slot) {
                SavedItem record;
                if (!Describe(item, slot, record)) return false;
                npc.items.push_back(std::move(record));
                return true;
            };
            bool valid = true;
            for (auto armor : WornArmor(actor)) valid &= append(armor, "armor");
            for (bool left : {false, true}) {
                auto form = actor->GetEquippedObject(left);
                if (auto weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr) {
                    // A two-handed weapon appearing in both hands is still one inventory item.
                    if (left && (weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow())) continue;
                    valid &= append(weapon, left ? "left" : "right");
                }
            }
            if (auto ammo = GetEquippedAmmo(actor)) valid &= append(ammo, "ammo");
            if (!valid) {
                Status("Cannot save: an equipped dynamic item has no recoverable base recipe. Existing save unchanged.");
                return;
            }
            if (!WriteSaved(npc)) {
                logger::error("NPC outfits: failed to save {} to {}", npc.reference, FileFor(npc.reference).string());
                Status("NPC outfit could not be written. Existing save unchanged; check the log.");
                return;
            }
            saved[npc.reference] = std::move(npc);
            sessions[actor->GetFormID()].edited = false;
            Status("Saved NPC outfit (shared across characters). Automatic application requires persistence enabled.");
        }

        RE::BGSEquipSlot* EquipSlot(const SavedItem& item) {
            auto defaults = RE::BGSDefaultObjectManager::GetSingleton();
            if (!defaults) return nullptr;
            if (item.slot == "left") return defaults->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kLeftHandEquip);
            if (item.slot == "right") return defaults->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kRightHandEquip);
            return nullptr;
        }

        void ApplySaved(RE::Actor* actor, const SavedNPC& npc) {
            if (QARFormID(actor->GetActorBase()) != npc.base || actor->IsInCombat()) return;
            auto manager = RE::ActorEquipManager::GetSingleton();
            if (!manager) return;
            std::vector<std::pair<const SavedItem*, RE::TESBoundObject*>> resolved;
            // Resolve everything BEFORE touching the actor. Missing mods must never strip an NPC.
            for (const auto& item : npc.items) {
                auto form = LookupForm<RE::TESBoundObject>(item.form);
                if (!Stable(form)) return;
                if ((item.slot == "armor" && !form->As<RE::TESObjectARMO>()) ||
                    ((item.slot == "left" || item.slot == "right") && !form->As<RE::TESObjectWEAP>()) ||
                    (item.slot == "ammo" && !form->As<RE::TESAmmo>())) return;
                const auto& e = item.enhancement;
                if (e.HasEnchantment() && !Stable(LookupForm<RE::EnchantmentItem>(e.enchantmentFormID))) return;
                if (e.IsEnhanced()) {
                    auto armor = form->As<RE::TESObjectARMO>();
                    if (!armor) return;
                    form = CreateEnhancedArmor(armor, e, false);
                    if (!form) return;
                }
                resolved.emplace_back(&item, form);
            }
            // Armor is persistent; weapon hands are best-effort because NPC combat AI owns them.
            // A lost dynamic armor may leave a source-less enchantment effect in a game save.
            // Only clean recipes belonging to this NPC, and never dispel effects with a live source
            // or an enchantment still supplied by worn armor.
            std::unordered_set<RE::FormID> orphanEnchantments;
            auto wornArmor = WornArmor(actor);
            for (const auto& [item, form] : resolved) {
                if (!item->enhancement.HasEnchantment()) continue;
                auto enchantment = LookupForm<RE::EnchantmentItem>(item->enhancement.enchantmentFormID);
                if (enchantment && std::none_of(wornArmor.begin(), wornArmor.end(), [enchantment](auto armor) { return armor->formEnchanting == enchantment; })) {
                    orphanEnchantments.insert(enchantment->GetFormID());
                }
            }
            if (!orphanEnchantments.empty()) {
                auto magic = actor->AsMagicTarget();
                auto effects = magic ? magic->GetActiveEffectList() : nullptr;
                std::vector<RE::ActiveEffect*> toDispel;
                if (effects) for (auto effect : *effects) {
                    if (effect && !effect->source && effect->spell && orphanEnchantments.contains(effect->spell->GetFormID())) toDispel.push_back(effect);
                }
                for (auto effect : toDispel) effect->Dispel(true);
            }
            for (auto armor : wornArmor) {
                if (std::none_of(resolved.begin(), resolved.end(), [armor](auto& item) { return item.second == armor; })) {
                    manager->UnequipObject(actor, armor, nullptr, 1, armor->GetEquipSlot(), false, false, false);
                }
            }
            for (bool left : {false, true}) {
                auto held = actor->GetEquippedObject(left);
                auto weapon = held ? held->As<RE::TESObjectWEAP>() : nullptr;
                if (weapon && std::none_of(resolved.begin(), resolved.end(), [weapon](auto& entry) { return entry.second == weapon; })) {
                    manager->UnequipObject(actor, weapon, nullptr, 1, nullptr, false, false, false);
                }
            }
            if (auto ammo = GetEquippedAmmo(actor); ammo && std::none_of(resolved.begin(), resolved.end(), [ammo](auto& entry) { return entry.second == ammo; })) {
                manager->UnequipObject(actor, ammo, nullptr, 1, nullptr, false, false, false);
            }
            std::unordered_map<RE::TESBoundObject*, int> counts;
            for (const auto& [item, form] : resolved) {
                int needed = ++counts[form];
                if (ItemCount(actor, form) < needed) actor->AddObjectToContainer(form, nullptr, needed - ItemCount(actor, form), nullptr);
                bool worn = item->slot == "left" ? actor->GetEquippedObject(true) == form :
                            item->slot == "right" ? actor->GetEquippedObject(false) == form : Worn(actor, form);
                if (!worn) manager->EquipObject(actor, form, nullptr, 1, EquipSlot(*item), false, false, false);
            }
        }

        void Scan() {
            auto lists = RE::ProcessLists::GetSingleton();
            auto player = RE::PlayerCharacter::GetSingleton();
            auto ui = RE::UI::GetSingleton();
            if (!lists || !player || !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) return;
            std::vector<Candidate> found;
            // Only high-process loaded actors, not every reference/form in the game.
            for (const auto& handle : lists->highActorHandles) {
                auto actor = handle.get();
                if (!Valid(actor.get())) continue;
                float distance = std::sqrt(actor->GetPosition().GetSquaredDistance(player->GetPosition()));
                found.push_back({handle, actor->GetFormID(), std::format("{} [{:08X}]", DisplayName(actor.get()), actor->GetFormID()), distance});
            }
            std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.distance != b.distance ? a.distance < b.distance : a.id < b.id; });
            RE::ActorHandle target;
            {
                std::scoped_lock lock(uiMutex);
                candidates = found;
                target = selected;
            }
            // Session tracking is never allowed to attach to a recycled handle or a different save.
            std::erase_if(sessions, [&](auto& session) {
                return std::none_of(found.begin(), found.end(), [&](auto& c) { return c.id == session.first && c.handle == session.second.handle; });
            });
            if (pendingSave && Clock::now() >= pendingSave->after) {
                auto pending = *pendingSave;
                pendingSave.reset();
                saveRequested = false;
                auto actor = pending.handle.get();
                if (pending.epoch == epoch && Valid(actor.get())) SaveActor(actor.get());
                else Status("Save canceled: NPC is no longer available.");
            }
            auto selectedSession = std::find_if(sessions.begin(), sessions.end(), [&](auto& s) { return s.second.handle == target; });
            trackedCount = selectedSession == sessions.end() ? 0 : static_cast<int>(selectedSession->second.given.size());
            if (!persistence || ui->IsItemMenuOpen() || found.empty()) return;
            std::size_t applied = 0;
            for (std::size_t i = 0; i < found.size() && applied < kApplyBudget; ++i) {
                auto& candidate = found[applyCursor++ % found.size()];
                if (visible && candidate.handle == target) continue;
                auto actor = candidate.handle.get();
                if (!Valid(actor.get()) || !Stable(actor.get())) continue;
                auto session = sessions.find(candidate.id);
                if (session != sessions.end() && session->second.edited) continue;
                auto outfit = saved.find(QARFormID(actor.get()));
                if (outfit == saved.end()) continue;
                ++applied;
                ApplySaved(actor.get(), outfit->second);
            }
        }

        void RefreshFrame() {
            RE::ActorHandle handle;
            {
                std::scoped_lock lock(uiMutex);
                frameNPC = npcSelected;
                handle = selected;
            }
            frameActor.reset();
            if (!ready) return;
            if (frameNPC) {
                if (!detection) return;
                auto actor = handle.get();
                if (Valid(actor.get())) frameActor = std::move(actor);
            } else {
                frameActor.reset(RE::PlayerCharacter::GetSingleton());
            }
        }
    }

    void RegisterEnhanced(RE::TESObjectARMO* enhanced, RE::TESObjectARMO* base, const EnhancedItemConfig& config) {
        if (!enhanced || !Stable(base)) return;
        std::scoped_lock lock(recipeMutex);
        recipes[enhanced->GetFormID()] = {QARFormID(base), "armor", config};
    }

    void Initialize() {
        detectionRange = Rules::ClampRadius(g_Config.npcDetectionRange);
        detection = g_Config.bNPCDetection;
        persistence = g_Config.bNPCDetection && g_Config.bNPCPersistence;
        try { LoadSaved(); } catch (const std::exception& e) { logger::error("NPC outfits: {}", e.what()); }
    }

    void Suspend() {
        ready = false;
        saveRequested = false;
        {
            std::scoped_lock lock(recipeMutex);
            recipes.clear();
        }
        ++epoch;
        ++worldGeneration;
        ++revision;
        trackedCount = 0;
        std::scoped_lock lock(uiMutex);
        selected.reset();
        npcSelected = false;
        candidates.clear();
        status.clear();
    }

    void Resume() {
        sessions.clear();
        pendingSave.reset();
        applyCursor = 0;
        saveRequested = false;
        queued = false;  // A game load can discard tasks queued before it started.
        ready = true;
        ++revision;
    }

    void Tick(bool uiOpen) {
        visible = uiOpen;
        // Closing QAR during a drag can skip ImGui's deactivation callback.
        if (!uiOpen && rangeDirty) {
            g_Config.Save();
            rangeDirty = false;
        }
        if (!ready || !detection || (!uiOpen && !persistence && !saveRequested)) return;
        auto now = Clock::now();
        if (now - lastTick < kInterval || queued.exchange(true)) return;
        lastTick = now;
        const auto currentEpoch = epoch.load();
        SKSE::GetTaskInterface()->AddTask([currentEpoch]() {
            try {
                if (ready && detection && currentEpoch == epoch) Scan();
            } catch (const std::exception& e) {
                logger::error("NPC outfits: {}", e.what());
                Status("NPC operation failed; see the log.");
            }
            queued = false;
        });
    }

    FrameTarget::FrameTarget() { RefreshFrame(); }
    FrameTarget::~FrameTarget() { frameActor.reset(); }
    RE::Actor* GetActor() { return frameActor.get(); }
    bool IsNPC() { return frameNPC; }
    std::uint64_t Revision() { return revision.load(); }
    std::uint64_t Generation() { return worldGeneration.load(); }
    int TrackedCount() { return frameNPC ? trackedCount.load() : 0; }

    int ItemCount(RE::Actor* actor, RE::TESBoundObject* item) {
        if (!actor || !item) return 0;
        if (actor == RE::PlayerCharacter::GetSingleton()) return RE::PlayerCharacter::GetSingleton()->GetItemCount(item);
        // Do not call GetInventory (hooked by some inventory mods) or initialize inventory on the render thread.
        if (auto changes = actor->GetInventoryChanges(true)) {
            return std::max(0, changes->GetCount(item, [](const RE::InventoryEntryData*) { return true; }));
        }
        auto container = actor->GetContainer();
        return container ? std::max(0, container->GetObjectCount(item)) : 0;
    }

    std::vector<RE::TESObjectARMO*> InventoryArmor(RE::Actor* actor) {
        std::set<RE::TESObjectARMO*> unique;
        if (!actor) return {};
        auto add = [&](RE::TESBoundObject* object) {
            if (auto armor = object ? object->As<RE::TESObjectARMO>() : nullptr) unique.insert(armor);
        };
        if (auto container = actor->GetContainer()) {
            container->ForEachContainerObject([&](RE::ContainerObject& entry) { add(entry.obj); return RE::BSContainer::ForEachResult::kContinue; });
        }
        if (auto changes = actor->GetInventoryChanges(true); changes && changes->entryList) {
            for (auto entry : *changes->entryList) if (entry) add(entry->object);
        }
        std::vector<RE::TESObjectARMO*> result;
        for (auto armor : unique) if (ItemCount(actor, armor) > 0) result.push_back(armor);
        return result;
    }

    void Request(Action action, RE::TESBoundObject* item, bool equip, bool reuse, const EnhancedItemConfig* enhancement) {
        RE::ActorHandle target;
        {
            std::scoped_lock lock(uiMutex);
            if (!npcSelected || !detection || !ready) return;
            target = selected;
        }
        const auto currentEpoch = epoch.load();
        const auto id = item ? item->GetFormID() : 0;
        const auto config = enhancement ? *enhancement : EnhancedItemConfig{};
        const auto frame = ImGui::GetFrameCount();
        SKSE::GetTaskInterface()->AddTask([=]() {
            if (currentEpoch != epoch || !ready || !detection) return;
            auto actor = target.get();
            auto manager = RE::ActorEquipManager::GetSingleton();
            auto ui = RE::UI::GetSingleton();
            if (!Valid(actor.get()) || !manager || !ui || ui->IsItemMenuOpen()) {
                Status("Action canceled: NPC unavailable or an inventory menu is open.");
                return;
            }
            auto& session = sessions[actor->GetFormID()];
            if (session.handle != target) session = Session{target};
            session.edited = true;
            auto object = id ? RE::TESForm::LookupByID<RE::TESBoundObject>(id) : nullptr;
            if (config.IsEnhanced() && object) {
                if (auto armor = object->As<RE::TESObjectARMO>()) object = CreateEnhancedArmor(armor, config, false);
            }
            auto equipObject = [&](RE::TESBoundObject* form) {
                if (session.equipFrame != frame) { session.equipFrame = frame; session.equipSlots = 0; }
                RE::BGSEquipSlot* slot = nullptr;
                if (auto armor = form->As<RE::TESObjectARMO>()) {
                    auto slots = armor->GetSlotMask().underlying();
                    if ((slots & session.equipSlots) != 0) return;
                    session.equipSlots |= slots;
                    slot = armor->GetEquipSlot();
                }
                manager->EquipObject(actor.get(), form, nullptr, 1, slot, false, false, false);
            };
            auto give = [&](RE::TESBoundObject* form, bool reuseExisting) {
                if (!reuseExisting || ItemCount(actor.get(), form) == 0) {
                    actor->AddObjectToContainer(form, nullptr, 1, nullptr);
                    session.given.emplace_back(frame, form->GetFormID());
                }
            };
            auto remove = [&](RE::FormID formID) {
                auto form = RE::TESForm::LookupByID<RE::TESBoundObject>(formID);
                if (!form || ItemCount(actor.get(), form) < 1) return;
                // RemoveItem handles worn inventory entries itself; do not leave a deferred
                // equip/unequip request pointing at a just-deleted inventory entry.
                actor->RemoveItem(form, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            };
            switch (action) {
                case Action::Give:
                    if (object) { give(object, reuse); if (equip) equipObject(object); }
                    break;
                case Action::Equip:
                    if (object && ItemCount(actor.get(), object) > 0) equipObject(object);
                    break;
                case Action::Unequip:
                    if (object) manager->UnequipObject(actor.get(), object, nullptr, 1, nullptr, false, false, false);
                    break;
                case Action::Toggle:
                    if (object) {
                        if (Worn(actor.get(), object)) manager->UnequipObject(actor.get(), object, nullptr, 1, nullptr, false, false, false);
                        else { give(object, true); equipObject(object); }
                    }
                    break;
                case Action::UnequipAll:
                    session.stripped.clear();
                    session.equipSlots = 0;
                    for (auto armor : WornArmor(actor.get())) {
                        session.stripped.push_back(armor->GetFormID());
                        manager->UnequipObject(actor.get(), armor, nullptr, 1, armor->GetEquipSlot(), false, false, false);
                    }
                    break;
                case Action::Restore:
                    session.equipSlots = 0;
                    for (auto formID : session.stripped) {
                        auto form = RE::TESForm::LookupByID<RE::TESBoundObject>(formID);
                        if (form && ItemCount(actor.get(), form) > 0) equipObject(form);
                    }
                    session.stripped.clear();
                    break;
                case Action::Remove:
                    if (auto it = std::find_if(session.given.begin(), session.given.end(), [id](auto& p) { return p.second == id; }); it != session.given.end()) {
                        remove(id);
                        session.given.erase(it);
                    }
                    break;
                case Action::Undo:
                    if (!session.given.empty()) {
                        auto lastFrame = session.given.back().first;
                        while (!session.given.empty() && session.given.back().first == lastFrame) {
                            remove(session.given.back().second);
                            session.given.pop_back();
                        }
                    }
                    break;
                case Action::RemoveAll:
                    for (auto& entry : session.given) remove(entry.second);
                    session.given.clear();
                    break;
            }
            {
                std::scoped_lock lock(uiMutex);
                if (selected == target) trackedCount = static_cast<int>(session.given.size());
            }
            ++revision;
        });
    }

    void DrawControls() {
        bool detect = detection, persist = persistence;
        if (ImGui::Checkbox("NPC Detection", &detect)) {
            detection = detect;
            g_Config.bNPCDetection = detect;
            if (!detect) {
                persistence = false;
                persist = false;
                saveRequested = false;
                g_Config.bNPCPersistence = false;
                ++epoch;  // Cancel pending actions, even if detection is immediately re-enabled.
                std::scoped_lock lock(uiMutex);
                npcSelected = false;
                selected.reset();
                candidates.clear();
                status.clear();
            }
            ++revision;
            g_Config.Save();
            RefreshFrame();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!detect);
        ImGui::SetNextItemWidth(300.0f);
        std::vector<Candidate> list;
        std::string label;
        RE::ActorHandle target;
        {
            std::scoped_lock lock(uiMutex);
            label = npcSelected ? selectedLabel : "Player";
            target = selected;
        }
        if (frameNPC && !frameActor) label += " (unavailable)";
        auto select = [&](const RE::ActorHandle& handle, const std::string& text, bool npc) {
            {
                std::scoped_lock lock(uiMutex);
                selected = handle;
                selectedLabel = text;
                npcSelected = npc;
                status.clear();
            }
            trackedCount = 0;
            ++revision;
            RefreshFrame();
        };
        if (ImGui::BeginCombo("Target", label.c_str())) {
            {
                std::scoped_lock lock(uiMutex);
                list = candidates;
            }
            if (ImGui::Selectable("Player", !frameNPC)) select({}, "Player", false);
            for (auto& candidate : list) {
                auto text = std::format("{}  ({:.0f} units)", candidate.label, candidate.distance);
                if (ImGui::Selectable(text.c_str(), frameNPC && target == candidate.handle)) select(candidate.handle, candidate.label, true);
            }
            if (list.empty()) ImGui::TextDisabled("No loaded living NPCs within %d units.", detectionRange.load());
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Outfit Persistence", &persist)) {
            persistence = persist;
            g_Config.bNPCPersistence = persist;
            g_Config.Save();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reapply saved NPC outfits nearby, including while this UI is closed.\nShared across characters. Combat actors are skipped. Unsaved edits are left alone until the NPC leaves range.");
        ImGui::SameLine();
        ImGui::BeginDisabled(saveRequested || !frameNPC || !frameActor || !Stable(frameActor.get()));
        if (ImGui::Button("Save NPC")) {
            auto handle = frameActor->GetHandle();
            auto currentEpoch = epoch.load();
            Status("Saving after queued equipment actions finish...");
            saveRequested = true;
            SKSE::GetTaskInterface()->AddTask([handle, currentEpoch]() {
                if (currentEpoch == epoch && ready && detection) pendingSave = PendingSave{handle, currentEpoch, Clock::now() + std::chrono::milliseconds(500)};
            });
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Save the selected NPC's currently equipped armor, weapons and ammo.\nRequires a stable placed NPC reference; spawned/temporary NPCs cannot be shared across characters.\nPer-instance tempering and custom player enchantments are not copied.");
        ImGui::EndDisabled();

        // A separate row keeps the controls usable in narrower windows. Range can be
        // configured while detection is off; changing it does not enable detection.
        auto range = detectionRange.load();
        auto setRange = [&](int value) {
            value = Rules::ClampRadius(value);
            detectionRange = value;
            g_Config.npcDetectionRange = value;
            rangeDirty = true;
            {
                std::scoped_lock lock(uiMutex);
                std::erase_if(candidates, [value](const Candidate& candidate) {
                    return !Rules::InRange(candidate.distance * candidate.distance, value);
                });
            }
            ++revision;
            RefreshFrame();  // Immediately block a selected NPC now outside the range.
        };
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::SliderInt("Detection Range", &range, Rules::minRadius, Rules::maxRadius, "%d units", ImGuiSliderFlags_AlwaysClamp)) {
            setRange(range);
        }
        // Avoid writing the config file every frame while the slider is dragged.
        if (ImGui::IsItemDeactivatedAfterEdit() && rangeDirty) {
            g_Config.Save();
            rangeDirty = false;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Distance from the player in Skyrim game units. Default: %d.\nApplies to selection, queued NPC actions and outfit persistence.\nThe nearby list refreshes within one second. Only loaded NPCs can be detected, even at larger ranges.", Rules::defaultRadius);
        ImGui::SameLine();
        if (ImGui::Button("Reset Range")) {
            setRange(Rules::defaultRadius);
            g_Config.Save();
            rangeDirty = false;
        }
        {
            std::scoped_lock lock(uiMutex);
            if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
        }
        if (frameNPC && !frameActor) ImGui::TextDisabled("Selected NPC is unavailable. Actions are blocked; select Player or another nearby NPC.");
        ImGui::Separator();
    }
}
