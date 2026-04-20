#include "ipc.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "logger.hpp"

namespace gff
{
    namespace
    {
        constexpr uint32_t kMaxPayload = 64 * 1024;

        std::string socketPath()
        {
            const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
            std::string base       = runtimeDir ? runtimeDir : "/tmp";
            return base + "/" + kSocketName;
        }

        bool readExact(int fd, void* data, size_t len)
        {
            char* p = static_cast<char*>(data);
            while (len > 0)
            {
                ssize_t n = ::read(fd, p, len);
                if (n == 0)
                    return false;
                if (n < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return false;
                }
                p += n;
                len -= static_cast<size_t>(n);
            }
            return true;
        }

        std::string peekType(const std::string& json)
        {
            const std::string key = "\"type\"";
            auto pos              = json.find(key);
            if (pos == std::string::npos)
                return {};
            pos = json.find(':', pos + key.size());
            if (pos == std::string::npos)
                return {};
            pos = json.find('"', pos);
            if (pos == std::string::npos)
                return {};
            auto end = json.find('"', pos + 1);
            if (end == std::string::npos)
                return {};
            return json.substr(pos + 1, end - pos - 1);
        }
    } // namespace

    IpcClient::IpcClient() = default;

    IpcClient::~IpcClient()
    {
        m_running.store(false, std::memory_order_release);
        if (m_thread.joinable())
            m_thread.join();
    }

    bool IpcClient::start(Handler handler, ConnectionHandler connHandler)
    {
        if (m_running.load(std::memory_order_acquire))
            return true;
        m_handler     = std::move(handler);
        m_connHandler = std::move(connHandler);
        m_socketPath  = socketPath();
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread(&IpcClient::run, this);
        return true;
    }

    void IpcClient::run()
    {
        // Frontend may come up after the layer, or may disappear and come back.
        // Loop: connect → read commands until EOF → back off → reconnect.
        int attempt = 0;
        while (m_running.load(std::memory_order_acquire))
        {
            int fd        = -1;
            int savedErrno = 0;
            if (!connectToServer(fd, savedErrno))
            {
                // Log first failure + every 30 failures (~30s) so we don't
                // drown the log, but the problem is diagnosable.
                if (attempt == 0 || attempt % 30 == 0)
                {
                    vkBasalt::Logger::warn(
                        "ipc: connect() failed (attempt " + std::to_string(attempt + 1) +
                        "): " + std::strerror(savedErrno) + " — trying path " + m_socketPath);
                }
                attempt++;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            attempt = 0;
            vkBasalt::Logger::info("ipc: connected to frontend at " + m_socketPath);
            if (m_connHandler)
                m_connHandler(true);
            readLoop(fd);
            ::close(fd);
            if (m_connHandler)
                m_connHandler(false);
            if (m_running.load(std::memory_order_acquire))
                vkBasalt::Logger::info("ipc: disconnected; will retry");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    // Try to connect to the frontend's IPC server. We try the filesystem path
    // first (works when frontend and game share the same XDG_RUNTIME_DIR), and
    // fall back to an abstract Linux socket name if that fails — abstract
    // sockets live in a flat kernel namespace and cross pressure-vessel /
    // bubblewrap sandbox boundaries that isolate /run/user filesystem views.
    bool IpcClient::connectToServer(int& outFd, int& savedErrno)
    {
        auto attemptPath = [&](bool abstractNs) -> bool {
            int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0) { savedErrno = errno; return false; }

            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;

            socklen_t addrLen = sizeof(addr.sun_family);
            if (abstractNs)
            {
                // Abstract socket name: first byte of sun_path is NUL, then
                // the name itself. The name is not NUL-terminated.
                const size_t nameLen = std::strlen(kSocketName);
                if (1 + nameLen > sizeof(addr.sun_path))
                {
                    ::close(fd);
                    savedErrno = ENAMETOOLONG;
                    return false;
                }
                addr.sun_path[0] = '\0';
                std::memcpy(addr.sun_path + 1, kSocketName, nameLen);
                addrLen += 1 + nameLen;
            }
            else
            {
                if (m_socketPath.size() >= sizeof(addr.sun_path))
                {
                    ::close(fd);
                    savedErrno = ENAMETOOLONG;
                    return false;
                }
                std::strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);
                addrLen += m_socketPath.size();
            }

            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), addrLen) < 0)
            {
                savedErrno = errno;
                ::close(fd);
                return false;
            }
            outFd = fd;
            return true;
        };

        if (attemptPath(false))
            return true;
        // Filesystem path failed — try abstract. Don't overwrite savedErrno
        // from the abstract attempt if that one fails too; callers find the
        // first error most useful.
        int firstErrno = savedErrno;
        if (attemptPath(true))
            return true;
        savedErrno = firstErrno;
        return false;
    }

    void IpcClient::readLoop(int fd)
    {
        while (m_running.load(std::memory_order_acquire))
        {
            uint32_t len_le = 0;
            if (!readExact(fd, &len_le, sizeof(len_le)))
                return;
            if (len_le > kMaxPayload)
            {
                vkBasalt::Logger::warn("ipc: payload too large: " + std::to_string(len_le));
                return;
            }
            std::string body(len_le, '\0');
            if (!readExact(fd, body.data(), len_le))
                return;
            FrontendCommand cmd;
            cmd.type     = peekType(body);
            cmd.raw_json = std::move(body);
            if (m_handler)
                m_handler(cmd);
        }
    }
} // namespace gff
