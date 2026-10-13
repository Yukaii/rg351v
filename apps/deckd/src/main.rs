//! deckd: Spotify Connect daemon for the RG351V deck player.
//!
//! Same job as the librespot binary (discovery, cached login, Connect device,
//! ALSA player) plus:
//!   * a Unix control socket (`status`, transport commands, `subscribe`),
//!   * the `/run/rg351v/{playing,stopped}` flags autosleep.py reads,
//!   * restoring playback after the Spotify session drops (a weak Wi-Fi link
//!     makes librespot reconnect as an idle device, which loses the phone's
//!     control and the queue position).
//!
//! Options (all `--key value`):
//!   --name RG351V  --cache DIR  --device default  --sock PATH
//!   --bitrate 96|160|320  --initial-volume 0-100
use std::fs;
use std::io;
use std::path::PathBuf;
use std::pin::Pin;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use futures_util::StreamExt;
use librespot_connect::{
    ConnectConfig, LoadContextOptions, LoadRequest, LoadRequestOptions, Spirc,
};
use librespot_core::authentication::Credentials;
use librespot_core::cache::Cache;
use librespot_core::config::DeviceType;
use librespot_core::{Session, SessionConfig};
use librespot_discovery::Discovery;
use librespot_metadata::audio::UniqueFields;
use librespot_playback::audio_backend;
use librespot_playback::config::{AudioFormat, Bitrate, PlayerConfig};
use librespot_playback::mixer::{self, MixerConfig};
use librespot_playback::player::{Player, PlayerEvent};
use sha1::{Digest, Sha1};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::UnixListener;
use tokio::sync::{broadcast, mpsc};

const RUN_DIR: &str = "/run/rg351v";
const RECONNECT_WINDOW: Duration = Duration::from_secs(600);
const RECONNECT_LIMIT: usize = 5;
/// How long after a reconnect to wait for the Connect state to settle before
/// loading the track again.
const RESTORE_DELAY: Duration = Duration::from_secs(3);
/// Only restore if the drop happened while playing this recently.
const RESTORE_MAX_AGE: Duration = Duration::from_secs(120);

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Play {
    Stopped,
    Loading,
    Playing,
    Paused,
}

impl Play {
    fn name(self) -> &'static str {
        match self {
            Play::Stopped => "stopped",
            Play::Loading => "loading",
            Play::Playing => "playing",
            Play::Paused => "paused",
        }
    }
}

#[derive(Clone, Debug)]
struct State {
    play: Play,
    uri: String,
    title: String,
    artist: String,
    album: String,
    cover: String,
    duration_ms: u32,
    position_ms: u32,
    position_at: Instant,
    volume: u16,
    shuffle: bool,
    repeat: bool,
    user: String,
    connected: bool,
    changed_at: Instant,
}

impl State {
    fn new() -> Self {
        let now = Instant::now();
        State {
            play: Play::Stopped,
            uri: String::new(),
            title: String::new(),
            artist: String::new(),
            album: String::new(),
            cover: String::new(),
            duration_ms: 0,
            position_ms: 0,
            position_at: now,
            volume: 0,
            shuffle: false,
            repeat: false,
            user: String::new(),
            connected: false,
            changed_at: now,
        }
    }

    /// Position now, extrapolated while playing.
    fn position(&self) -> u32 {
        if self.play == Play::Playing {
            let add = self.position_at.elapsed().as_millis() as u64;
            (self.position_ms as u64 + add).min(self.duration_ms.max(1) as u64) as u32
        } else {
            self.position_ms
        }
    }

    fn dump(&self) -> String {
        let mut s = String::new();
        let mut kv = |k: &str, v: &str| {
            s.push_str(k);
            s.push('=');
            s.push_str(&clean(v));
            s.push('\n');
        };
        kv("state", self.play.name());
        kv("connected", if self.connected { "1" } else { "0" });
        kv("user", &self.user);
        kv("uri", &self.uri);
        kv("title", &self.title);
        kv("artist", &self.artist);
        kv("album", &self.album);
        kv("cover", &self.cover);
        kv("duration_ms", &self.duration_ms.to_string());
        kv("position_ms", &self.position().to_string());
        kv("volume", &self.volume.to_string());
        kv("shuffle", if self.shuffle { "1" } else { "0" });
        kv("repeat", if self.repeat { "1" } else { "0" });
        s
    }
}

/// Control characters would break the line protocol.
fn clean(v: &str) -> String {
    v.chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .collect()
}

