#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::hooks::teleport {

/** Three floats make one position or velocity vector. */
inline constexpr std::size_t kVectorLanes = 3;
/** The vertical lane. The camera basis is X forward, Z up, so the third lane is up. */
inline constexpr std::size_t kVerticalLane = 2;

/** One world-space vector as the game stores it. */
using Vector = std::array<float, kVectorLanes>;

/** Read-only camera pose published by the existing camera-frame hook. */
struct CameraPose final {
    Vector position{};
    Vector forward{};
    Vector up{};
    float horizontalFov{};
    float aspect{};
};

/** Writes the local player's controlled-object handle, or the invalid sentinel. */
using ControlledHandle = std::uint32_t* (*)(std::uint32_t*);
/** Returns the camera pose block array. The pointer in its global is obfuscated, so we call it. */
using CameraSingleton = std::byte* (*)();

/**
 * Publishes the two functions the hooks call.
 * @param controlled Writes the local player's object handle.
 * @param singleton Returns the camera pose block array.
 */
void publish_targets(ControlledHandle controlled, CameraSingleton singleton) noexcept;

/** Drops those functions and every latched request. */
void clear_targets() noexcept;

/**
 * Reports whether one candidate object is the one the local player controls.
 * @param object Candidate object base pointer. Null is never the local player.
 * @return True when the object's handle matches the controlled-object handle.
 */
[[nodiscard]] bool is_controlled_object(const void* object) noexcept;

/**
 * Reads the local player's current world position.
 * @param output Receives three world-space lanes when the player and body were found.
 * @return True when the player's physics component and body were found and read.
 */
[[nodiscard]] bool current_position(std::array<float, 3>& output) noexcept;

/**
 * Reads the local player's current position and camera forward vector together.
 * @param position Receives three world-space lanes of the player or camera pose.
 * @param forward Receives three lanes of the published camera forward vector.
 * @return True when a finite position and forward vector were available.
 */
[[nodiscard]] bool current_camera_pose(std::array<float, 3>& position,
                                       std::array<float, 3>& forward) noexcept;

/**
 * Attaches the camera and physics hooks that carry the teleport.
 * @return True when all three targets were found and both detours attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches both hooks only after camera, physics and explicit sync callers have finished. */
[[nodiscard]] bool uninstall() noexcept;

/** Publishes the camera pose for the frame and the forward vector for the next physics tick. */
void capture_camera_pose(std::uint32_t playerIndex) noexcept;

/** Latches one teleport request if the bound key went down this frame. */
void poll_request() noexcept;

/** Runs the move for a request no physics tick collected, and drives the sync for it. */
void force_pending() noexcept;

/**
 * Finds both key tables from the polled keyboard scan.
 * @return True when the scan and both of its table loads were found.
 */
[[nodiscard]] bool resolve_action_keys() noexcept;

/** Drops both key tables. */
void clear_action_keys() noexcept;

/**
 * Turns one authored binding into the virtual key the scan will read.
 * @param binding Input code taken from an authored binding half.
 * @return The virtual key, or 0 when there is none.
 */
[[nodiscard]] std::uint32_t action_key(std::uint16_t binding) noexcept;

/**
 * Calls the physics sync for one component through the installed trampoline.
 * @param component Physics component to sync.
 */
void invoke_sync(void* component) noexcept;

/**
 * Observes the local player's physics component and applies a pending teleport when requested.
 * @param component Physics component about to be synced.
 */
void apply_pending(void* component) noexcept;

/**
 * @param component Candidate physics component.
 * @return True when it drives the object the local player controls.
 */
[[nodiscard]] bool owns_local_player(void* component) noexcept;

/**
 * Reads the world position of the body a physics component drives.
 * @param component Physics component.
 * @param position Receives the three lanes.
 * @return True when the body was found and read.
 */
[[nodiscard]] bool read_position(void* component, Vector& position) noexcept;

/**
 * Writes the world position of the body a physics component drives.
 * @param component Physics component.
 * @param position Three world-space lanes to store.
 * @return True when the body was found and written.
 */
[[nodiscard]] bool write_position(void* component, const Vector& position) noexcept;

/**
 * Reads the linear velocity of the body a physics component drives.
 * @param component Physics component.
 * @param velocity Receives the three lanes.
 * @return True when the body was found and read.
 */
[[nodiscard]] bool read_velocity(void* component, Vector& velocity) noexcept;

/**
 * Writes the linear velocity of the body a physics component drives.
 * @param component Physics component.
 * @param velocity Three lanes to store.
 * @return True when the body was found and written.
 */
[[nodiscard]] bool write_velocity(void* component, const Vector& velocity) noexcept;

/**
 * Reports the physics component the local player was last seen driving.
 * The sync stops for a player at rest, so a frame poll has no other way back to them.
 * @return That component, or null before the player has been seen.
 */
[[nodiscard]] void* local_player_component() noexcept;

/**
 * Reports the complete datum handle of the object controlled by the local player.
 * @return Full controlled-object handle, or 0xFFFFFFFF when unavailable.
 */
[[nodiscard]] std::uint32_t local_player_handle() noexcept;

/**
 * The camera hook is the only site that sees the pose block, so it publishes the vector here.
 * @param forward Receives the camera forward vector published this frame.
 * @return True once the camera hook has published one.
 */
[[nodiscard]] bool camera_forward(Vector& forward) noexcept;

/** Copies the last complete pose published by the camera-frame hook. */
[[nodiscard]] bool camera_pose(CameraPose& pose) noexcept;

/** Reads the native controlled handle for local-player ownership checks. */
[[nodiscard]] bool current_controlled_handle(std::uint32_t& handle) noexcept;

} // namespace sunrise::client::hooks::teleport
