#pragma once

namespace sunrise::client::diagnostics {

/**
 * Stocks the client's entity free-slot bitmap, which on this host is never stocked at all.
 *
 * The client logs `failed to create '<type>' entity` and nothing else, and that line covers two
 * different refusals: an index allocator that answers -1 when it has nothing to give, and an
 * initialiser that answers false when it refuses the entity it was handed. Hooking the allocator
 * separates them, and the answer is the allocator: its free-slot bitmap is all-zero from the first
 * frame.
 *
 * The client fills that bitmap itself, with every slot free, but only when a role global reads
 * zero. Hosted by Sunrise it reads 3, so the fill never runs and every entity creation fails for
 * the life of the process. No enemy, door, plate or banner is ever built, and an encounter bubble
 * that needs its own objects gives up and returns the player to orbit with error BIRD.
 *
 * So Sunrise writes those bytes instead, with two differences from the client's own fill. The
 * host's reserved indices at the top of the space stay clear, because here the client does not own
 * the whole range. And a slot is freed only when no entity is sitting in it: the game's own record
 * table is read for occupancy, so a refill can never hand one index to two entities. A blanket fill
 * does exactly that, and it crashes on respawn.
 *
 * Stocking once is not enough: the client trims the bitmap back to ~135 free within about 16
 * allocations, so the pool is empty again by the next encounter bubble. Every drain is refilled,
 * which the occupancy check above is what makes safe.
 *
 * The allocator is found with an independent scan rather than through the shared target registry,
 * so a signature that no longer matches a future build costs this and nothing else.
 * @return True when the hook attached.
 */
[[nodiscard]] bool install_entity_create_probe() noexcept;

/** Detaches the entity allocator hook. */
void uninstall_entity_create_probe() noexcept;

} // namespace sunrise::client::diagnostics