#[derive(Debug)]
enum Cmd {
    Play,
    Pause,
    Toggle,
    Next,
    Prev,
    Seek(u32),
    Volume(u16),
    Shuffle(bool),
    Repeat(bool),
    Load(String),
    /// Drop the Spotify session on purpose (exercises the restore path).
    Reconnect,
}

struct Opts {
    name: String,
    cache: PathBuf,
    device: String,
    sock: PathBuf,
    bitrate: Bitrate,
    initial_volume: u16,
}

fn parse_opts() -> Opts {
    let mut o = Opts {
        name: "RG351V".into(),
        cache: "/storage/.cache/librespot".into(),
        device: "default".into(),
        sock: format!("{RUN_DIR}/deckd.sock").into(),
        bitrate: Bitrate::Bitrate160,
        initial_volume: 50,
    };
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < args.len() {
        let val = args.get(i + 1).cloned().unwrap_or_default();
        match args[i].as_str() {
            "--name" => o.name = val,
            "--cache" => o.cache = val.into(),
            "--device" => o.device = val,
            "--sock" => o.sock = val.into(),
            "--bitrate" => {
                o.bitrate = match val.as_str() {
                    "96" => Bitrate::Bitrate96,
                    "320" => Bitrate::Bitrate320,
                    _ => Bitrate::Bitrate160,
                }
            }
            "--initial-volume" => {
                let pct: u32 = val.parse().unwrap_or(50).min(100);
                o.initial_volume = (pct * 65535 / 100) as u16;
            }
            other => {
                eprintln!("unknown option: {other}");
                std::process::exit(2);
            }
        }
        i += 2;
    }
    o
}

fn device_id(name: &str) -> String {
    let digest = Sha1::digest(name.as_bytes());
    digest.iter().map(|b| format!("{b:02x}")).collect()
}

fn set_flags(playing: bool) {
    let _ = fs::create_dir_all(RUN_DIR);
    if playing {
        let _ = fs::write(format!("{RUN_DIR}/playing"), b"");
    } else {
        let _ = fs::remove_file(format!("{RUN_DIR}/playing"));
        // autosleep.py treats a recent stop as still playing, which bridges
        // the gap between tracks.
        let _ = fs::write(format!("{RUN_DIR}/stopped"), b"");
    }
}

type Shared = Arc<Mutex<State>>;

/// Fold a player event into the shared state; returns an event line to
/// broadcast when something a UI cares about changed.
fn apply_event(st: &mut State, ev: &PlayerEvent) -> Option<String> {
    let now = Instant::now();
    match ev {
        PlayerEvent::TrackChanged { audio_item } => {
            st.uri = audio_item.uri.clone();
            st.title = audio_item.name.clone();
            st.duration_ms = audio_item.duration_ms;
            st.cover = audio_item
                .covers
                .first()
                .map(|c| c.url.clone())
                .unwrap_or_default();
            match &audio_item.unique_fields {
                UniqueFields::Track { artists, album, .. } => {
                    st.artist = artists
                        .iter()
                        .map(|a| a.name.clone())
                        .collect::<Vec<_>>()
                        .join(", ");
                    st.album = album.clone();
                }
                UniqueFields::Local { artists, album, .. } => {
                    st.artist = artists.clone().unwrap_or_default();
                    st.album = album.clone().unwrap_or_default();
                }
                UniqueFields::Episode { show_name, .. } => {
                    st.artist = show_name.clone();
                    st.album.clear();
                }
            }
            st.position_ms = 0;
            st.position_at = now;
            Some("track".into())
        }
        PlayerEvent::Loading { position_ms, .. } => {
            st.play = Play::Loading;
            st.position_ms = *position_ms;
            st.position_at = now;
            Some("loading".into())
        }
        PlayerEvent::Playing { position_ms, .. } => {
            st.play = Play::Playing;
            st.position_ms = *position_ms;
            st.position_at = now;
            st.changed_at = now;
            set_flags(true);
            Some("playing".into())
        }
        PlayerEvent::Paused { position_ms, .. } => {
            st.play = Play::Paused;
            st.position_ms = *position_ms;
            st.position_at = now;
            st.changed_at = now;
            set_flags(false);
            Some("paused".into())
        }
        PlayerEvent::Stopped { .. } => {
            st.play = Play::Stopped;
            st.changed_at = now;
            set_flags(false);
            Some("stopped".into())
        }
        PlayerEvent::PositionChanged { position_ms, .. }
        | PlayerEvent::PositionCorrection { position_ms, .. }
        | PlayerEvent::Seeked { position_ms, .. } => {
            st.position_ms = *position_ms;
            st.position_at = now;
            None
        }
        PlayerEvent::VolumeChanged { volume } => {
            st.volume = *volume;
            Some("volume".into())
        }
        PlayerEvent::ShuffleChanged { shuffle } => {
            st.shuffle = *shuffle;
            Some("shuffle".into())
        }
        PlayerEvent::RepeatChanged { context, .. } => {
            st.repeat = *context;
            Some("repeat".into())
        }
        PlayerEvent::SessionConnected { user_name, .. } => {
            st.connected = true;
            st.user = user_name.clone();
            Some("session".into())
        }
        PlayerEvent::SessionDisconnected { .. } => {
            st.connected = false;
            Some("session".into())
        }
        _ => None,
    }
}

