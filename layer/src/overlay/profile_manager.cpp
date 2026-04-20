#include "profile_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
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

namespace gff
{
    namespace fs = std::filesystem;

    namespace
    {
        ProfileState g_state;
        bool         g_initialized = false;

        // The parameter keys the overlay exposes. Keeping this list local
        // (rather than scraping the registry) gives us stable ordering and
        // a fixed persisted schema — adding new params is a one-line change
        // here alongside shader/effect updates.
        const std::vector<std::string> kProfileKeys = {
            "gff.exposure",
            "gff.contrast",
            "gff.highlights",
            "gff.shadows",
            "gff.gamma",
            "gff.tintColor",
            "gff.tintIntensity",
            "gff.temperature",
            "gff.vibrance",
            "gff.sharpen",
            "gff.clarity",
            "gff.hdrToning",
            "gff.bloom",
            "gff.vignette",
            "gff.bwIntensity",
        };

        constexpr const char* kEffectName = "gff_pipeline";

        std::string readFile(const fs::path& path)
        {
            std::ifstream in(path);
            if (!in.good())
                return {};
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        // Reject names that would escape gameDir() or alias it. argv[0]
        // is attacker-influenceable (a launcher can spoof it), and the
        // result is concatenated into ~/.config/game-filters-flatpak/games/<name>/ — so
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
                return fs::path(xdg) / "game-filters-flatpak";
            const char* home = std::getenv("HOME");
            if (home && *home)
                return fs::path(home) / ".config" / "game-filters-flatpak";
            return fs::path("/tmp") / "game-filters-flatpak";
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

        // Write the current registry values for every known GFF param to a
        // .conf file. Ensures the parent directory exists first.
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
            out << "# GameFiltersFlatpak profile\n";
            out << "effects = gff_pipeline\n";
            for (const auto& key : kProfileKeys)
            {
                auto* param = reg->getParameter(kEffectName, key);
                if (!param)
                    continue;
                auto* fp = dynamic_cast<vkBasalt::FloatParam*>(param);
                if (!fp)
                    continue;
                out << key << " = " << vkBasalt::floatToString(fp->value) << "\n";
            }
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
        // rebuild reads the right numbers). Keys not in `vals` reset to 0
        // so switching profiles doesn't leak values from the old one.
        void applyValuesToRegistryAndConfig(const std::map<std::string, std::string>& vals,
                                            vkBasalt::EffectRegistry*                  reg)
        {
            auto* cfg = reg->getConfig();
            if (cfg)
                cfg->clearOverrides();
            for (const auto& key : kProfileKeys)
            {
                float value = 0.0f;
                auto it = vals.find(key);
                if (it != vals.end())
                {
                    std::stringstream ss(it->second);
                    ss.imbue(std::locale::classic());
                    ss >> value;
                }
                // Reject malformed .conf values — NaN/Inf flow directly
                // into shader spec constants and yield undefined GPU math.
                // Clamp in-range too so a profile pack with out-of-slider
                // values can't drive the shader off its calibrated curve.
                if (!std::isfinite(value))
                    value = 0.0f;
                if (auto* param = reg->getParameter(kEffectName, key);
                    param && param->getType() == vkBasalt::ParamType::Float)
                {
                    auto* fp = static_cast<vkBasalt::FloatParam*>(param);
                    value = std::clamp(value, fp->minValue, fp->maxValue);
                }
                reg->setParameterValue(kEffectName, key, value);
                if (cfg)
                    cfg->setOverride(key, vkBasalt::floatToString(value));
            }
        }
    } // namespace

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

        g_state.gameName = detectGameName();
        std::error_code ec;
        fs::create_directories(gameDir(g_state.gameName), ec);

        // Create any missing profile files so the UI always has three valid
        // slots to show. Files that already exist are left alone.
        for (int n = 1; n <= 3; ++n)
        {
            auto path = profilePath(g_state.gameName, n);
            if (!fs::exists(path))
                writeProfileFile(path, reg);
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
            auto vals = parseConf(profilePath(g_state.gameName, g_state.activeProfile));
            applyValuesToRegistryAndConfig(vals, reg);
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

        auto vals = parseConf(profilePath(g_state.gameName, n));
        applyValuesToRegistryAndConfig(vals, reg);
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
        auto vals = parseConf(profilePath(g_state.gameName, g_state.activeProfile));
        applyValuesToRegistryAndConfig(vals, reg);
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
} // namespace gff
