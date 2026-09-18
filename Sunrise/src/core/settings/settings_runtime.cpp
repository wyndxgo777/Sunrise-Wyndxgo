#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <string_view>

#include "../../../resources/resource.h"
#include "../filesystem/path.h"
#include "../logging/log.h"
#include "parser.h"
#include "settings.h"
#include "steam/platform_identity.h"

namespace sunrise::core::settings {
namespace {

/** The JSON settings file is the only file stored directly in the owned folder. */
constexpr std::wstring_view kSettingsFileSuffix = L"\\settings.json";
/** Largest settings file accepted into fixed storage. */
constexpr std::size_t kConfigCapacity = 1024 * 1024;

Settings g_settings = defaults();

/**
 * Names the step that ended the load. Settings are read before the log sinks exist, so this line
 * is the only way to report a boot failure here.
 * @param reason Short key naming the step.
 * @return Always false, so callers can return it directly.
 */
[[nodiscard]] bool fail(std::string_view reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=settings result=fail reason=%.*s",
                                      static_cast<int>(reason.size()),
                                      reason.data());
    if (written > 0) {
        log::early({line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

/**
 * Reports a settings version that differs from this build.
 * @param fileVersion Version read from the file, or zero when the key was missing.
 */
void report_version(std::uint32_t fileVersion) noexcept {
    if (fileVersion == kSettingsVersion) {
        return;
    }
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=settings stage=version result=mismatch file=%u build=%u",
                                      static_cast<unsigned>(fileVersion),
                                      static_cast<unsigned>(kSettingsVersion));
    if (written > 0) {
        log::early({line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Borrows the default settings document out of the module resources.
 * @param module Loaded DLL holding the default JSON resource.
 * @param output Receives the resource bytes, owned by the module.
 * @return True when the resource is present and not empty.
 */
[[nodiscard]] bool bundled_document(void* module, std::string_view& output) noexcept {
    const HMODULE loadedModule = static_cast<HMODULE>(module);
    const HRSRC resource =
        FindResourceW(loadedModule, MAKEINTRESOURCEW(IDR_DEFAULT_SETTINGS), RT_RCDATA);
    if (resource == nullptr) {
        return false;
    }
    const DWORD size = SizeofResource(loadedModule, resource);
    const HGLOBAL loaded = LoadResource(loadedModule, resource);
    const auto* bytes =
        loaded != nullptr ? static_cast<const char*>(LockResource(loaded)) : nullptr;
    if (size == 0 || bytes == nullptr) {
        return false;
    }
    output = std::string_view(bytes, size);
    return true;
}

/**
 * Writes one default settings document. An existing file is never overwritten.
 * @param configPath Null-terminated destination path.
 * @param document Bundled or shipped default bytes.
 * @return True when every byte is written and the file closes cleanly.
 */
[[nodiscard]] bool write_document_text(const path::Buffer& configPath,
                                       std::string_view document) noexcept {
    const HANDLE file = CreateFileW(configPath.chars.data(),
                                    GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const auto size = static_cast<DWORD>(document.size());
    bool complete =
        WriteFile(file, document.data(), size, &written, nullptr) != FALSE && written == size;
    complete = CloseHandle(file) != FALSE && complete;
    if (!complete) {
        // A half-written default must not become the next boot's settings.
        (void)DeleteFileW(configPath.chars.data());
    }
    return complete;
}

/**
 * Copies the bundled default settings. An existing file is never overwritten.
 * @param module Loaded DLL holding the default JSON resource.
 * @param configPath Null-terminated destination path.
 * @return True when every bundled byte is written and the file closes cleanly.
 */
[[nodiscard]] bool write_default(void* module, const path::Buffer& configPath) noexcept {
    std::string_view document;
    if (!bundled_document(module, document)) {
        return false;
    }
    return write_document_text(configPath, document);
}

/**
 * Drops a leading UTF-8 byte order mark.
 * Common editors write one and the parser would read it as a stray token, which fails startup
 * before the log opens.
 * @param document Whole settings text as read from disk.
 * @return The same text with any BOM removed.
 */
[[nodiscard]] std::string_view without_byte_order_mark(std::string_view document) noexcept {
    // UTF-8 byte order mark. An editor writes it and the parser must not see it.
    constexpr std::string_view kMark = "\xEF\xBB\xBF";
    return document.starts_with(kMark) ? document.substr(kMark.size()) : document;
}

} // namespace

/**
 * Validates one loaded document, migrates old layouts, and commits it as active settings.
 * Mirrors the DLL initialize() tail for the headless EXE entry (module resource default vs
 * shipped default_settings.json file). Keep the validation rules identical in both.
 * @param configPath Destination rewritten when a missing file is created or an old layout
 *        is migrated.
 * @param rawDocument Bytes just read from the settings file, BOM not yet removed.
 * @param defaultDocument Default bytes used when the file is created or migrated, or empty
 *        when no default source exists (creation/migration then fail instead of crashing).
 * @return True when a valid document is active.
 */
[[nodiscard]] bool activate_document(const path::Buffer& configPath,
                                     std::string_view rawDocument,
                                     std::string_view defaultDocument) noexcept {
    std::string_view document = without_byte_order_mark(rawDocument);
    std::uint32_t version = 0;
    bool compact = false;
    if (!parser::Parser(document).parse_version(version, &compact)) {
        return fail("version");
    }
    if (!compact && version < kSettingsVersion) {
        if (defaultDocument.empty()) {
            return fail("write_default");
        }
        if (!DeleteFileW(configPath.chars.data())) {
            return fail("delete_old");
        }
        if (!write_document_text(configPath, defaultDocument)) {
            return fail("write_default");
        }
        document = without_byte_order_mark(defaultDocument);
    }
    Settings parsed;
    if (!parse(document, parsed)) {
        return fail("parse");
    }
    if (parsed.compactClient || parsed.compactHost) {
        const auto selectedRole = parsed.compactHost ? Role::host : Role::client;
        if (!configure_role(selectedRole, true)) {
            return fail("compact_role");
        }
        if (!parsed.steam.user.hasConfiguredSteamId
            && !steam::platform_identity::load_or_create(parsed.steam.user.steamId)) {
            return fail("identity_cache");
        }
        parsed.version = kSettingsVersion;
        parsed.configuredRole = selectedRole;
        parsed.client.externalServer.enabled = false;
        if (parsed.compactHost) {
            parsed.server.bapBind = {0, 0, 0, 0};
            parsed.server.bapPort = parsed.client.serverEndpoint.bapPort;
            parsed.server.gameplay.bindAddress = {0, 0, 0, 0};
            parsed.server.gameplay.advertisedAddress = parsed.client.serverEndpoint.address;
            parsed.server.gameplay.transportAddress = parsed.client.serverEndpoint.address;
        } else {
            parsed.server.upstream.enabled = true;
            parsed.server.upstream.host = parsed.client.serverEndpoint.host;
            parsed.server.upstream.address = parsed.client.serverEndpoint.address;
            parsed.server.upstream.bapPort = parsed.client.serverEndpoint.bapPort;
            parsed.server.bapPort = 0;
        }
    } else if (!configure_role(parsed.configuredRole, parsed.hasConfiguredRole)) {
        return fail("configured_role");
    }
    if (role() == Role::client && !parsed.client.externalServer.enabled) {
        if (!parsed.server.upstream.enabled) {
            parsed.server.upstream.enabled = true;
            parsed.server.upstream.host = parsed.client.serverEndpoint.host;
            parsed.server.upstream.address = parsed.client.serverEndpoint.address;
            parsed.server.upstream.bapPort = parsed.client.serverEndpoint.bapPort;
        }
        parsed.server.bapPort = 0;
    }
    if (hosts_session() && parsed.server.upstream.enabled) {
        return fail("host_upstream");
    }
    // Dedicated obeys the playing-host listener rules: it serves, never proxies, and its
    // BAP bind must be all-interfaces (LAN joins) or loopback (local test), never a
    // single public IP that would silently drop one side's traffic.
    if (role() == Role::host || role() == Role::dedicated) {
        if (parsed.client.externalServer.enabled) {
            return fail("host_external");
        }
        if (parsed.server.bapPort == 0
            || (parsed.server.bapBind != std::array<unsigned char, 4>{0, 0, 0, 0}
                && parsed.server.bapBind != std::array<unsigned char, 4>{127, 0, 0, 1})) {
            return fail("host_local_listener");
        }
        if (parsed.server.gameplay.topology != server::gameplay::Topology::embedded) {
            return fail("host_gameplay");
        }
    }
    for (std::size_t i = 0; i < parsed.server.upstream.host.size(); ++i) {
        parsed.server.upstream.hostWide[i] = static_cast<wchar_t>(parsed.server.upstream.host[i]);
    }
    report_version(parsed.version);
    g_settings = parsed;
    return true;
}

/** Loads the settings file from the owned folder, or creates the default one. */
bool initialize(void* module) noexcept {
    if (!configure_role(Role::embedded, false)) {
        return fail("role");
    }
    path::Buffer configPath;
    if (!path::artifact_directory(module, configPath)
        || !path::append(configPath, kSettingsFileSuffix)) {
        return fail("path");
    }

    const HANDLE file = CreateFileW(configPath.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    HANDLE readableFile = file;
    if (readableFile == INVALID_HANDLE_VALUE) {
        // A missing file is created once; other open failures remain fatal.
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return fail("open");
        }
        if (!write_default(module, configPath)) {
            return fail("write_default");
        }
        readableFile = CreateFileW(configPath.chars.data(),
                                   GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
        if (readableFile == INVALID_HANDLE_VALUE) {
            return fail("reopen");
        }
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(readableFile, &size) || size.QuadPart <= 0) {
        CloseHandle(readableFile);
        return fail("empty");
    }
    if (static_cast<std::uint64_t>(size.QuadPart) > kConfigCapacity) {
        // Silence here reads exactly like a crash, and the cap is the usual cause.
        CloseHandle(readableFile);
        return fail("too_large");
    }

    const std::unique_ptr<std::array<char, kConfigCapacity>> buffer{
        new (std::nothrow) std::array<char, kConfigCapacity>{}};
    if (!buffer) {
        CloseHandle(readableFile);
        return fail("allocate");
    }
    DWORD read = 0;
    const bool readOk =
        ReadFile(readableFile, buffer->data(), static_cast<DWORD>(size.QuadPart), &read, nullptr)
            != FALSE
        && read == size.QuadPart;
    const bool closed = CloseHandle(readableFile) != FALSE;
    if (!readOk || !closed) {
        return fail("read");
    }
    std::string_view document = without_byte_order_mark(std::string_view(buffer->data(), read));
    std::uint32_t version = 0;
    bool compact = false;
    if (!parser::Parser(document).parse_version(version, &compact)) {
        return fail("version");
    }
    if (!compact && version < kSettingsVersion) {
        if (!DeleteFileW(configPath.chars.data())) {
            return fail("delete_old");
        }
        if (!write_default(module, configPath) || !bundled_document(module, document)) {
            return fail("write_default");
        }
    }
    Settings parsed;
    if (!parse(document, parsed)) {
        return fail("parse");
    }
    if (parsed.compactClient || parsed.compactHost) {
        const auto selectedRole = parsed.compactHost ? Role::host : Role::client;
        if (!configure_role(selectedRole, true)) {
            return fail("compact_role");
        }
        if (!parsed.steam.user.hasConfiguredSteamId
            && !steam::platform_identity::load_or_create(parsed.steam.user.steamId)) {
            return fail("identity_cache");
        }
        parsed.version = kSettingsVersion;
        parsed.configuredRole = selectedRole;
        parsed.client.externalServer.enabled = false;
        if (parsed.compactHost) {
            parsed.server.bapBind = {0, 0, 0, 0};
            parsed.server.bapPort = parsed.client.serverEndpoint.bapPort;
            parsed.server.gameplay.bindAddress = {0, 0, 0, 0};
            parsed.server.gameplay.advertisedAddress = parsed.client.serverEndpoint.address;
            parsed.server.gameplay.transportAddress = parsed.client.serverEndpoint.address;
        } else {
            parsed.server.upstream.enabled = true;
            parsed.server.upstream.host = parsed.client.serverEndpoint.host;
            parsed.server.upstream.address = parsed.client.serverEndpoint.address;
            parsed.server.upstream.bapPort = parsed.client.serverEndpoint.bapPort;
            parsed.server.bapPort = 0;
        }
    } else if (!configure_role(parsed.configuredRole, parsed.hasConfiguredRole)) {
        return fail("configured_role");
    }
    if (role() == Role::client && !parsed.client.externalServer.enabled) {
        if (!parsed.server.upstream.enabled) {
            parsed.server.upstream.enabled = true;
            parsed.server.upstream.host = parsed.client.serverEndpoint.host;
            parsed.server.upstream.address = parsed.client.serverEndpoint.address;
            parsed.server.upstream.bapPort = parsed.client.serverEndpoint.bapPort;
        }
        parsed.server.bapPort = 0;
    }
    if (hosts_session() && parsed.server.upstream.enabled) {
        return fail("host_upstream");
    }
    // Dedicated obeys the playing-host listener rules: it serves, never proxies, and its
    // BAP bind must be all-interfaces (LAN joins) or loopback (local test).
    if (role() == Role::host || role() == Role::dedicated) {
        if (parsed.client.externalServer.enabled) {
            return fail("host_external");
        }
        if (parsed.server.bapPort == 0
            || (parsed.server.bapBind != std::array<unsigned char, 4>{0, 0, 0, 0}
                && parsed.server.bapBind != std::array<unsigned char, 4>{127, 0, 0, 1})) {
            return fail("host_local_listener");
        }
        if (parsed.server.gameplay.topology != server::gameplay::Topology::embedded) {
            return fail("host_gameplay");
        }
    }
    for (std::size_t i = 0; i < parsed.server.upstream.host.size(); ++i) {
        parsed.server.upstream.hostWide[i] = static_cast<wchar_t>(parsed.server.upstream.host[i]);
    }
    report_version(parsed.version);
    g_settings = parsed;
    return true;
}

namespace {
/** Default-settings file the headless EXE ships beside itself (repo: Sunrise/resources/). */
constexpr std::wstring_view kDefaultSettingsFileSuffix = L"\\default_settings.json";

/**
 * Reads one whole file into fixed config storage.
 * @param filePath Null-terminated source path.
 * @param buffer Caller storage; one whole file must fit.
 * @param read Receives the byte count on success.
 * @return True only when the file opened, fitted, read whole, and closed cleanly.
 */
[[nodiscard]] bool read_config_bytes(const path::Buffer& filePath,
                                     std::array<char, kConfigCapacity>& buffer,
                                     DWORD& read) noexcept {
    const HANDLE file = CreateFileW(filePath.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0
        && static_cast<std::uint64_t>(size.QuadPart) <= kConfigCapacity;
    DWORD got = 0;
    if (ok) {
        ok = ReadFile(file, buffer.data(), static_cast<DWORD>(size.QuadPart), &got, nullptr)
                != FALSE
            && got == size.QuadPart;
    }
    ok = CloseHandle(file) != FALSE && ok;
    if (!ok) {
        return false;
    }
    read = got;
    return true;
}
} // namespace

/**
 * Loads the settings file from one directory, for the headless dedicated EXE that has no
 * DLL module or bundled default-settings resource. Missing settings.json is created from
 * the default_settings.json shipped beside the EXE.
 */
bool initialize_from_directory(const wchar_t* directory) noexcept {
    if (directory == nullptr || directory[0] == L'\0') {
        return fail("directory");
    }
    if (!configure_role(Role::embedded, false)) {
        return fail("role");
    }
    path::Buffer owned{};
    if (!path::assign(owned, std::wstring_view(directory))) {
        return fail("path");
    }
    while (owned.length > 0
           && (owned.chars[owned.length - 1] == L'\\'
               || owned.chars[owned.length - 1] == L'/')) {
        owned.chars[--owned.length] = L'\0';
    }
    if (owned.length == 0) {
        return fail("path");
    }
    path::Buffer configPath = owned;
    if (!path::append(configPath, kSettingsFileSuffix)) {
        return fail("path");
    }
    path::Buffer defaultPath = owned;
    if (!path::append(defaultPath, kDefaultSettingsFileSuffix)) {
        return fail("path");
    }

    // Heap-allocated: two config buffers plus path buffers exceed the default 1 MB
    // stack when kept as locals (the DLL entry does the same).
    const std::unique_ptr<std::array<char, kConfigCapacity>> defaultStorage{
        new (std::nothrow) std::array<char, kConfigCapacity>{}};
    const std::unique_ptr<std::array<char, kConfigCapacity>> fileStorage{
        new (std::nothrow) std::array<char, kConfigCapacity>{}};
    if (!defaultStorage || !fileStorage) {
        return fail("allocate");
    }
    std::string_view defaultDocument;
    {
        DWORD defaultRead = 0;
        if (read_config_bytes(defaultPath, *defaultStorage, defaultRead)) {
            defaultDocument = std::string_view(defaultStorage->data(), defaultRead);
        }
    }

    DWORD fileRead = 0;
    if (!read_config_bytes(configPath, *fileStorage, fileRead)) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND || defaultDocument.empty()) {
            return fail("open");
        }
        if (!write_document_text(configPath, defaultDocument)) {
            return fail("write_default");
        }
        return activate_document(configPath, defaultDocument, defaultDocument);
    }
    return activate_document(
        configPath, std::string_view(fileStorage->data(), fileRead), defaultDocument);
}

/** Resets active settings to the fixed defaults. */
void shutdown() noexcept {
    g_settings = defaults();
}

/** @return Active read-only Core settings. */
const Settings& get() noexcept {
    return g_settings;
}

} // namespace sunrise::core::settings