fn parse_cmd(line: &str) -> Result<Cmd, String> {
    let mut it = line.split_whitespace();
    let word = it.next().unwrap_or("");
    let arg = it.next();
    let num = |a: Option<&str>| -> Result<u32, String> {
        a.ok_or("missing argument")?
            .parse::<u32>()
            .map_err(|_| "not a number".to_string())
    };
    let onoff = |a: Option<&str>| -> Result<bool, String> {
        match a {
            Some("on") | Some("1") => Ok(true),
            Some("off") | Some("0") => Ok(false),
            _ => Err("expected on|off".into()),
        }
    };
    Ok(match word {
        "play" => Cmd::Play,
        "pause" => Cmd::Pause,
        "toggle" => Cmd::Toggle,
        "next" => Cmd::Next,
        "prev" => Cmd::Prev,
        "seek" => Cmd::Seek(num(arg)?),
        "volume" => Cmd::Volume((num(arg)?.min(100) * 65535 / 100) as u16),
        "shuffle" => Cmd::Shuffle(onoff(arg)?),
        "repeat" => Cmd::Repeat(onoff(arg)?),
        "load" => Cmd::Load(arg.ok_or("missing uri")?.to_string()),
        "reconnect" => Cmd::Reconnect,
        other => return Err(format!("unknown command: {other}")),
    })
}

async fn serve_client(
    stream: tokio::net::UnixStream,
    state: Shared,
    cmds: mpsc::UnboundedSender<Cmd>,
    events: broadcast::Sender<String>,
) {
    let (rd, mut wr) = stream.into_split();
    let mut lines = BufReader::new(rd).lines();
    let mut sub: Option<broadcast::Receiver<String>> = None;
    loop {
        tokio::select! {
            line = lines.next_line() => {
                let line = match line {
                    Ok(Some(l)) => l,
                    _ => return,
                };
                let reply = match line.trim() {
                    "" => continue,
                    "status" => {
                        let mut s = state.lock().unwrap().dump();
                        s.push_str(".\n");
                        s
                    }
                    "subscribe" => {
                        sub = Some(events.subscribe());
                        "OK\n".to_string()
                    }
                    "quit" => return,
                    other => match parse_cmd(other) {
                        Ok(c) => {
                            let _ = cmds.send(c);
                            "OK\n".to_string()
                        }
                        Err(e) => format!("ERR {e}\n"),
                    },
                };
                if wr.write_all(reply.as_bytes()).await.is_err() {
                    return;
                }
            }
            ev = async {
                match sub.as_mut() {
                    Some(r) => r.recv().await,
                    None => std::future::pending().await,
                }
            } => {
                match ev {
                    Ok(e) => {
                        if wr.write_all(format!("event {e}\n").as_bytes()).await.is_err() {
                            return;
                        }
                    }
                    Err(broadcast::error::RecvError::Lagged(_)) => {}
                    Err(_) => return,
                }
            }
        }
    }
}

fn bind_socket(path: &PathBuf) -> io::Result<UnixListener> {
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir)?;
    }
    let _ = fs::remove_file(path);
    UnixListener::bind(path)
}

/// What to reload after an unexpected session loss.
#[derive(Clone)]
struct Restore {
    uri: String,
    position_ms: u32,
    was_playing: bool,
    taken: Instant,
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let opts = parse_opts();

    let cache = Cache::new(Some(&opts.cache), Some(&opts.cache), None, None)
        .unwrap_or_else(|e| fatal(&format!("cache {}: {e}", opts.cache.display())));
    let session_config = SessionConfig {
        device_id: device_id(&opts.name),
        ..Default::default()
    };
    let initial_volume = cache.volume().unwrap_or(opts.initial_volume);
    let connect_config = ConnectConfig {
        name: opts.name.clone(),
        device_type: DeviceType::GameConsole,
        initial_volume,
        ..Default::default()
    };

