#include "client_ui_module_runtime.h"

#include <string_view>

#include "../../../core/ui/modules/registry/ui_module_registry.h"
#include "../../../core/ui/modules/ui_module_descriptor.h"
#include "../mission_launch/mission_launch_panel.h"
#include "../movement/movement_panel.h"
#include "../player/player_panel.h"
#include "../../../server/ui/spawn/spawn_panel.h"
#include "../../../server/ui/weapon_editor/weapon_editor_panel.h"

namespace sunrise::client::ui::runtime {
namespace {

/** Namespaced stable IDs prevent Client modules from colliding with Server modules. */
constexpr std::string_view kMovementStableId = "client.movement";
constexpr std::string_view kPlayerStableId = "client.player";
constexpr std::string_view kSpawnStableId = "client.spawn";
constexpr std::string_view kWeaponEditorStableId = "client.weapon_editor";
/** Short menu label for the shared teleport and noclip page. */
constexpr std::string_view kMovementDisplayName = "Movement";
/** Short menu label for the player page. */
constexpr std::string_view kPlayerDisplayName = "Player";
/** Short menu label for the entity spawner page. */
constexpr std::string_view kSpawnDisplayName = "Spawn";
/** Short menu label for the gear editor page. */
constexpr std::string_view kWeaponEditorDisplayName = "Gear Editor";

core::ui::modules::registry::PageRegistration g_movementPage;
core::ui::modules::registry::PageRegistration g_playerPage;
core::ui::modules::registry::PageRegistration g_activityLauncherPage;
core::ui::modules::registry::PageRegistration g_spawnPage;
core::ui::modules::registry::PageRegistration g_weaponEditorPage;

} // namespace

/** @return True when the Client modules own their Core UI registry slots. */
bool initialize() noexcept {
    // Registered after movement, which is the order the menu lists them in.
    const bool movementOwned = g_movementPage.acquire(
        core::ui::modules::Owner::client, kMovementStableId, kMovementDisplayName, &movement::draw);
    const bool playerOwned = g_playerPage.acquire(
        core::ui::modules::Owner::client, kPlayerStableId, kPlayerDisplayName, &player::draw);
    const bool launcherOwned = g_activityLauncherPage.acquire(core::ui::modules::Owner::client,
                                                              "client.mission_launch",
                                                              "Activity Launcher",
                                                              &mission_launch::draw);
    const bool spawnOwned = g_spawnPage.acquire(core::ui::modules::Owner::client,
                                                kSpawnStableId,
                                                kSpawnDisplayName,
                                                &server::ui::spawn::draw);
    const bool weaponEditorOwned = g_weaponEditorPage.acquire(core::ui::modules::Owner::client,
                                                              kWeaponEditorStableId,
                                                              kWeaponEditorDisplayName,
                                                              &server::ui::weapon_editor::draw);
    return movementOwned && playerOwned && launcherOwned && spawnOwned && weaponEditorOwned;
}

/** Removes the Client modules from the Core UI registry. */
void shutdown() noexcept {
    g_weaponEditorPage.release();
    g_spawnPage.release();
    g_activityLauncherPage.release();
    g_playerPage.release();
    g_movementPage.release();
}

} // namespace sunrise::client::ui::runtime
