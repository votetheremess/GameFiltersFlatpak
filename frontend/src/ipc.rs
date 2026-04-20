use std::path::PathBuf;
use std::sync::Arc;

use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixListener;
use tokio::sync::{broadcast, mpsc, Mutex};

// Protocol invariant — must match `kSocketName` in layer/src/ipc.hpp and
// the `SOCKET` variable in scripts/run.sh. Used for both the filesystem
// path (`$XDG_RUNTIME_DIR/<name>`) and the abstract namespace (`@<name>`).
const SOCKET_NAME: &str = "game-filters-flatpak.sock";

use crate::messages::{FrontendCommand, LayerEvent};

const MAX_PAYLOAD: u32 = 64 * 1024;
// 256 slots so a burst of `param-updated` events during active slider
// drags doesn't cause `Lagged` drops on clients with many connected
// layers (KWin + Steam UI + game + every Proton helper).
const BROADCAST_CAP: usize = 256;
// Once a client has committed to sending a frame (length prefix read),
// the body must arrive quickly. Without this cap a client that stalls
// between prefix and body pins a tokio task indefinitely.
const BODY_READ_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(5);

#[derive(thiserror::Error, Debug)]
pub enum IpcError {
    #[error("I/O: {0}")]
    Io(#[from] std::io::Error),
    #[error("JSON: {0}")]
    Json(#[from] serde_json::Error),
    #[error("payload {0} exceeds max {MAX_PAYLOAD}")]
    PayloadTooLarge(u32),
}

/// Handle given to UI code. Internally owns a broadcast sender — every
/// layer that connects gets a tokio task subscribed to the channel, so
/// UI-side `send()` reaches all layers simultaneously.
#[derive(Clone)]
pub struct Client {
    tx: broadcast::Sender<FrontendCommand>,
    #[allow(dead_code)]
    events: Arc<Mutex<mpsc::Receiver<LayerEvent>>>,
}

impl Client {
    pub fn spawn() -> Self {
        let (cmd_tx, _cmd_rx) = broadcast::channel::<FrontendCommand>(BROADCAST_CAP);
        let (evt_tx, evt_rx) = mpsc::channel::<LayerEvent>(32);

        let cmd_tx_for_task = cmd_tx.clone();
        std::thread::Builder::new()
            .name("gff-ipc".to_owned())
            .spawn(move || {
                let rt = tokio::runtime::Builder::new_current_thread()
                    .enable_all()
                    .build()
                    .expect("tokio rt");
                rt.block_on(run_server(cmd_tx_for_task, evt_tx));
            })
            .expect("spawn ipc thread");

        Client {
            tx: cmd_tx,
            events: Arc::new(Mutex::new(evt_rx)),
        }
    }

    pub fn send(&self, cmd: FrontendCommand) {
        match self.tx.send(cmd) {
            Ok(n) => log::debug!("ipc: broadcast to {n} layer client(s)"),
            Err(_) => log::debug!("ipc: no layer clients connected; dropping command"),
        }
    }
}

fn socket_path() -> PathBuf {
    let runtime_dir = std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp"));
    runtime_dir.join(SOCKET_NAME)
}

/// RAII guard that removes the socket file when dropped. Runs whenever the
/// server task unwinds — normal tokio shutdown, panic, or process exit via
/// signals we handle.
struct SocketGuard(PathBuf);

impl Drop for SocketGuard {
    fn drop(&mut self) {
        match std::fs::remove_file(&self.0) {
            Ok(()) => log::info!("ipc: cleaned up socket {}", self.0.display()),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => log::warn!("ipc: failed to remove {}: {e}", self.0.display()),
        }
    }
}

async fn run_server(
    cmd_tx: broadcast::Sender<FrontendCommand>,
    evt_tx: mpsc::Sender<LayerEvent>,
) {
    // 1. Filesystem socket — normal Unix path; works for host-only games and
    //    any Flatpak that shares our xdg-run path.
    let path = socket_path();
    if path.exists() {
        if let Err(e) = std::fs::remove_file(&path) {
            log::warn!("ipc: couldn't clear stale socket {}: {e}", path.display());
        }
    }
    let fs_listener = match UnixListener::bind(&path) {
        Ok(l) => l,
        Err(e) => {
            log::error!("ipc: cannot bind {}: {e}", path.display());
            return;
        }
    };
    let _guard = SocketGuard(path.clone());
    log::info!("ipc: listening on fs path {}", path.display());

    // 2. Abstract socket — Linux-only, kernel namespace, no filesystem entry.
    //    Crosses pressure-vessel / bubblewrap sandbox boundaries that hide
    //    /run/user/1000 from Steam Proton games. The layer tries both.
    let abstract_listener = match bind_abstract(SOCKET_NAME) {
        Ok(l) => {
            log::info!("ipc: also listening on abstract socket @{}", SOCKET_NAME);
            Some(l)
        }
        Err(e) => {
            log::warn!("ipc: couldn't bind abstract socket: {e}");
            None
        }
    };

    // Intentionally no tokio signal handlers here. Those would race with
    // GTK's own SIGINT/SIGTERM handling: tokio would intercept the signal,
    // return from run_server, drop the guard (unlinking the socket), while
    // the GTK main thread keeps running — leaving the frontend alive but
    // socketless. Cleanup happens through two paths instead:
    //   1. Normal exit → `_guard: SocketGuard` drops.
    //   2. Crash / SIGKILL → run.sh's postflight_cleanup removes the file.
    loop {
        let accept = async {
            tokio::select! {
                a = fs_listener.accept() => a.map(|s| ("fs", s)),
                a = async {
                    match &abstract_listener {
                        Some(l) => l.accept().await,
                        None => std::future::pending().await,
                    }
                } => a.map(|s| ("abstract", s)),
            }
        }
        .await;
        match accept {
            Ok((kind, (stream, _addr))) => {
                log::info!("ipc: layer client connected via {kind} socket");
                let cmd_rx = cmd_tx.subscribe();
                let evt_tx = evt_tx.clone();
                tokio::spawn(async move {
                    if let Err(e) = serve_client(stream, cmd_rx, evt_tx).await {
                        log::debug!("ipc: client session ended: {e}");
                    }
                });
            }
            Err(e) => {
                log::warn!("ipc: accept failed: {e}");
                tokio::time::sleep(std::time::Duration::from_millis(200)).await;
            }
        }
    }
}

/// Bind a Linux abstract Unix socket. No stdlib / tokio helper exists, so
/// we construct sockaddr_un manually: the path starts with a NUL byte and
/// the rest is the name (not NUL-terminated).
fn bind_abstract(name: &str) -> std::io::Result<UnixListener> {
    use std::os::fd::{FromRawFd, OwnedFd};

    let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_STREAM | libc::SOCK_CLOEXEC, 0) };
    if fd < 0 {
        return Err(std::io::Error::last_os_error());
    }
    let owned = unsafe { OwnedFd::from_raw_fd(fd) };

