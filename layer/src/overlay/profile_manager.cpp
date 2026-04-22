#include "profile_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "config.hpp"
#include "effects/effect_registry.hpp"
#include "effects/params/effect_param.hpp"
#include "imgui_overlay.hpp"
#include "ipc.hpp"
#include "logger.hpp"

namespace lumen
{
    namespace fs = std::filesystem;

    namespace
    {
        ProfileState g_state;
        bool         g_initialized = false;

        // The four cards a profile persists slider values for. Order here
        // is the default / migration order — `cards()` returns this list
        // to the overlay and the writer. Card membership is compile-time
        // stable: adding a new knob is a one-line append plus the matching
        // shader + effect update.
        const std::vector<CardDef>& cardList()
        {
            static const std::vector<CardDef> list = {
                {
                    "lumen_tonal",
                    "Brightness / Contrast",
                    {
                        {"lumen.exposure",    "Exposure",     "%.0f"},
                        {"lumen.contrast",    "Contrast",     "%.0f"},
                        {"lumen.highlights",  "Highlights",   "%.0f"},
                        {"lumen.shadows",     "Shadows",      "%.0f"},
                        {"lumen.darkShadows", "Deep Shadows", "%.0f"},
                        {"lumen.gamma",       "Gamma",        "%.0f"},
                    },
                },
                {
                    "lumen_color",
                    "Color",
                    {
                        {"lumen.tintColor",     "Tint Color",     "%.0f\xc2\xb0"},
                        {"lumen.tintIntensity", "Tint Intensity", "%.0f"},
                        {"lumen.temperature",   "Temperature",    "%.0f"},
                        {"lumen.vibrance",      "Vibrance",       "%.0f"},
                    },
                },
                {
                    "lumen_local",
                    "Details",
                    {
                        {"lumen.sharpen",   "Sharpen",    "%.0f"},
                        {"lumen.clarity",   "Clarity",    "%.0f"},
                        {"lumen.hdrToning", "HDR Toning", "%.0f"},
                        {"lumen.bloom",     "Bloom",      "%.0f"},
                    },
                },
                {
                    "lumen_stylistic",
                    "Effects",
                    {
                        {"lumen.vignette",    "Vignette",      "%.0f"},
                        {"lumen.bwIntensity", "Black & White", "%.0f"},
                    },
                },
            };
            return list;
        }

        // Reject names that would escape gameDir() or alias it. argv[0]
        // is attacker-influenceable (a launcher can spoof it), and the
        // result is concatenated into ~/.config/lumen/games/<name>/ — so
        // `..`, `.`, empty, or anything containing a path separator
        // would break confinement.
        bool isSafeGameName(const std::string& s)
        {
            if (s.empty() || s == "." || s == "..")
                return false;
            return s.find_first_of("/\\") == std::string::npos
                && s.find('\0') == std::string::npos;
        }

        // Best-effort read of the hosting process's executable basename.
        // /proc/self/cmdline is NUL-delimited; argv[0] is before the first
        // NUL. For Wine/Proton games argv[0] is typically the .exe, which
        // we strip. Falls back to /proc/self/comm (15-char truncated) and
        // then a literal "default" if both fail or yield an unsafe name.
        std::string detectGameName()
        {
            std::ifstream cmdline("/proc/self/cmdline");
            if (cmdline.good())
            {
                std::string content((std::istreambuf_iterator<char>(cmdline)),
                                    std::istreambuf_iterator<char>());
                size_t nul = content.find('\0');
                if (nul != std::string::npos)
                    content.resize(nul);
                size_t slash = content.find_last_of('/');
                if (slash != std::string::npos)
                    content = content.substr(slash + 1);
                if (content.size() > 4
                    && (content.substr(content.size() - 4) == ".exe"
                     || content.substr(content.size() - 4) == ".EXE"))
                    content.resize(content.size() - 4);
                if (isSafeGameName(content))
                    return content;
            }
            std::ifstream comm("/proc/self/comm");
            std::string name;
            if (comm.good() && std::getline(comm, name) && isSafeGameName(name))
                return name;
            return "default";
        }