    let state: Shared = Arc::new(Mutex::new(State::new()));
    state.lock().unwrap().volume = initial_volume;
    let (events_tx, _) = broadcast::channel::<String>(64);
    let (cmd_tx, mut cmd_rx) = mpsc::unbounded_channel::<Cmd>();

    let listener = bind_socket(&opts.sock)
        .unwrap_or_else(|e| fatal(&format!("socket {}: {e}", opts.sock.display())));
    info!("control socket {}", opts.sock.display());

    let mut discovery = Some(
        Discovery::builder(
            session_config.device_id.clone(),
            session_config.client_id.clone(),
        )
        .name(opts.name.clone())
        .device_type(DeviceType::GameConsole)
        .launch()
        .unwrap_or_else(|e| fatal(&format!("discovery: {e}"))),
    );

    let mut last_credentials: Option<Credentials> = cache.credentials();
    let mut connecting = last_credentials.is_some();
    if connecting {
        info!("using cached credentials");
    } else {
        info!("no cached login: pair from the Spotify app (Connect devices)");
    }

    let mixer = (mixer::find(None).unwrap_or_else(|| fatal("no mixer")))(MixerConfig::default())
        .unwrap_or_else(|e| fatal(&format!("mixer: {e}")));
    let mut session = Session::new(session_config.clone(), Some(cache.clone()));
    let backend = audio_backend::find(Some("alsa".to_string()))
        .unwrap_or_else(|| fatal("alsa backend missing"));
    let device = opts.device.clone();
    let player = Player::new(
        PlayerConfig {
            bitrate: opts.bitrate,
            position_update_interval: Some(Duration::from_secs(1)),
            ..Default::default()
        },
        session.clone(),
        mixer.get_soft_volume(),
        move || backend(Some(device), AudioFormat::S16),
    );
    let mut player_events = player.get_player_event_channel();

    let mut spirc: Option<Spirc> = None;
    let mut spirc_task: Option<Pin<Box<dyn std::future::Future<Output = ()>>>> = None;
    let mut reconnects: Vec<Instant> = vec![];
    let mut restore: Option<Restore> = None;
    let mut restore_at: Option<Instant> = None;
    let mut tick = tokio::time::interval(Duration::from_millis(500));

