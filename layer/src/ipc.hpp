#ifndef LUMEN_IPC_HPP_INCLUDED
#define LUMEN_IPC_HPP_INCLUDED

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace lumen
{
    // Canonical socket name, shared by the filesystem path
    // (`$XDG_RUNTIME_DIR/<name>`) and the Linux abstract namespace
    // (`@<name>`). Frontend (Rust), scripts/run.sh, and docs all hardcode
    // the same literal — if this ever changes, grep the whole repo.
    inline constexpr const char* kSocketName = "lumen.sock";

    // A command received from the frontend. `raw_json` is the untouched
    // payload in case the handler wants to parse more fields than just `type`.
    struct FrontendCommand
    {
        std::string type;
        std::string raw_json;
    };

    // Connects as a client to the frontend's IPC server. Every Vulkan process
    // that loads the layer runs one of these. The frontend broadcasts commands
    // to all connected clients; whichever process has actual rendering picks
    // up the toggle and flips its overlay.
    class IpcClient
    {
    public:
        using Handler           = std::function<void(const FrontendCommand&)>;
        using ConnectionHandler = std::function<void(bool connected)>;

        IpcClient();
        ~IpcClient();

        IpcClient(const IpcClient&)            = delete;
        IpcClient& operator=(const IpcClient&) = delete;

        // Spawn the reader thread. Idempotent.
        // `connHandler` (optional) is called from the IPC thread on every
        // connect (true) and disconnect (false) edge — useful for the
        // "frontend is alive" gate that controls whether the layer applies
        // saved profile values or stays pass-through.
        bool start(Handler handler, ConnectionHandler connHandler = nullptr);

        bool isRunning() const { return m_running.load(std::memory_order_acquire); }

    private:
        void run();
        bool connectToServer(int& outFd, int& savedErrno);
        void readLoop(int fd);

        Handler            m_handler;
        ConnectionHandler  m_connHandler;
        std::atomic<bool>  m_running{false};
        std::thread        m_thread;
        std::string        m_socketPath;
    };
} // namespace lumen

#endif // LUMEN_IPC_HPP_INCLUDED