    let mut addr: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    addr.sun_family = libc::AF_UNIX as libc::sa_family_t;
    let name_bytes = name.as_bytes();
    if 1 + name_bytes.len() > addr.sun_path.len() {
        return Err(std::io::Error::new(std::io::ErrorKind::InvalidInput, "name too long"));
    }
    // First byte NUL → abstract namespace.
    addr.sun_path[0] = 0;
    for (i, &b) in name_bytes.iter().enumerate() {
        addr.sun_path[i + 1] = b as libc::c_char;
    }
    let addr_len =
        (std::mem::size_of::<libc::sa_family_t>() + 1 + name_bytes.len()) as libc::socklen_t;

    let rc = unsafe {
        libc::bind(
            fd,
            &addr as *const libc::sockaddr_un as *const libc::sockaddr,
            addr_len,
        )
    };
    if rc != 0 {
        return Err(std::io::Error::last_os_error());
    }
    let rc = unsafe { libc::listen(fd, 16) };
    if rc != 0 {
        return Err(std::io::Error::last_os_error());
    }

    // Convert into a std UnixListener, then tokio UnixListener.
    let fd_raw = owned.into_raw_fd_inner();
    let std_listener = unsafe { std::os::unix::net::UnixListener::from_raw_fd(fd_raw) };
    std_listener.set_nonblocking(true)?;
    UnixListener::from_std(std_listener)
}

trait IntoRawFdInner {
    fn into_raw_fd_inner(self) -> std::os::fd::RawFd;
}
impl IntoRawFdInner for std::os::fd::OwnedFd {
    fn into_raw_fd_inner(self) -> std::os::fd::RawFd {
        use std::os::fd::IntoRawFd;
        self.into_raw_fd()
    }
}

async fn serve_client(
    mut stream: tokio::net::UnixStream,
    mut cmd_rx: broadcast::Receiver<FrontendCommand>,
    evt_tx: mpsc::Sender<LayerEvent>,
) -> Result<(), IpcError> {
    let (mut read_half, mut write_half) = stream.split();

    loop {
        tokio::select! {
            cmd = cmd_rx.recv() => {
                let cmd = match cmd {
                    Ok(c) => c,
                    Err(broadcast::error::RecvError::Lagged(n)) => {
                        log::warn!("ipc: client lagged {n} commands — skipping");
                        continue;
                    }
                    Err(broadcast::error::RecvError::Closed) => return Ok(()),
                };
                let body = serde_json::to_vec(&cmd)?;
                let len = u32::try_from(body.len()).map_err(|_| IpcError::PayloadTooLarge(u32::MAX))?;
                if len > MAX_PAYLOAD {
                    return Err(IpcError::PayloadTooLarge(len));
                }
                write_half.write_all(&len.to_le_bytes()).await?;
                write_half.write_all(&body).await?;
                write_half.flush().await?;
            }
            res = read_frame(&mut read_half) => {
                let payload = res?;
                match serde_json::from_slice::<LayerEvent>(&payload) {
                    Ok(evt) => {
                        if evt_tx.send(evt).await.is_err() {
                            return Ok(());
                        }
                    }
                    Err(e) => log::warn!("malformed layer event: {e}"),
                }
            }
        }
    }
}

async fn read_frame<R: AsyncReadExt + Unpin>(r: &mut R) -> Result<Vec<u8>, IpcError> {
    let mut len_buf = [0u8; 4];
    r.read_exact(&mut len_buf).await?;
    let len = u32::from_le_bytes(len_buf);
    if len > MAX_PAYLOAD {
        return Err(IpcError::PayloadTooLarge(len));
    }
    let mut buf = vec![0u8; len as usize];
    match tokio::time::timeout(BODY_READ_TIMEOUT, r.read_exact(&mut buf)).await {
        Ok(res) => { res?; }
        Err(_) => {
            return Err(IpcError::Io(std::io::Error::new(
                std::io::ErrorKind::TimedOut,
                "body read timed out after length prefix",
            )));
        }
    }
    Ok(buf)
}