        fs::path gffConfigRoot()
        {
            const char* xdg = std::getenv("XDG_CONFIG_HOME");
            if (xdg && *xdg)
                return fs::path(xdg) / "lumen";
            const char* home = std::getenv("HOME");
            if (home && *home)
                return fs::path(home) / ".config" / "lumen";
            return fs::path("/tmp") / "lumen";
        }

        fs::path gameDir(const std::string& gameName)
        {
            return gffConfigRoot() / "games" / gameName;
        }

        fs::path profilePath(const std::string& gameName, int n)
        {
            n = std::clamp(n, 1, 3);
            return gameDir(gameName) / ("profile" + std::to_string(n) + ".conf");
        }

        fs::path activeMarkerPath(const std::string& gameName)
        {
            return gameDir(gameName) / "active.txt";
        }

        // Read a `key = value` .conf file into a flat map. Shares the
        // minimal parser contract with layer/src/config.cpp — whitespace
        // around `=` is ignored, `#` starts a line comment.
        std::map<std::string, std::string> parseConf(const fs::path& path)
        {
            std::map<std::string, std::string> out;
            std::ifstream in(path);
            if (!in.good())
                return out;
            std::string line;
            while (std::getline(in, line))
            {
                auto hash = line.find('#');
                if (hash != std::string::npos)
                    line.resize(hash);
                auto eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                std::string k = line.substr(0, eq);
                std::string v = line.substr(eq + 1);
                auto trim = [](std::string& s) {
                    auto l = s.find_first_not_of(" \t\r\n");
                    auto r = s.find_last_not_of(" \t\r\n");
                    if (l == std::string::npos) { s.clear(); return; }
                    s = s.substr(l, r - l + 1);
                };
                trim(k);
                trim(v);
                if (!k.empty())
                    out[k] = v;
            }
            return out;
        }

        // Parse a colon-separated `effects = a:b:c` value into a vector.
        // Whitespace around each entry is trimmed; empty entries are
        // skipped.
        //
        // Two legacy-rename cases are migrated silently to preserve user
        // profiles across the GameFiltersFlatpak → Lumen transition:
        //   1. Single-name `gff_pipeline` (the v1 pre-split combined effect) →
        //      the four-effect Lumen chain.
        //   2. Individual `gff_{local,tonal,color,stylistic}` entries (the
        //      post-split GameFiltersFlatpak chain) → their `lumen_*` equivalents.
        std::vector<std::string> parseChain(const std::string& raw, bool& outMigrated)
        {
            outMigrated = false;
            if (raw == "gff_pipeline")
            {
                outMigrated = true;
                return {"lumen_local", "lumen_tonal", "lumen_color", "lumen_stylistic"};
            }
            std::vector<std::string> result;
            std::stringstream ss(raw);
            std::string item;
            while (std::getline(ss, item, ':'))
            {
                auto l = item.find_first_not_of(" \t");
                auto r = item.find_last_not_of(" \t");
                if (l == std::string::npos)
                    continue;
                std::string name = item.substr(l, r - l + 1);
                // Per-entry legacy migration: GameFiltersFlatpak-era effect
                // names had a `gff_` prefix. Rewrite to the Lumen-era `lumen_`
                // prefix so the chain still resolves to registered effects
                // after the rename.
                if (name.rfind("gff_", 0) == 0)
                {
                    name = "lumen_" + name.substr(4);
                    outMigrated = true;
                }
                result.push_back(name);
            }
            return result;
        }

        std::string joinChain(const std::vector<std::string>& chain)
        {
            std::string out;
            for (size_t i = 0; i < chain.size(); ++i)
            {
                if (i > 0)
                    out += ':';
                out += chain[i];
            }
            return out;
        }

        // Write the current registry state to a .conf file:
        //   - `effects = a:b:c` with the current chain order
        //   - every slider's current value (across all four cards, regardless
        //     of whether the card is active right now — this preserves the
        //     values so re-adding a removed card restores them)
        void writeProfileFile(const fs::path& path, vkBasalt::EffectRegistry* reg)
        {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::trunc);
            if (!out.good())
            {
                vkBasalt::Logger::warn("profile: cannot write " + path.string());
                return;
            }
            out << "# Lumen profile\n";
            out << "effects = " << joinChain(reg->getSelectedEffects()) << "\n";
            for (const auto& card : cardList())
            {
                for (const auto& s : card.sliders)
                {
                    auto* param = reg->getParameter(card.effectName, s.key);
                    if (!param)
                        continue;
                    auto* fp = dynamic_cast<vkBasalt::FloatParam*>(param);
                    if (!fp)
                        continue;
                    out << s.key << " = " << vkBasalt::floatToString(fp->value) << "\n";
                }
            }
        }