    loop {
        tokio::select! {
            creds = async {
                match discovery.as_mut() {
                    Some(d) => d.next().await,
                    None => std::future::pending().await,
                }
            } => {
                match creds {
                    Some(c) => {
                        info!("paired from a Connect client");
                        last_credentials = Some(c);
                        reconnects.clear();
                        restore = None;
                        if let Some(s) = spirc.take() {
                            let _ = s.shutdown();
                        }
                        if let Some(t) = spirc_task.take() {
                            // The task is not Send, so finish it inline.
                            let _ = tokio::time::timeout(Duration::from_secs(3), t).await;
                        }
                        if !session.is_invalid() {
                            session.shutdown();
                        }
                        connecting = true;
                    }
                    None => {
                        warn!("discovery stopped");
                        discovery = None;
                    }
                }
            },
            _ = async {}, if connecting && last_credentials.is_some() => {
                if session.is_invalid() {
                    session = Session::new(session_config.clone(), Some(cache.clone()));
                    player.set_session(session.clone());
                }
                match Spirc::new(
                    connect_config.clone(),
                    session.clone(),
                    last_credentials.clone().unwrap_or_default(),
                    player.clone(),
                    mixer.clone(),
                )
                .await
                {
                    Ok((s, task)) => {
                        spirc = Some(s);
                        spirc_task = Some(Box::pin(task));
                        connecting = false;
                        {
                            let mut st = state.lock().unwrap();
                            st.connected = true;
                            if let Some(c) = last_credentials.as_ref() {
                                st.user = c.username.clone().unwrap_or_default();
                            }
                        }
                        let _ = events_tx.send("session".into());
                        if restore.is_some() {
                            restore_at = Some(Instant::now() + RESTORE_DELAY);
                        }
                    }
                    Err(e) => {
                        // Network still down (e.g. just after resume): retry.
                        warn!("could not start Connect: {e}");
                        if !session.is_invalid() {
                            session.shutdown();
                        }
                        tokio::time::sleep(Duration::from_secs(5)).await;
                    }
                }
            },
            _ = async {
                match spirc_task.as_mut() {
                    Some(t) => t.await,
                    None => std::future::pending().await,
                }
            }, if spirc_task.is_some() && !connecting => {
                spirc_task = None;
                spirc = None;
                warn!("Connect session ended unexpectedly");
                state.lock().unwrap().connected = false;
                let _ = events_tx.send("session".into());
                {
                    let st = state.lock().unwrap();
                    if st.play == Play::Playing
                        || (st.play == Play::Paused && st.changed_at.elapsed() < RESTORE_MAX_AGE)
                    {
                        restore = Some(Restore {
                            uri: st.uri.clone(),
                            position_ms: st.position(),
                            was_playing: st.play == Play::Playing,
                            taken: Instant::now(),
                        });
                    }
                }
                reconnects.retain(|t| t.elapsed() < RECONNECT_WINDOW);
                if last_credentials.is_some() && reconnects.len() <= RECONNECT_LIMIT {
                    reconnects.push(Instant::now());
                    if !session.is_invalid() {
                        session.shutdown();
                    }
                    connecting = true;
                } else {
                    fatal("Connect session keeps dropping; exiting so systemd restarts us");
                }
            },
            ev = player_events.recv() => {
                match ev {
                    Some(ev) => {
                        let line = apply_event(&mut state.lock().unwrap(), &ev);
                        if let Some(l) = line {
                            let _ = events_tx.send(l);
                        }
                    }
                    None => fatal("player shut down unexpectedly"),
                }
            },
            cmd = cmd_rx.recv() => {
                match (cmd, spirc.as_ref()) {
                    (Some(Cmd::Reconnect), _) => {
                        info!("reconnect requested");
                        if !session.is_invalid() {
                            session.shutdown();
                        }
                    }
                    (Some(cmd), Some(s)) => run_cmd(s, cmd),
                    _ => {}
                }
            },
            conn = listener.accept() => {
                if let Ok((stream, _)) = conn {
                    tokio::spawn(serve_client(
                        stream,
                        state.clone(),
                        cmd_tx.clone(),
                        events_tx.clone(),
                    ));
                }
            },
            _ = tick.tick() => {
                if let (Some(at), Some(s)) = (restore_at, spirc.as_ref()) {
                    if Instant::now() >= at {
                        restore_at = None;
                        if let Some(r) = restore.take() {
                            // The player keeps playing a buffered track through
                            // an outage, but the new Connect session is idle:
                            // the phone cannot control it and no queue exists.
                            // Loading the current track re-registers this
                            // device as active; autoplay continues afterwards.
                            let (play, pos) = {
                                let st = state.lock().unwrap();
                                (st.play, st.position())
                            };
                            if !r.uri.is_empty() && r.taken.elapsed() < RESTORE_MAX_AGE * 3 {
                                let playing = play == Play::Playing || (play != Play::Paused && r.was_playing);
                                let at = if play == Play::Playing { pos } else { r.position_ms };
                                info!("restoring {} at {} ms (playing: {})", r.uri, at, playing);
                                let _ = s.activate();
                                let _ = s.load(LoadRequest::from_tracks(
                                    vec![r.uri],
                                    LoadRequestOptions {
                                        start_playing: playing,
                                        seek_to: at,
                                        context_options: Some(LoadContextOptions::Autoplay),
                                        ..Default::default()
                                    },
                                ));
                            }
                        }
                    }
                }
            },
            _ = tokio::signal::ctrl_c() => break,
        }
    }

    info!("shutting down");
    set_flags(false);
    if let Some(s) = spirc {
        let _ = s.shutdown();
        if let Some(t) = spirc_task {
            let _ = tokio::time::timeout(Duration::from_secs(3), t).await;
        }
    }
    let _ = fs::remove_file(&opts.sock);
}

fn run_cmd(s: &Spirc, cmd: Cmd) {
    let r = match cmd {
        Cmd::Play => s.play(),
        Cmd::Pause => s.pause(),
        Cmd::Toggle => s.play_pause(),
        Cmd::Next => s.next(),
        Cmd::Prev => s.prev(),
        Cmd::Seek(ms) => s.set_position_ms(ms),
        Cmd::Volume(v) => s.set_volume(v),
        Cmd::Shuffle(b) => s.shuffle(b),
        Cmd::Repeat(b) => s.repeat(b),
        Cmd::Reconnect => Ok(()),
        Cmd::Load(uri) => {
            let opts = LoadRequestOptions {
                start_playing: true,
                ..Default::default()
            };
            if uri.starts_with("spotify:track:") || uri.starts_with("spotify:episode:") {
                s.load(LoadRequest::from_tracks(vec![uri], opts))
            } else {
                s.load(LoadRequest::from_context_uri(uri, opts))
            }
        }
    };
    if let Err(e) = r {
        warn!("command failed: {e}");
    }
}

fn fatal(msg: &str) -> ! {
    error!("{msg}");
    std::process::exit(1);
}

#[macro_use]
extern crate log;
