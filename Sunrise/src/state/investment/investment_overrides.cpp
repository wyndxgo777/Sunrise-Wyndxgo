#include "investment_overrides.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>

#include "investment.h"
#include "store_internal.h"

namespace sunrise::state::investment {
namespace {

std::atomic_bool g_refetchRequested{false};

} // namespace

/** Asks the client to fetch its family-5 object again. */
void request_client_refetch() noexcept {
    g_refetchRequested.store(true, std::memory_order_release);
}

/** @return True once per request. */
bool consume_client_refetch() noexcept {
    return g_refetchRequested.exchange(false, std::memory_order_acq_rel);
}

/** Sets or replaces one flag override. */
bool set_flag_override(std::uint16_t slot, std::uint8_t value) noexcept {
    store::g_mutex.lock();
    Family5State family{};
    const bool loaded = store::read_family5(family);
    if (!loaded) {
        store::g_mutex.unlock();
        return false;
    }
    bool stored = false;
    for (std::size_t index = 0; index < family.flagCount; ++index) {
        if (family.flags[index].slot == slot) {
            family.flags[index].value = value;
            stored = true;
            break;
        }
    }
    if (!stored && family.flagCount < family.flags.size()) {
        family.flags[family.flagCount++] = {slot, value};
        stored = true;
    }
    const bool written = stored && store::write_family5(family);
    store::g_mutex.unlock();
    return written;
}

/** Removes one flag override. */
void clear_flag_override(std::uint16_t slot) noexcept {
    store::g_mutex.lock();
    Family5State family{};
    if (store::read_family5(family)) {
        for (std::size_t index = 0; index < family.flagCount; ++index) {
            if (family.flags[index].slot == slot) {
                --family.flagCount;
                family.flags[index] = family.flags[family.flagCount];
                family.flags[family.flagCount] = {};
                break;
            }
        }
        (void)store::write_family5(family);
    }
    store::g_mutex.unlock();
}

/** Sets or replaces one value override. */
bool set_value_override(std::uint16_t slot, std::int32_t value) noexcept {
    store::g_mutex.lock();
    Family5State family{};
    const bool loaded = store::read_family5(family);
    if (!loaded) {
        store::g_mutex.unlock();
        return false;
    }
    bool stored = false;
    for (std::size_t index = 0; index < family.valueCount; ++index) {
        if (family.values[index].slot == slot) {
            family.values[index].value = value;
            stored = true;
            break;
        }
    }
    if (!stored && family.valueCount < family.values.size()) {
        family.values[family.valueCount++] = {slot, value};
        stored = true;
    }
    const bool written = stored && store::write_family5(family);
    store::g_mutex.unlock();
    return written;
}

/** Removes one value override. */
void clear_value_override(std::uint16_t slot) noexcept {
    store::g_mutex.lock();
    Family5State family{};
    if (store::read_family5(family)) {
        for (std::size_t index = 0; index < family.valueCount; ++index) {
            if (family.values[index].slot == slot) {
                --family.valueCount;
                family.values[index] = family.values[family.valueCount];
                family.values[family.valueCount] = {};
                break;
            }
        }
        (void)store::write_family5(family);
    }
    store::g_mutex.unlock();
}

} // namespace sunrise::state::investment