        // Seed a brand-new profile file: empty chain (no filters active,
        // matching the Freestyle "start with nothing" UX) and all sliders
        // at zero. Used by initializeForGame when a profile slot is missing.
        void writeInitialProfileFile(const fs::path& path)
        {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::trunc);
            if (!out.good())
            {
                vkBasalt::Logger::warn("profile: cannot write " + path.string());
                return;
            }
            out << "# Lumen profile\n";
            out << "effects = \n";
            for (const auto& card : cardList())
                for (const auto& s : card.sliders)
                    out << s.key << " = 0.0\n";
        }

        void writeActiveMarker(const std::string& gameName, int n)
        {
            std::error_code ec;
            fs::create_directories(gameDir(gameName), ec);
            std::ofstream out(activeMarkerPath(gameName), std::ios::trunc);
            if (out.good())
                out << std::clamp(n, 1, 3);
        }

        int readActiveMarker(const std::string& gameName)
        {
            std::ifstream in(activeMarkerPath(gameName));
            int n = 1;
            if (in.good())
                in >> n;
            return std::clamp(n, 1, 3);
        }

        // Apply values from `vals` into both the EffectRegistry (so the UI
        // reflects them) and the Config override map (so the next effect
        // rebuild reads the right numbers). Also applies the chain order
        // from the `effects` key, with legacy-migration for profiles that
        // still use the `gff_pipeline` / `gff_*` / `gff.*` GameFiltersFlatpak-
        // era naming.
        //
        // Returns true if legacy migration was performed — the caller
        // should follow up with writeProfileFile(path, reg) to persist
        // the migrated form so it doesn't re-migrate on every launch.
        bool applyValuesToRegistryAndConfig(const std::map<std::string, std::string>& vals,
                                            vkBasalt::EffectRegistry*                  reg)
        {
            auto* cfg = reg->getConfig();
            if (cfg)
                cfg->clearOverrides();

            // Chain (active effects). No `effects` key = empty chain =
            // pass-through (Freestyle "no filters active" default).
            bool migrated = false;
            std::vector<std::string> chain;
            if (auto it = vals.find("effects"); it != vals.end())
                chain = parseChain(it->second, migrated);
            reg->setSelectedEffects(chain);
            // Ensure each effect has an EffectConfig entry in the registry
            // before we try to set its parameters below — setSelectedEffects
            // only updates the ordered name list, and upstream's chain
            // builder gates on isEffectEnabled (which returns false for
            // unregistered effects, producing a silent pass-through chain).
            // Mirrors the pattern in initializeSelectedEffectsFromConfig.
            for (const auto& name : chain)
                reg->ensureEffect(name);

            // Per-card slider values. Written for every card regardless of
            // whether the card is currently active, so re-adding a removed
            // card restores its last-known values. Falls back to the legacy
            // `gff.*` slider key if the current `lumen.*` key isn't found —
            // this is how GameFiltersFlatpak-era profiles keep their slider
            // values across the Lumen rename.
            for (const auto& card : cardList())
            {
                for (const auto& s : card.sliders)
                {
                    float value = 0.0f;
                    auto it = vals.find(s.key);
                    if (it == vals.end())
                    {
                        // Legacy `gff.<suffix>` fallback for the `lumen.<suffix>` key.
                        std::string legacyKey = s.key;
                        if (legacyKey.rfind("lumen.", 0) == 0)
                        {
                            legacyKey = "gff." + legacyKey.substr(std::strlen("lumen."));
                            it = vals.find(legacyKey);
                            if (it != vals.end())
                                migrated = true;
                        }
                    }
                    if (it != vals.end())
                    {
                        std::stringstream ss(it->second);
                        ss.imbue(std::locale::classic());
                        ss >> value;
                    }
                    // Reject malformed .conf values — NaN/Inf flow directly
                    // into shader spec constants and yield undefined GPU
                    // math. Clamp in-range too so a profile pack with out-
                    // of-slider values can't drive the shader off its
                    // calibrated curve.
                    if (!std::isfinite(value))
                        value = 0.0f;
                    if (auto* param = reg->getParameter(card.effectName, s.key);
                        param && param->getType() == vkBasalt::ParamType::Float)
                    {
                        auto* fp = static_cast<vkBasalt::FloatParam*>(param);
                        value = std::clamp(value, fp->minValue, fp->maxValue);
                    }
                    reg->setParameterValue(card.effectName, s.key, value);
                    if (cfg)
                        cfg->setOverride(s.key, vkBasalt::floatToString(value));
                }
            }
            if (migrated)
                vkBasalt::Logger::info("profile: migrating legacy GameFiltersFlatpak state -> Lumen");
            return migrated;
        }

        // Load a profile from disk, apply, and persist the migrated form
        // back to disk if legacy migration was triggered.
        void loadProfileFromDisk(const fs::path& path, vkBasalt::EffectRegistry* reg)
        {
            auto vals = parseConf(path);
            if (applyValuesToRegistryAndConfig(vals, reg))
                writeProfileFile(path, reg);
        }
    } // namespace

    const std::vector<CardDef>& cards() { return cardList(); }

    const ProfileState& state() { return g_state; }

    bool frontendAvailable()
    {
        // Cache the probe result for a short window. Games routinely
        // recreate swapchains in bursts during startup and mode changes,
        // and the Wine/Proton launch sequence spawns many short-lived
        // helper processes that each call this. Without caching we'd open
        // a connect-and-close socket per call — dozens per second into the
        // frontend's listener, showing up as a "early eof" storm in the
        // frontend log and drowning out any real signal. 500 ms is short
        // enough that the user closing the frontend mid-game still goes
        // neutral within ~half a second, long enough to collapse a burst
        // into one probe.
        using namespace std::chrono;
        static std::mutex                          cacheMutex;
        static steady_clock::time_point            lastCheck{};
        static bool                                cachedResult = false;
        static bool                                hasCache     = false;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            if (hasCache
                && duration_cast<milliseconds>(steady_clock::now() - lastCheck).count() < 500)
                return cachedResult;
        }

        // Try the filesystem path first, then the abstract socket. Same
        // logic as IpcClient::connectToServer, condensed.
        const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
        std::string fsPath     = (runtimeDir ? runtimeDir : "/tmp");
        fsPath += "/";
        fsPath += kSocketName;

        auto tryConnect = [](bool abstractNs, const std::string& path) {
            int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0)
                return false;
            sockaddr_un addr{};
            addr.sun_family   = AF_UNIX;
            socklen_t addrLen = sizeof(addr.sun_family);
            if (abstractNs)
            {
                const size_t nameLen = std::strlen(kSocketName);
                if (1 + nameLen > sizeof(addr.sun_path))
                {
                    ::close(fd);
                    return false;
                }
                addr.sun_path[0] = '\0';
                std::memcpy(addr.sun_path + 1, kSocketName, nameLen);
                addrLen += 1 + nameLen;
            }
            else
            {
                if (path.size() >= sizeof(addr.sun_path))
                {
                    ::close(fd);
                    return false;
                }
                std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
                addrLen += path.size();
            }
            int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), addrLen);
            ::close(fd);
            return rc == 0;
        };

        bool result = tryConnect(false, fsPath) || tryConnect(true, "");

        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            cachedResult = result;
            lastCheck    = steady_clock::now();
            hasCache     = true;
        }
        return result;
    }

    // --- Enabled-games cache (populated by IPC) -------------------------
    //
    // The frontend broadcasts a `games-enabled-update` message on every
    // client connect and on every toggle change. We store the two id
    // classes separately so the lookup path in isGameEnabled() is O(log n)
    // on each (typically tens of entries max). Guarded by a std::mutex so
    // the IPC thread's writes race-free with the Vulkan thread's reads
    // inside vkCreateSwapchainKHR.
    namespace
    {
        std::mutex            g_enabledMutex;
        std::set<std::string> g_enabledSteamApps;
        std::set<std::string> g_enabledExeBasenames;

        // Basename of the first NUL-terminated token of /proc/self/cmdline.
        // Used for exe-path matching; we match on basename (not full path)
        // because Wine/Proton rewrites argv[0] into a Windows-style path
        // like `Z:\games\foo.exe` that won't match the Linux source path
        // the user picked in the scanner. Basenames are stable across
        // that rewrite.
        std::string currentExeBasename()
        {
            std::ifstream cmdline("/proc/self/cmdline");
            if (!cmdline.good())
                return {};
            std::string content((std::istreambuf_iterator<char>(cmdline)),
                                std::istreambuf_iterator<char>());
            size_t nul = content.find('\0');
            if (nul != std::string::npos)
                content.resize(nul);
            // Split on both / and \ so Linux and Windows-style paths both
            // yield the right basename.
            size_t slash = content.find_last_of("/\\");
            if (slash == std::string::npos)
                return content;
            return content.substr(slash + 1);
        }

        std::string pathBasename(const std::string& p)
        {
            size_t slash = p.find_last_of("/\\");
            return (slash == std::string::npos) ? p : p.substr(slash + 1);
        }

        // Pull a string value out of a JSON fragment by key. Minimal — no
        // escape handling beyond what the frontend actually emits (serde's
        // default string serialization). Sufficient for kind / value extraction.
        std::string extractJsonString(const std::string& obj, const std::string& key)
        {
            std::string needle = "\"" + key + "\"";
            size_t k = obj.find(needle);
            if (k == std::string::npos)
                return {};
            k = obj.find(':', k + needle.size());
            if (k == std::string::npos)
                return {};
            k = obj.find('"', k);
            if (k == std::string::npos)
                return {};
            size_t end = obj.find('"', k + 1);
            if (end == std::string::npos)
                return {};
            return obj.substr(k + 1, end - k - 1);
        }
    } // namespace

    void applyEnabledGamesJson(const std::string& rawJson)
    {
        // Shape we're parsing:
        //   {"type":"games-enabled-update","enabled":[{"kind":"SteamApp","value":"..."},{"kind":"Executable","value":"/..."}]}
        std::set<std::string> steamApps;
        std::set<std::string> exeBasenames;

        size_t pos = rawJson.find("\"enabled\"");
        if (pos == std::string::npos)
        {
            // No enabled list; treat as empty (everything off).
        }
        else
        {
            size_t arrStart = rawJson.find('[', pos);
            size_t arrEnd   = (arrStart == std::string::npos) ? std::string::npos
                                                              : rawJson.find(']', arrStart);
            if (arrStart != std::string::npos && arrEnd != std::string::npos)
            {
                size_t cur = arrStart + 1;
                while (cur < arrEnd)
                {
                    size_t objStart = rawJson.find('{', cur);
                    if (objStart == std::string::npos || objStart >= arrEnd)
                        break;
                    size_t objEnd = rawJson.find('}', objStart);
                    if (objEnd == std::string::npos || objEnd >= arrEnd)
                        break;
                    std::string obj = rawJson.substr(objStart, objEnd - objStart + 1);
                    std::string kind  = extractJsonString(obj, "kind");
                    std::string value = extractJsonString(obj, "value");
                    if (kind == "SteamApp" && !value.empty())
                        steamApps.insert(std::move(value));
                    else if (kind == "Executable" && !value.empty())
                        exeBasenames.insert(pathBasename(value));
                    cur = objEnd + 1;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_enabledMutex);
            g_enabledSteamApps    = std::move(steamApps);
            g_enabledExeBasenames = std::move(exeBasenames);
        }
        vkBasalt::Logger::info("ipc: games-enabled-update received — "
                               + std::to_string(g_enabledSteamApps.size()) + " appids + "
                               + std::to_string(g_enabledExeBasenames.size()) + " exe basenames");
    }

    bool isGameEnabled()
    {
        std::lock_guard<std::mutex> lock(g_enabledMutex);
        if (const char* sid = std::getenv("SteamAppId"))
        {
            if (g_enabledSteamApps.count(sid))
                return true;
        }
        const std::string base = currentExeBasename();
        if (!base.empty() && g_enabledExeBasenames.count(base))
            return true;
        return false;
    }

    bool isGameProcess()
    {
        // Computed once per process. argv[0] / env don't change at runtime
        // and a false negative here means the user's filters don't apply;
        // we'd rather decide fast and be consistent than re-check.
        static const bool cached = [] {
            std::ifstream cmdline("/proc/self/cmdline");
            if (cmdline.good())
            {
                std::string content((std::istreambuf_iterator<char>(cmdline)),
                                    std::istreambuf_iterator<char>());
                size_t nul = content.find('\0');
                if (nul != std::string::npos)
                    content.resize(nul);
                if (content.size() >= 4)
                {
                    std::string tail = content.substr(content.size() - 4);
                    if (tail == ".exe" || tail == ".EXE")
                        return true;
                }
            }
            // Steam sets these for everything it launches — native Linux
            // games included. A process carrying them is something the
            // user opted into via their library, so apply filters.
            if (std::getenv("SteamAppId"))            return true;
            if (std::getenv("SteamGameId"))           return true;
            if (std::getenv("STEAM_COMPAT_DATA_PATH")) return true;
            return false;
        }();
        return cached;
    }

    void initializeForGame(vkBasalt::EffectRegistry* reg, vkBasalt::ImGuiOverlay* overlay)
    {
        if (g_initialized)
            return;
        g_initialized = true;
        if (!reg)
            return;

        // One-shot legacy migration: if the user has a
        // `~/.config/game-filters-flatpak/` tree from the pre-rename era but
        // the new `~/.config/lumen/` tree doesn't exist yet, copy everything
        // over. Subsequent slider writes land in the new tree and keep it
        // current; the legacy tree is left in place as a manual backup.
        {
            const char* xdg = std::getenv("XDG_CONFIG_HOME");
            const char* home = std::getenv("HOME");
            fs::path configBase;
            if (xdg && *xdg)
                configBase = fs::path(xdg);
            else if (home && *home)
                configBase = fs::path(home) / ".config";
            if (!configBase.empty())
            {
                fs::path legacyRoot = configBase / "game-filters-flatpak";
                fs::path newRoot    = configBase / "lumen";
                std::error_code existsEc;
                if (fs::exists(legacyRoot, existsEc) && !fs::exists(newRoot, existsEc))
                {
                    std::error_code copyEc;
                    fs::create_directories(newRoot.parent_path(), copyEc);
                    fs::copy(legacyRoot, newRoot,
                             fs::copy_options::recursive
                               | fs::copy_options::skip_existing,
                             copyEc);
                    if (!copyEc)
                        vkBasalt::Logger::info("profile: migrated legacy GameFiltersFlatpak data -> "
                                               + newRoot.string());
                    else
                        vkBasalt::Logger::warn("profile: legacy-dir copy failed: "
                                               + copyEc.message());
                }
            }
        }

        g_state.gameName = detectGameName();
        std::error_code ec;
        fs::create_directories(gameDir(g_state.gameName), ec);

        // Create any missing profile files so the UI always has three valid
        // slots to show. New files start empty (no filters active, all
        // sliders zero) — matches Freestyle's "start from scratch" UX.
        for (int n = 1; n <= 3; ++n)
        {
            auto path = profilePath(g_state.gameName, n);
            if (!fs::exists(path))
                writeInitialProfileFile(path);
        }

        g_state.activeProfile = readActiveMarker(g_state.gameName);

        // Frontend-gating: if the user closed the frontend, the layer goes
        // pass-through. This matches Nvidia's "filter app must be running"
        // behavior and gives users a kill-switch (Tray → Quit). The IPC
        // connection handler in basalt.cpp re-applies the active profile
        // when the frontend comes back online.
        const bool feOk = frontendAvailable();
        if (feOk)
        {
            loadProfileFromDisk(profilePath(g_state.gameName, g_state.activeProfile), reg);
        }
        else
        {
            applyValuesToRegistryAndConfig({}, reg);
        }
        if (overlay)
            overlay->markDirty();

        vkBasalt::Logger::info(std::string("profile: game=") + g_state.gameName
                               + " active=" + std::to_string(g_state.activeProfile)
                               + " frontend=" + (feOk ? "connected" : "absent")
                               + " dir=" + gameDir(g_state.gameName).string());
    }

    void switchProfile(int n, vkBasalt::EffectRegistry* reg, vkBasalt::ImGuiOverlay* overlay)
    {
        n = std::clamp(n, 1, 3);
        if (!reg)
            return;
        // Persist the profile we're about to leave so any unsaved slider
        // wiggle from the last few ms is preserved. (saveActiveProfile is
        // called on every drag, but there's a short debounce window where
        // the profile file might not yet reflect the latest slider.)
        saveActiveProfile(reg);

        g_state.activeProfile = n;
        writeActiveMarker(g_state.gameName, n);

        loadProfileFromDisk(profilePath(g_state.gameName, n), reg);
        // Without markDirty the spec constants on the existing pipeline
        // never get rebaked, so the new values would only show in the UI.
        if (overlay)
            overlay->markDirty();
        vkBasalt::Logger::info("profile: switched to profile " + std::to_string(n));
    }

    void saveActiveProfile(vkBasalt::EffectRegistry* reg)
    {
        if (!reg || g_state.gameName.empty())
            return;
        writeProfileFile(profilePath(g_state.gameName, g_state.activeProfile), reg);
    }

    void applyActiveProfile(vkBasalt::EffectRegistry* reg, vkBasalt::ImGuiOverlay* overlay)
    {
        if (!reg || g_state.gameName.empty())
            return;
        loadProfileFromDisk(profilePath(g_state.gameName, g_state.activeProfile), reg);
        if (overlay)
            overlay->markDirty();
    }

    void applyNeutral(vkBasalt::EffectRegistry* reg, vkBasalt::ImGuiOverlay* overlay)
    {
        if (!reg)
            return;
        applyValuesToRegistryAndConfig({}, reg);
        if (overlay)
            overlay->markDirty();
    }

    void addCard(const std::string&       effectName,
                 vkBasalt::EffectRegistry* reg,
                 vkBasalt::ImGuiOverlay*   overlay)
    {
        if (!reg)
            return;
        auto chain = reg->getSelectedEffects();
        if (std::find(chain.begin(), chain.end(), effectName) != chain.end())
            return;
        chain.push_back(effectName);
        reg->setSelectedEffects(chain);
        // Materialize the EffectConfig so isEffectEnabled reports true for
        // the new card when basalt.cpp's debounced reload runs; otherwise
        // the chain rebuild filters it out and the overlay looks active
        // but rendering stays pass-through.
        reg->ensureEffect(effectName);
        saveActiveProfile(reg);
        if (overlay)
            overlay->markDirty();
        vkBasalt::Logger::info("cards: activated " + effectName
                               + " (chain=" + joinChain(chain) + ")");
    }

    void removeCard(const std::string&       effectName,
                    vkBasalt::EffectRegistry* reg,
                    vkBasalt::ImGuiOverlay*   overlay)
    {
        if (!reg)
            return;
        auto chain = reg->getSelectedEffects();
        auto it = std::find(chain.begin(), chain.end(), effectName);
        if (it == chain.end())
            return;
        chain.erase(it);
        reg->setSelectedEffects(chain);
        saveActiveProfile(reg);
        if (overlay)
            overlay->markDirty();
        vkBasalt::Logger::info("cards: removed " + effectName
                               + " (chain=" + joinChain(chain) + ")");
    }

    void moveCard(const std::string&       effectName,
                  int                       delta,
                  vkBasalt::EffectRegistry* reg,
                  vkBasalt::ImGuiOverlay*   overlay)
    {
        if (!reg || delta == 0)
            return;
        auto chain = reg->getSelectedEffects();
        auto it = std::find(chain.begin(), chain.end(), effectName);
        if (it == chain.end())
            return;
        int idx    = static_cast<int>(it - chain.begin());
        int target = idx + delta;
        if (target < 0 || target >= static_cast<int>(chain.size()))
            return;
        std::swap(chain[idx], chain[target]);
        reg->setSelectedEffects(chain);
        saveActiveProfile(reg);
        if (overlay)
            overlay->markDirty();
        vkBasalt::Logger::info(std::string("cards: moved ") + effectName
                               + (delta < 0 ? " up" : " down")
                               + " (chain=" + joinChain(chain) + ")");
    }
} // namespace lumen
