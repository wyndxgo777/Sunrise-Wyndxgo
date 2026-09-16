#include "entity_create_probe.h"

#include <Windows.h>
#include <intrin.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../core/logging/log.h"
#include "../hooking/detour.h"
#include "../patterns/image_scan.h"
#include "../patterns/signature_text.h"

namespace sunrise::client::diagnostics {
namespace {

namespace patterns = client::patterns;
namespace detour = client::hooking::detour;

/**
 * The index allocator the entity creator calls first.
 * Recovered from the mapped-image dump. Its body is unmistakable: it stores -1 into the caller's
 * out-parameter, then asks a pool at `+0xC118` sized `0x2000` for a free index. The frame size is
 * wildcarded so the match carries no position-dependent byte.
 */
constexpr std::string_view kIndexAllocatorText =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC ? 48 8B DA C7 02 FF FF FF FF 48 8B F9 "
    "BA 00 20 00 00";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kIndexAllocator =
    patterns::signature<patterns::signature_length(kIndexAllocatorText)>(kIndexAllocatorText);

/** The allocator answers this in its out-parameter when it has no index to give. */
constexpr std::int32_t kNoIndex = -1;
/**
 * Byte offset of the free-slot bitmap inside the manager the allocator is handed.
 * Read out of the allocator's body: it calls the bitmap search with `rcx = manager + 0xC118` and
 * a width of `0x2000`, then clears the bit it was given. A set bit is therefore a FREE slot, and
 * the search answers -1 only when every word is zero.
 */
constexpr std::size_t kFreeBitmapOffset = 0xC118;
/** Slots the bitmap covers, from the width the allocator passes. */
constexpr std::size_t kFreeBitmapBits = 0x2000;
/** Words in that bitmap. */
constexpr std::size_t kFreeBitmapWords = kFreeBitmapBits / 32;
/** Bytes in the bitmap, from the width the client's own stocking path passes to its fill. */
constexpr std::size_t kFreeBitmapBytes = kFreeBitmapBits / 8;
/** Bits per bitmap word. */
constexpr std::size_t kBitsPerWord = 32;
/**
 * High slots the host keeps for its own entities and never leases to the client.
 * The join grant is `kSlotCount - kDefaultServerReserve` = 7936, so the top 256 indices are the
 * host's. The client's own initialiser frees the whole bitmap because in its intended world it
 * owns every slot; here it does not, and handing it the reserve would let it allocate an index
 * the host also considers its own.
 */
constexpr std::size_t kServerReserveSlots = 256;
/** Bytes of the bitmap that stay clear, covering the reserve at the top of the index space. */
constexpr std::size_t kReserveBytes = kServerReserveSlots / 8;
/** Bytes of the bitmap that are freed to the client. */
constexpr std::size_t kClientBytes = kFreeBitmapBytes - kReserveBytes;
/** Words of the bitmap covering the client's half. The split lands on a word boundary. */
constexpr std::size_t kClientWords = kClientBytes / sizeof(std::uint32_t);
static_assert(kClientBytes % sizeof(std::uint32_t) == 0,
              "the client half must end on a word so a refill never touches the reserve");

/**
 * Image offset of the pointer to the game's entity record table.
 * Recovered from the creation path itself, which indexes it as `base + (handle & 0x1FFF) * stride`
 * at `0x4D71F7`: `imul ebx, [rip -> 0x1F93430]` then `add rbx, [rip -> 0x1F93428]`. The mask is the
 * same 13 bits the allocator's bitmap covers, so a record addresses exactly one allocated index.
 */
constexpr std::uintptr_t kEntityTableBaseRva = 0x1F93428;
/** Image offset of the record stride that pairs with the table above. */
constexpr std::uintptr_t kEntityTableStrideRva = 0x1F93430;
/** Stride the dump reports. Checked at runtime, because a wrong one would read foreign memory. */
constexpr std::uint32_t kExpectedRecordStride = 224;
/**
 * Record class every live entity carries at `+0x64`.
 * Constant across every record of a run, so it marks a slot the game has actually built rather
 * than one holding whatever the last entity left behind.
 */
constexpr std::uint32_t kRecordClass = 0x80809783;
/** Offset of the record class within a record. */
constexpr std::size_t kRecordClassOffset = 0x64;

/** Outcomes reported per run, so a per-frame failure cannot fill the log. */
constexpr LONG kMaxReports = 200;

/**
 * The allocator's real shape, read from its body rather than guessed.
 * It uses exactly two arguments: `rcx` is the manager whose free-slot bitmap sits at `+0xC118`,
 * and `rdx` is the out-parameter it fills with the allocated index. It returns `rdx` unchanged.
 */
using IndexAllocator = void*(__fastcall*)(void*, std::int32_t*) noexcept;

detour::Handle g_allocator{};
volatile LONG g_reports{};
/**
 * One manager's record of the indices this hook has watched the allocator hand out.
 *
 * A blanket `memset(bitmap, 0xFF, ...)` is what made the very first stocking work and what made
 * every later one lethal. It frees index 0 upward, and by the time a pool has drained, index 0
 * belongs to a live entity. The allocator picks the lowest set bit, so the next creation lands on
 * top of a live entity and the world stops being a consistent list of them. That is the crash on
 * respawn, the crash on an ability, and the mainloop stall that ends an encounter a few seconds
 * after the room loads.
 *
 * Keeping the set of indices already handed out turns the refill from "free everything" into
 * "free what was never taken", which is the only form of it that is safe to run on a live pool.
 */
struct PoolRecord {
    /** Manager this record belongs to, or null while the slot is unused. */
    void* pool;
    /** Set bit per index the allocator gave out and the client has not since handed back. */
    std::array<volatile LONG, kFreeBitmapWords> live;
};

/** Managers tracked at once. A world change builds a new one, so several are live per run. */
constexpr std::size_t kTrackedPoolCapacity = 16;
/** Per-manager occupancy records, claimed on first sight. */
std::array<PoolRecord, kTrackedPoolCapacity> g_pools{};

/**
 * Counts the free slots the manager currently holds.
 * The exhaustion line alone cannot separate "the host never gave the client any slots" from
 * "the client used everything it was given", and those need opposite fixes.
 * @param pool Manager the allocator was handed.
 * @return Set bits in its free bitmap, or -1 when the bitmap cannot be read.
 */
[[nodiscard]] std::int64_t free_slot_count(const void* pool) noexcept {
    if (pool == nullptr) {
        return -1;
    }
    std::int64_t free = 0;
    __try {
        const auto* words = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::byte*>(pool) + kFreeBitmapOffset);
        for (std::size_t word = 0; word < kFreeBitmapWords; ++word) {
            free += static_cast<std::int64_t>(__popcnt(words[word]));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return free;
}

/**
 * Finds the record for one manager, claiming a free slot on first sight.
 * @param pool Manager the allocator was handed.
 * @return Its record, or null when the table is full.
 */
[[nodiscard]] PoolRecord* find_pool(void* pool) noexcept {
    for (auto& record : g_pools) {
        if (record.pool == pool) {
            return &record;
        }
    }
    for (auto& record : g_pools) {
        auto* const slot = reinterpret_cast<void* volatile*>(&record.pool);
        if (InterlockedCompareExchangePointer(slot, pool, nullptr) == nullptr
            || record.pool == pool) {
            return &record;
        }
    }
    // Past capacity nothing is tracked, so nothing is refilled either. A missed refill costs this
    // world's entities; an untracked one corrupts a live pool.
    return nullptr;
}

/**
 * Records that one index is now owned by an entity.
 * @param record Manager record, or null when the manager is untracked.
 * @param index Index the allocator produced.
 */
void mark_live(PoolRecord* record, std::int32_t index) noexcept {
    if (record == nullptr || index < 0 || static_cast<std::size_t>(index) >= kFreeBitmapBits) {
        return;
    }
    const auto slot = static_cast<std::size_t>(index);
    (void)InterlockedOr(&record->live[slot / kBitsPerWord],
                        static_cast<LONG>(1u << (slot % kBitsPerWord)));
}

/**
 * Reports one outcome, up to the per-run budget.
 * @param stage Which half answered.
 * @param outcome What it answered.
 * @param detail Free slots left in the pool, or -1 when the bitmap could not be read.
 */
void report(const char* stage, const char* outcome, std::int64_t detail) noexcept {
    if (!core::log::accepts(core::log::Channel::client, core::log::Level::debug)
        || InterlockedIncrement(&g_reports) > kMaxReports) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=entity_create stage=%s result=%s free=%lld",
                                      stage,
                                      outcome,
                                      static_cast<long long>(detail));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Reports one refill, naming how many slots it actually handed back.
 * @param outcome Whether the pool answered after the refill.
 * @param free Free slots the bitmap holds now.
 * @param freed Slots this refill put back.
 */
void report_stock(const char* outcome, std::int64_t free, std::int64_t freed) noexcept {
    if (!core::log::accepts(core::log::Channel::client, core::log::Level::debug)
        || InterlockedIncrement(&g_reports) > kMaxReports) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=entity_create stage=allocate result=%s free=%lld freed=%lld",
                      outcome,
                      static_cast<long long>(free),
                      static_cast<long long>(freed));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Reads the game's entity record table, or reports that it cannot be addressed.
 * @param table Receives the table base.
 * @param stride Receives the record stride.
 * @return True when both were read and the stride still matches this build.
 */
[[nodiscard]] bool entity_table(const std::byte*& table, std::uint32_t& stride) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0) {
        return false;
    }
    __try {
        table = *reinterpret_cast<const std::byte* const*>(base + kEntityTableBaseRva);
        stride = *reinterpret_cast<const std::uint32_t*>(base + kEntityTableStrideRva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return table != nullptr && stride == kExpectedRecordStride;
}

/**
 * Reports which of one word's 32 indices already hold an entity.
 *
 * This hook's own record of handed-out indices covers only what came through the hooked allocator,
 * which a census measured as 58 of 830 — the world's placed objects reach the table by some other
 * path entirely. Trusting that record alone therefore freed 7936 slots while live entities were
 * sitting in them, and the client then allocated straight over the top. The game's own record
 * table is the authority on which slots are taken, so occupancy is read from there instead.
 * @param table Entity record table base.
 * @param stride Record stride.
 * @param word Word of the free bitmap being refilled.
 * @return Set bit per index in that word whose record is live.
 */
[[nodiscard]] LONG occupied_mask(const std::byte* table,
                                 std::uint32_t stride,
                                 std::size_t word) noexcept {
    std::uint32_t mask = 0;
    for (std::size_t bit = 0; bit < kBitsPerWord; ++bit) {
        const std::size_t index = word * kBitsPerWord + bit;
        __try {
            if (*reinterpret_cast<const std::uint32_t*>(table + index * stride
                                                        + kRecordClassOffset)
                == kRecordClass) {
                mask |= 1u << bit;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // An unreadable record is treated as taken, which costs a slot rather than an entity.
            mask |= 1u << bit;
        }
    }
    return static_cast<LONG>(mask);
}

/**
 * Frees every client slot that no entity holds, leaving the ones that do alone.
 *
 * The client's own initialiser fills this bitmap with `0xFF` — every slot free — but only when a
 * role global reads zero; hosted by Sunrise it reads 3, so the fill never runs and the bitmap is
 * all-zero from the first frame. Every entity creation then fails, which is why no enemy, plate,
 * door or banner ever appeared and why an encounter bubble kicked to orbit. Writing those bytes
 * ourselves is right exactly once, on a pool that is still empty. On a pool that has drained it is
 * catastrophic, because the slots the client is using read as clear too and become free again.
 *
 * So the refill is driven by occupancy instead of by a constant. A slot is freed only when the
 * bitmap says it is taken AND neither the game's record table nor this hook's own list says an
 * entity is in it. Two passes, because another thread may claim a slot while the first one runs:
 * the second re-clears anything that became live in between, so no index is ever offered twice.
 * @param pool Manager the allocator was handed.
 * @param record Occupancy record for that manager.
 * @param failure Receives a Windows error, or 1 for a null pool and 2 for a faulting write.
 * @return Slots freed, or -1 when the bitmap could not be written.
 */
[[nodiscard]] std::int64_t stock_pool(void* pool,
                                      PoolRecord* record,
                                      std::uint32_t& failure) noexcept {
    failure = 0;
    if (pool == nullptr || record == nullptr) {
        failure = 1;
        return -1;
    }
    auto* const bitmap = static_cast<std::byte*>(pool) + kFreeBitmapOffset;
    // The bitmap sits in the game's own allocation, so it carries whatever protection that
    // allocation was given. Reading it worked, which does not prove it is writable.
    DWORD previous = 0;
    if (VirtualProtect(bitmap, kFreeBitmapBytes, PAGE_READWRITE, &previous) == FALSE) {
        failure = GetLastError();
        return -1;
    }
    const std::byte* table = nullptr;
    std::uint32_t stride = 0;
    const bool hasTable = entity_table(table, stride);
    std::int64_t freed = 0;
    __try {
        auto* const words = reinterpret_cast<volatile LONG*>(bitmap);
        for (std::size_t word = 0; word < kClientWords; ++word) {
            const LONG available = words[word];
            // A slot the client has put back is no longer live, so it returns to the pool with the
            // rest. Without this the record would only ever grow and the refill would fade to a
            // no-op over a long session.
            const LONG live = InterlockedAnd(&record->live[word], ~available) & ~available;
            // The record table is the authority; this hook's own list is kept as a second opinion
            // for anything created in the window before its record is filled in.
            const LONG occupied = hasTable ? occupied_mask(table, stride, word) : 0;
            const LONG missing =
                static_cast<LONG>(~static_cast<std::uint32_t>(live | available | occupied));
            if (missing != 0) {
                (void)InterlockedOr(&words[word], missing);
                freed += __popcnt(static_cast<unsigned int>(missing));
            }
        }
        for (std::size_t word = 0; word < kClientWords; ++word) {
            const LONG live = record->live[word];
            if (live != 0) {
                (void)InterlockedAnd(&words[word], ~live);
            }
        }
        // The host's reserve at the top of the space stays clear so the client cannot allocate an
        // index the host also considers its own.
        for (std::size_t word = kClientWords; word < kFreeBitmapWords; ++word) {
            (void)InterlockedAnd(&words[word], 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        failure = 2;
        freed = -1;
    }
    DWORD restored = 0;
    (void)VirtualProtect(bitmap, kFreeBitmapBytes, previous, &restored);
    if (freed >= 0 && !hasTable) {
        // Worth saying out loud: without the table the refill is back to trusting a list that has
        // been measured as 7% complete, which is how live entities got overwritten.
        report("allocate", "stock_without_table", freed);
    }
    return freed;
}

/**
 * Mirrors the index allocator, stocking its pool when the pool has nothing to give.
 * The out-parameter is the answer: the original writes -1 into it before doing anything, and
 * overwrites it only on success.
 */
void* __fastcall allocator_body(void* pool, std::int32_t* index) noexcept {
    const auto call = reinterpret_cast<IndexAllocator>(g_allocator.original);
    if (call == nullptr) {
        return nullptr;
    }
    void* result = call(pool, index);
    PoolRecord* const record = find_pool(pool);
    if (index == nullptr || *index != kNoIndex) {
        // Every index the client takes is recorded before anything else can act on it, because a
        // refill that does not know about it would offer the same index to a second entity.
        if (index != nullptr) {
            mark_live(record, *index);
        }
        return result;
    }
    const std::int64_t free = free_slot_count(pool);
    // Every drain is refilled, not just the first. The client trims the bitmap back within about
    // 16 allocations, so a pool stocked once is empty again by the next encounter bubble. This is
    // safe for the same reason the first stock is: the refill frees nothing the record table
    // reports as occupied.
    if (free != 0 || record == nullptr) {
        report("allocate", "exhausted", free);
        return result;
    }
    std::uint32_t failure = 0;
    const std::int64_t freed = stock_pool(pool, record, failure);
    if (freed < 0) {
        // Naming the reason matters: a refused write and a faulting page need different fixes.
        report("allocate", "stock_failed", static_cast<std::int64_t>(failure));
        return result;
    }
    result = call(pool, index);
    // `freed` is the number that matters. It should fall well short of the whole client half: the
    // gap is the live entities the old fill used to trample.
    report_stock(*index == kNoIndex ? "stocked_still_empty" : "stocked",
                 free_slot_count(pool),
                 freed);
    if (index != nullptr) {
        mark_live(record, *index);
    }
    return result;
}

/**
 * Attaches the hook, reporting its own outcome.
 * @param signature Pattern to find.
 * @param name Reported name.
 * @param replacement Replacement body.
 * @param handle Receives the trampoline.
 * @return True when the target was found and the detour attached.
 */
[[nodiscard]] bool attach(std::span<const patterns::PatternByte> signature,
                          const char* name,
                          void* replacement,
                          detour::Handle& handle) noexcept {
    std::byte* const target = patterns::scan_main_image_unique(signature, name);
    std::array<char, core::log::kLineCapacity> line{};
    if (target == nullptr) {
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=entity_create stage=attach name=%s result=fail",
                                          name);
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(written)});
        }
        return false;
    }
    const detour::Spec spec{target, replacement};
    const bool attached = detour::install(spec, handle);
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=entity_create stage=attach name=%s result=%s",
                                      name,
                                      attached ? "ok" : "fail");
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         attached ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return attached;
}

} // namespace

/** Stocks the client's entity free-slot bitmap, which on this host is never stocked at all. */
bool install_entity_create_probe() noexcept {
    // The initialiser beside the allocator is deliberately NOT hooked. Its fifth argument is
    // passed on the stack (`mov dword [var_20h], eax` before the call), and a four-argument
    // replacement got that wrong and black-screened the load. It does not need hooking anyway:
    // the allocator alone answers the question, because the initialiser only runs when the
    // allocator succeeded.
    return attach(kIndexAllocator,
                  "entity_index_allocator",
                  reinterpret_cast<void*>(&allocator_body),
                  g_allocator);
}

/** Detaches the entity allocator hook. */
void uninstall_entity_create_probe() noexcept {
    if (g_allocator.attached) {
        (void)detour::uninstall(g_allocator);
    }
}

} // namespace sunrise::client::diagnostics