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

use bytes::Bytes;

use futures_util::future::join_all;
use futures_util::StreamExt;
use librespot_connect::{
    ConnectConfig, LoadContextOptions, LoadRequest, LoadRequestOptions, PlayingTrack, Spirc,
};
use librespot_core::authentication::Credentials;
use librespot_core::cache::Cache;
use librespot_core::config::DeviceType;
use librespot_core::dealer::manager::BoxedStreamResult;
use librespot_core::dealer::protocol::Message;
use librespot_core::{Session, SessionConfig, SpotifyUri};
use librespot_discovery::Discovery;
use librespot_metadata::audio::UniqueFields;
use librespot_metadata::{Album, Metadata, Playlist, Track};
use librespot_playback::audio_backend;
use librespot_protocol::connect::ClusterUpdate;
use librespot_protocol::playlist4_external::SelectedListContent;
use protobuf::Message as _;
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

/// One upcoming track from the cluster state.
#[derive(Clone, Debug, Default)]
struct QItem {
    uri: String,
    /// Added by the user ("queue" provider) rather than coming from the context.
    queued: bool,
    title: String,
    artist: String,
}

#[derive(Clone, Debug)]
struct State {
    play: Play,
    uri: String,
    /// Playing context (playlist/album/...) from Connect cluster updates.
    context: String,
    /// Upcoming tracks as last reported by the cluster (next_tracks).
    queue: Vec<QItem>,
    title: String,
    artist: String,
    album: String,
    cover: String,
    /// Local JPEG of the cover (fetched by deckd), empty until downloaded.
    cover_file: String,
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
            context: String::new(),
            queue: Vec::new(),
            title: String::new(),
            artist: String::new(),
            album: String::new(),
            cover: String::new(),
            cover_file: String::new(),
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
        kv("context", &self.context);
        kv("title", &self.title);
        kv("artist", &self.artist);
        kv("album", &self.album);
        kv("cover", &self.cover);
        kv("cover_file", &self.cover_file);
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
    /// Skip forward `n` tracks (the queue view jumps this way, which keeps
    /// the context and the user queue intact).
    Skip(u32),
    /// Play `track`, inside `context` when given.
    PlayTrack { context: Option<String>, track: String },
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
/// The live Spotify session, for request handlers that talk to the Web API.
type SessShared = Arc<Mutex<Option<Session>>>;

const WEB_SCOPES: &str = "playlist-read-private,playlist-read-collaborative,user-library-read,user-read-private,user-top-read";

const PAGE: usize = 24;

fn session_of(sess: &SessShared) -> Result<Session, String> {
    sess.lock()
        .unwrap()
        .clone()
        .ok_or_else(|| "no session".to_string())
}

type NameCache = Mutex<std::collections::HashMap<String, (String, String)>>;

fn name_cache() -> &'static NameCache {
    static C: std::sync::OnceLock<NameCache> = std::sync::OnceLock::new();
    C.get_or_init(|| Mutex::new(std::collections::HashMap::new()))
}

/// Title and artists of a track, cached (the cluster state usually carries
/// them, otherwise one metadata lookup).
async fn track_name(session: &Session, uri: &str) -> Option<(String, String)> {
    if let Some(v) = name_cache().lock().unwrap().get(uri) {
        return Some(v.clone());
    }
    let id = SpotifyUri::from_uri(uri).ok()?;
    let t = Track::get(session, &id).await.ok()?;
    let artists = t
        .artists
        .iter()
        .map(|a| a.name.clone())
        .collect::<Vec<_>>()
        .join(", ");
    let v = (t.name, artists);
    name_cache().lock().unwrap().insert(uri.to_string(), v.clone());
    Some(v)
}

/// `item\tkind\turi\ttitle\tsubtitle` lines for a slice of track URIs.
async fn track_items(session: &Session, uris: &[String]) -> String {
    let rows = join_all(uris.iter().map(|u| async move {
        let (name, artist) = track_name(session, u)
            .await
            .unwrap_or_else(|| (u.clone(), String::new()));
        format!("item\ttrack\t{u}\t{}\t{}\n", clean(&name), clean(&artist))
    }))
    .await;
    rows.concat()
}

async fn list_playlists(session: &Session) -> Result<String, String> {
    let bytes = session
        .spclient()
        .get_rootlist(0, Some(200))
        .await
        .map_err(|e| format!("rootlist: {e}"))?;
    let list = SelectedListContent::parse_from_bytes(&bytes).map_err(|e| format!("parse: {e}"))?;
    let contents = &list.contents;
    let mut out = String::new();
    let mut n = 0;
    for (i, item) in contents.items.iter().enumerate() {
        let item_uri = item.uri().to_string();
        if !item_uri.starts_with("spotify:playlist:") {
            continue; // folders and group markers
        }
        let meta = contents.meta_items.get(i);
        let name = meta
            .map(|m| m.attributes.name().to_string())
            .filter(|n| !n.is_empty())
            .unwrap_or_else(|| item_uri.clone());
        let len = meta.map(|m| m.length()).unwrap_or(0);
        out.push_str(&format!(
            "item\tplaylist\t{}\t{}\t{} tracks\n",
            item_uri,
            clean(&name),
            len
        ));
        n += 1;
    }
    Ok(format!("page\t0\t{n}\n{out}"))
}

/// Track URIs of a context: a playlist URI, `liked`, or `search:<query>`.
async fn context_tracks(session: &Session, ctx: &str) -> Result<(String, Vec<String>), String> {
    if ctx.starts_with("spotify:playlist:") {
        let id = SpotifyUri::from_uri(ctx).map_err(|e| format!("uri: {e}"))?;
        let pl = Playlist::get(session, &id)
            .await
            .map_err(|e| format!("playlist: {e}"))?;
        let uris = pl.tracks().filter_map(|t| t.to_uri().ok()).collect();
        return Ok((ctx.to_string(), uris));
    }
    let uri = if ctx.starts_with("spotify:album:") {
        ctx.to_string()
    } else if ctx == "liked" {
        format!("spotify:user:{}:collection", session.username())
    } else if let Some(q) = ctx.strip_prefix("search:") {
        format!("spotify:search:{}", q.replace(' ', "+"))
    } else {
        return Err("unknown context".into());
    };
    let c = session
        .spclient()
        .get_context(&uri)
        .await
        .map_err(|e| format!("context: {e}"))?;
    let uris = c
        .pages
        .iter()
        .flat_map(|p| p.tracks.iter())
        .map(|t| t.uri().to_string())
        .filter(|u| u.starts_with("spotify:track:"))
        .collect();
    // Search results are not a playable context of their own.
    let play_ctx = if ctx.starts_with("search:") { "-".to_string() } else { uri };
    Ok((play_ctx, uris))
}

async fn list_tracks(session: &Session, ctx: &str, offset: usize) -> Result<String, String> {
    let (play_ctx, uris) = context_tracks(session, ctx).await?;
    let total = uris.len();
    let slice: Vec<String> = uris.into_iter().skip(offset).take(PAGE).collect();
    let items = track_items(session, &slice).await;
    Ok(format!("page\t{offset}\t{total}\ncontext\t{play_ctx}\n{items}"))
}

fn put_varint(v: &mut Vec<u8>, mut n: u64) {
    while n >= 0x80 {
        v.push((n as u8 & 0x7F) | 0x80);
        n >>= 7;
    }
    v.push(n as u8);
}

/// spotify.collection.proto.v2.PageRequest, hand-encoded: the generated
/// type is not worth a dependency for three fields.
fn encode_page_request(user: &str, set: &str, token: &str, limit: u32) -> Vec<u8> {
    let mut v = Vec::new();
    for (tag, val) in [(0x0Au8, user), (0x12, set), (0x1A, token)] {
        if val.is_empty() {
            continue;
        }
        v.push(tag);
        put_varint(&mut v, val.len() as u64);
        v.extend_from_slice(val.as_bytes());
    }
    if limit > 0 {
        v.push(0x20);
        put_varint(&mut v, limit as u64);
    }
    v
}

/// Read a varint from `b[*i..]`.
fn get_varint(b: &[u8], i: &mut usize) -> Option<u64> {
    let mut n = 0u64;
    let mut shift = 0;
    while *i < b.len() {
        let x = b[*i];
        *i += 1;
        n |= ((x & 0x7F) as u64) << shift;
        if x & 0x80 == 0 {
            return Some(n);
        }
        shift += 7;
        if shift > 63 {
            return None;
        }
    }
    None
}

/// Fields of one length-delimited protobuf message: (field number, payload).
fn pb_fields(b: &[u8]) -> Vec<(u32, &[u8])> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        let Some(key) = get_varint(b, &mut i) else { break };
        let (field, wire) = ((key >> 3) as u32, key & 7);
        match wire {
            0 => {
                if get_varint(b, &mut i).is_none() {
                    break;
                }
            }
            2 => {
                let Some(len) = get_varint(b, &mut i) else { break };
                let end = i + len as usize;
                if end > b.len() {
                    break;
                }
                out.push((field, &b[i..end]));
                i = end;
            }
            1 => i += 8,
            5 => i += 4,
            _ => break,
        }
    }
    out
}

/// One page of the user's collection set: ((item URI, added_at), next token).
/// The endpoint rejects any Content-Type, so none is sent.
async fn collection_page(
    session: &Session,
    set: &str,
    token: &str,
    limit: u32,
) -> Result<(Vec<(String, i64)>, String), String> {
    let body = encode_page_request(&session.username(), set, token, limit);
    let resp = session
        .spclient()
        .request(&http::Method::POST, "/collection/v2/paging", None, Some(&body))
        .await
        .map_err(|e| format!("collection: {e}"))?;
    let mut items = Vec::new();
    let mut next = String::new();
    for (field, payload) in pb_fields(&resp) {
        match field {
            1 => {
                // CollectionItem: uri = 1, added_at = 2 (int32 varint)
                let mut uri = String::new();
                let mut added = 0i64;
                let mut i = 0;
                while i < payload.len() {
                    let Some(key) = get_varint(payload, &mut i) else { break };
                    match (key >> 3, key & 7) {
                        (1, 2) => {
                            let Some(len) = get_varint(payload, &mut i) else { break };
                            let end = (i + len as usize).min(payload.len());
                            uri = String::from_utf8_lossy(&payload[i..end]).to_string();
                            i = end;
                        }
                        (_, 0) => {
                            let Some(v) = get_varint(payload, &mut i) else { break };
                            if key >> 3 == 2 {
                                added = v as i64;
                            }
                        }
                        (_, 2) => {
                            let Some(len) = get_varint(payload, &mut i) else { break };
                            i += len as usize;
                        }
                        (_, 1) => i += 8,
                        (_, 5) => i += 4,
                        _ => break,
                    }
                }
                if !uri.is_empty() {
                    items.push((uri, added));
                }
            }
            2 => next = String::from_utf8_lossy(payload).to_string(),
            _ => {}
        }
    }
    Ok((items, next))
}

type AlbumCache = Mutex<Option<(Instant, Vec<String>)>>;

fn album_cache() -> &'static AlbumCache {
    static C: std::sync::OnceLock<AlbumCache> = std::sync::OnceLock::new();
    C.get_or_init(|| Mutex::new(None))
}

/// Saved album URIs, newest first (cached for a couple of minutes).
async fn saved_albums(session: &Session) -> Result<Vec<String>, String> {
    if let Some((at, v)) = album_cache().lock().unwrap().as_ref() {
        if at.elapsed() < Duration::from_secs(120) {
            return Ok(v.clone());
        }
    }
    let mut all: Vec<(String, i64)> = Vec::new();
    let mut token = String::new();
    for _ in 0..40 {
        let (items, next) = collection_page(session, "collection", &token, 200).await?;
        all.extend(items.into_iter().filter(|(u, _)| u.starts_with("spotify:album:")));
        if next.is_empty() {
            break;
        }
        token = next;
    }
    all.sort_by(|a, b| b.1.cmp(&a.1));
    let uris: Vec<String> = all.into_iter().map(|(u, _)| u).collect();
    *album_cache().lock().unwrap() = Some((Instant::now(), uris.clone()));
    Ok(uris)
}

/// Read our device's live state (context and upcoming tracks) from the
/// connect-state service. The server does not echo our own state back over
/// the dealer, so this endpoint is the only place it can be read.
async fn refresh_cluster(session: &Session, state: &Shared) -> Result<(), String> {
    let body = session
        .spclient()
        .request(&http::Method::GET, "/connect-state/v1/cluster", None, None)
        .await
        .map_err(|e| format!("cluster: {e}"))?;
    let v: serde_json::Value = serde_json::from_slice(&body).map_err(|e| format!("cluster json: {e}"))?;
    let ps = &v["player_state"];
    let ctx = ps["context_uri"].as_str().unwrap_or("").to_string();
    let queue: Vec<QItem> = ps["next_tracks"]
        .as_array()
        .map(|a| {
            a.iter()
                .filter_map(|t| {
                    let uri = t["uri"].as_str()?;
                    if !uri.starts_with("spotify:track:") {
                        return None;
                    }
                    let md = &t["metadata"];
                    Some(QItem {
                        uri: uri.to_string(),
                        queued: t["provider"].as_str() == Some("queue"),
                        title: md["title"].as_str().unwrap_or("").to_string(),
                        artist: md["artist_name"].as_str().unwrap_or("").to_string(),
                    })
                })
                .collect()
        })
        .unwrap_or_default();
    let mut st = state.lock().unwrap();
    // Only trust it when this device is the one playing.
    if v["active_device_id"].as_str() == Some(&session.device_id()) || st.play != Play::Stopped {
        if !ctx.is_empty() {
            st.context = ctx;
        }
        st.queue = queue;
    }
    Ok(())
}

/// The upcoming queue: user-queued tracks first, then the context's next tracks.
async fn list_queue(session: &Session, state: &Shared) -> Result<String, String> {
    refresh_cluster(session, state).await?;
    let (q, ctx) = {
        let st = state.lock().unwrap();
        (st.queue.clone(), st.context.clone())
    };
    let rows = join_all(q.iter().take(60).map(|it| async move {
        let (name, artist) = if !it.title.is_empty() {
            (it.title.clone(), it.artist.clone())
        } else {
            track_name(session, &it.uri)
                .await
                .unwrap_or_else(|| (it.uri.clone(), String::new()))
        };
        let kind = if it.queued { "queued" } else { "track" };
        format!("item\t{kind}\t{}\t{}\t{}\n", it.uri, clean(&name), clean(&artist))
    }))
    .await;
    let ctx = if ctx.is_empty() { "-".to_string() } else { ctx };
    Ok(format!("page\t0\t{}\ncontext\t{ctx}\n{}", q.len().min(60), rows.concat()))
}

async fn list_albums(session: &Session, offset: usize) -> Result<String, String> {
    let uris = saved_albums(session).await?;
    let total = uris.len();
    let slice: Vec<String> = uris.into_iter().skip(offset).take(PAGE).collect();
    let rows = join_all(slice.iter().map(|u| async move {
        let named = async {
            let id = SpotifyUri::from_uri(u).ok()?;
            let a = Album::get(session, &id).await.ok()?;
            let artists = a
                .artists
                .iter()
                .map(|x| x.name.clone())
                .collect::<Vec<_>>()
                .join(", ");
            Some((a.name, artists))
        }
        .await;
        let (name, artist) = named.unwrap_or_else(|| (u.clone(), String::new()));
        format!("item\talbum\t{u}\t{}\t{}\n", clean(&name), clean(&artist))
    }))
    .await;
    Ok(format!("page\t{offset}\t{total}\n{}", rows.concat()))
}

/// Download a cover image into /run/rg351v/covers (tmpfs) and return its path.
async fn fetch_cover(session: &Session, url: &str) -> Result<String, String> {
    let dir = format!("{RUN_DIR}/covers");
    fs::create_dir_all(&dir).map_err(|e| format!("mkdir: {e}"))?;
    let mut h: u32 = 2166136261;
    for b in url.bytes() {
        h = (h ^ b as u32).wrapping_mul(16777619);
    }
    let path = format!("{dir}/{h:08x}.jpg");
    if std::path::Path::new(&path).exists() {
        return Ok(path);
    }
    let req = http::Request::builder()
        .method("GET")
        .uri(url)
        .body(Bytes::new())
        .map_err(|e| format!("request: {e}"))?;
    let body = session
        .http_client()
        .request_body(req)
        .await
        .map_err(|e| format!("http: {e}"))?;
    if body.len() < 100 {
        return Err("short image".into());
    }
    fs::write(&path, &body).map_err(|e| format!("write: {e}"))?;
    // Keep the newest few files only; /run is RAM.
    if let Ok(rd) = fs::read_dir(&dir) {
        let mut files: Vec<_> = rd
            .flatten()
            .filter_map(|e| Some((e.metadata().ok()?.modified().ok()?, e.path())))
            .collect();
        files.sort();
        let excess = files.len().saturating_sub(24);
        for (_, p) in files.into_iter().take(excess) {
            let _ = fs::remove_file(p);
        }
    }
    Ok(path)
}

/// The cover closest to ~300 px (the UI shows 192 px); URL or empty.
fn pick_cover(item: &librespot_metadata::audio::AudioItem) -> String {
    item.covers
        .iter()
        .min_by_key(|c| (c.width - 300).abs())
        .map(|c| c.url.clone())
        .unwrap_or_default()
}

/// GET https://api.spotify.com/v1/<path> with the session's own token.
async fn web_get(sess: &SessShared, path: &str) -> Result<String, String> {
    let session = sess
        .lock()
        .unwrap()
        .clone()
        .ok_or_else(|| "no session".to_string())?;
    let token = session
        .token_provider()
        .get_token(WEB_SCOPES)
        .await
        .map_err(|e| format!("token: {e}"))?;
    let req = http::Request::builder()
        .method("GET")
        .uri(format!("https://api.spotify.com/v1/{path}"))
        .header("Authorization", format!("Bearer {}", token.access_token))
        .body(Bytes::new())
        .map_err(|e| format!("request: {e}"))?;
    let body = session
        .http_client()
        .request_body(req)
        .await
        .map_err(|e| format!("http: {e}"))?;
    String::from_utf8(body.to_vec()).map_err(|e| format!("utf8: {e}"))
}

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
        "play" if arg.is_none() => Cmd::Play,
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
        "skip" => Cmd::Skip(num(arg)?.clamp(1, 60)),
        "play" if arg.is_some() => {
            let track = it.next().ok_or("usage: play <context|-> <track uri>")?;
            Cmd::PlayTrack {
                context: arg.filter(|c| *c != "-").map(|c| c.to_string()),
                track: track.to_string(),
            }
        }
        other => return Err(format!("unknown command: {other}")),
    })
}

async fn serve_client(
    stream: tokio::net::UnixStream,
    state: Shared,
    cmds: mpsc::UnboundedSender<Cmd>,
    events: broadcast::Sender<String>,
    sess: SessShared,
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
                    "playlists" => match session_of(&sess) {
                        Ok(se) => match list_playlists(&se).await {
                            Ok(b) => format!("{b}.\n"),
                            Err(e) => format!("ERR {}\n", clean(&e)),
                        },
                        Err(e) => format!("ERR {e}\n"),
                    },
                    "queue" => match session_of(&sess) {
                        Ok(se) => match list_queue(&se, &state).await {
                            Ok(b) => format!("{b}.\n"),
                            Err(e) => format!("ERR {}\n", clean(&e)),
                        },
                        Err(e) => format!("ERR {e}\n"),
                    },
                    q if q == "albums" || q.starts_with("albums ") => {
                        let off = q[6..].trim().parse().unwrap_or(0);
                        match session_of(&sess) {
                            Ok(se) => match list_albums(&se, off).await {
                                Ok(b) => format!("{b}.\n"),
                                Err(e) => format!("ERR {}\n", clean(&e)),
                            },
                            Err(e) => format!("ERR {e}\n"),
                        }
                    }
                    q if q.starts_with("tracks ") => {
                        let mut it = q[7..].split_whitespace();
                        let ctx = it.next().unwrap_or("").to_string();
                        let off = it.next().and_then(|v| v.parse().ok()).unwrap_or(0);
                        match session_of(&sess) {
                            Ok(se) => match list_tracks(&se, &ctx, off).await {
                                Ok(b) => format!("{b}.\n"),
                                Err(e) => format!("ERR {}\n", clean(&e)),
                            },
                            Err(e) => format!("ERR {e}\n"),
                        }
                    }
                    q if q.starts_with("search ") => {
                        // search <offset> <query...>
                        let rest = q[7..].trim();
                        let (off, query) = match rest.split_once(' ') {
                            Some((o, t)) if o.parse::<usize>().is_ok() => (o.parse().unwrap_or(0), t.trim()),
                            _ => (0, rest),
                        };
                        match session_of(&sess) {
                            Ok(se) => match list_tracks(&se, &format!("search:{query}"), off).await {
                                Ok(b) => format!("{b}.\n"),
                                Err(e) => format!("ERR {}\n", clean(&e)),
                            },
                            Err(e) => format!("ERR {e}\n"),
                        }
                    }
                    // Debug probes for exploring internal endpoints.
                    cl if cl.starts_with("coll ") => match session_of(&sess) {
                        Ok(se) => {
                            let mut it = cl[5..].split_whitespace();
                            let set = it.next().unwrap_or("collection");
                            let limit = it.next().and_then(|v| v.parse().ok()).unwrap_or(100);
                            match collection_page(&se, set, "", limit).await {
                                Ok((items, next)) => format!(
                                    "set {set}: {} items next={} first={:?}\n.\n",
                                    items.len(),
                                    clean(&next),
                                    items.first()
                                ),
                                Err(e) => format!("ERR {}\n", clean(&e)),
                            }
                        }
                        Err(e) => format!("ERR {e}\n"),
                    },
                    sp if sp.starts_with("sp ") => match session_of(&sess) {
                        Ok(se) => match se
                            .spclient()
                            .request(&http::Method::GET, sp[3..].trim(), None, None)
                            .await
                        {
                            Ok(b) => {
                                let t = String::from_utf8_lossy(&b).to_string();
                                format!("{} bytes: {}\n.\n", b.len(), clean(&t.chars().take(1500).collect::<String>()))
                            }
                            Err(e) => format!("ERR {}\n", clean(&e.to_string())),
                        },
                        Err(e) => format!("ERR {e}\n"),
                    },
                    mc if mc.starts_with("merc ") => match session_of(&sess) {
                        Ok(se) => match se.mercury().get(mc[5..].trim().to_string()) {
                            Ok(fut) => match fut.await {
                                Ok(r) => {
                                    let p = r.payload.first().map(|b| String::from_utf8_lossy(b).to_string()).unwrap_or_default();
                                    format!("status {} parts {}: {}\n.\n", r.status_code, r.payload.len(), clean(&p.chars().take(1500).collect::<String>()))
                                }
                                Err(e) => format!("ERR {}\n", clean(&e.to_string())),
                            },
                            Err(e) => format!("ERR {}\n", clean(&e.to_string())),
                        },
                        Err(e) => format!("ERR {e}\n"),
                    },
                    cx if cx.starts_with("ctx ") => match session_of(&sess) {
                        Ok(se) => match se.spclient().get_context(cx[4..].trim()).await {
                            Ok(c) => {
                                let n: usize = c.pages.iter().map(|p| p.tracks.len()).sum();
                                let first = c.pages.iter().flat_map(|p| p.tracks.iter()).next().map(|t| t.uri().to_string()).unwrap_or_default();
                                format!("uri {} pages {} tracks {} first {}\n.\n", c.uri(), c.pages.len(), n, first)
                            }
                            Err(e) => format!("ERR {}\n", clean(&e.to_string())),
                        },
                        Err(e) => format!("ERR {e}\n"),
                    },
                    web if web.starts_with("web ") => {
                        // Debug probe: raw Web API JSON on one line.
                        match web_get(&sess, web[4..].trim()).await {
                            Ok(body) => format!("{}\n.\n", clean(&body)),
                            Err(e) => format!("ERR {e}\n"),
                        }
                    }
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
    context: String,
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
    let sess_shared: SessShared = Arc::new(Mutex::new(None));

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
    // A command pressed while the session was down; runs once it is back.
    let mut pending: Option<(Cmd, Instant)> = None;
    let mono_start = Instant::now();
    let mut boot_off = boot_seconds();
    // After a reboot Spotify still lists this device as the active player and
    // resumes it as soon as the session is up. Playback that starts by itself
    // in the first seconds, before anyone asked for it, is paused again.
    const BOOT_GUARD: Duration = Duration::from_secs(25);
    let mut boot_guard = true;
    let mut tick = tokio::time::interval(Duration::from_millis(500));
    let last_path = opts.cache.join("deckd-last");
    let mut last: Option<Last> = load_last(&last_path);
    let mut last_saved = Instant::now();
    let mut cluster_rx: Option<BoxedStreamResult<ClusterUpdate>> = None;

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
                        *sess_shared.lock().unwrap() = Some(session.clone());
                        cluster_rx = session
                            .dealer()
                            .listen_for("hm://connect-state/v1/cluster", Message::from_raw::<ClusterUpdate>)
                            .ok();
                        if restore.is_some() || pending.is_some() {
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
                *sess_shared.lock().unwrap() = None;
                let _ = events_tx.send("session".into());
                {
                    let st = state.lock().unwrap();
                    if st.play == Play::Playing
                        || (st.play == Play::Paused && st.changed_at.elapsed() < RESTORE_MAX_AGE)
                    {
                        restore = Some(Restore {
                            uri: st.uri.clone(),
                            context: st.context.clone(),
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
                        if boot_guard && mono_start.elapsed() > BOOT_GUARD {
                            boot_guard = false;
                        }
                        if boot_guard
                            && matches!(ev, PlayerEvent::Loading { .. } | PlayerEvent::Playing { .. })
                        {
                            if let Some(s) = spirc.as_ref() {
                                info!("playback started by itself right after boot; pausing it");
                                let _ = s.pause();
                                boot_guard = false;
                            }
                        }
                        if let PlayerEvent::TrackChanged { audio_item } = &ev {
                            let url = pick_cover(audio_item);
                            state.lock().unwrap().cover_file.clear();
                            if !url.is_empty() && !session.is_invalid() {
                                let (se, st, tx) = (session.clone(), state.clone(), events_tx.clone());
                                tokio::spawn(async move {
                                    match fetch_cover(&se, &url).await {
                                        Ok(path) => {
                                            st.lock().unwrap().cover_file = path;
                                            let _ = tx.send("cover".into());
                                        }
                                        Err(e) => warn!("cover: {e}"),
                                    }
                                });
                                // Keep the context fresh (restore, queue view)
                                // once the new track's state has been published.
                                let (se, st) = (session.clone(), state.clone());
                                tokio::spawn(async move {
                                    tokio::time::sleep(Duration::from_millis(1500)).await;
                                    if let Err(e) = refresh_cluster(&se, &st).await {
                                        warn!("{e}");
                                    }
                                });
                            }
                        }
                        let line = apply_event(&mut state.lock().unwrap(), &ev);
                        if matches!(
                            ev,
                            PlayerEvent::Paused { .. }
                                | PlayerEvent::Stopped { .. }
                                | PlayerEvent::Playing { .. }
                                | PlayerEvent::TrackChanged { .. }
                        ) {
                            let st = state.lock().unwrap();
                            if !st.uri.is_empty() {
                                // A track that ended resumes from its start.
                                let near_end = st.duration_ms > 0
                                    && st.position().saturating_add(3000) >= st.duration_ms;
                                let l = Last {
                                    context: st.context.clone(),
                                    track: st.uri.clone(),
                                    position_ms: if near_end { 0 } else { st.position() },
                                };
                                if !matches!(ev, PlayerEvent::Playing { .. } | PlayerEvent::TrackChanged { .. }) {
                                    save_last(&last_path, &l);
                                }
                                last = Some(l);
                            }
                        }
                        if let Some(l) = line {
                            let _ = events_tx.send(l);
                        }
                    }
                    None => fatal("player shut down unexpectedly"),
                }
            },
            upd = async {
                match cluster_rx.as_mut() {
                    Some(c) => c.next().await,
                    None => std::future::pending().await,
                }
            } => {
                match upd {
                    Some(Ok(u)) => {
                        if let Some(c) = u.cluster.as_ref() {
                            // Only our own state tells us what we are playing.
                            let ours = c.active_device_id == session_config.device_id;
                            let ps = &c.player_state;
                            info!(
                                "cluster: active={} ours={} ctx={} next={} prev={}",
                                c.active_device_id,
                                ours,
                                ps.context_uri,
                                ps.next_tracks.len(),
                                ps.prev_tracks.len()
                            );
                            if ours {
                                let mut st = state.lock().unwrap();
                                let mut changed = false;
                                if !ps.context_uri.is_empty() && st.context != ps.context_uri {
                                    st.context = ps.context_uri.clone();
                                    changed = true;
                                }
                                let q: Vec<QItem> = ps
                                    .next_tracks
                                    .iter()
                                    .filter(|t| t.uri.starts_with("spotify:track:"))
                                    .map(|t| QItem {
                                        uri: t.uri.clone(),
                                        queued: t.provider == "queue",
                                        title: t.metadata.get("title").cloned().unwrap_or_default(),
                                        artist: t.metadata.get("artist_name").cloned().unwrap_or_default(),
                                    })
                                    .collect();
                                if q.len() != st.queue.len()
                                    || q.iter().zip(st.queue.iter()).any(|(a, b)| a.uri != b.uri)
                                {
                                    st.queue = q;
                                    changed = true;
                                }
                                drop(st);
                                if changed {
                                    let _ = events_tx.send("context".into());
                                }
                            }
                        }
                    }
                    Some(Err(e)) => warn!("cluster update: {e}"),
                    None => cluster_rx = None,
                }
            },
            cmd = cmd_rx.recv() => {
                if cmd.is_some() {
                    boot_guard = false; // someone is driving it on purpose
                }
                match (cmd, spirc.as_ref()) {
                    (Some(Cmd::Reconnect), _) => {
                        info!("reconnect requested");
                        if !session.is_invalid() {
                            session.shutdown();
                        }
                    }
                    (Some(cmd), Some(s)) => {
                        let idle = state.lock().unwrap().play == Play::Stopped;
                        dispatch(s, cmd, idle, &last);
                    }
                    (Some(cmd), None) if queueable(&cmd) => {
                        info!("session is down, holding {cmd:?} until it is back");
                        pending = Some((cmd, Instant::now()));
                    }
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
                        sess_shared.clone(),
                    ));
                }
            },
            _ = tick.tick() => {
                // A resume from suspend leaves a session the server has
                // already dropped; do not wait for the ping timeout.
                let off = boot_seconds() - mono_start.elapsed().as_secs_f64();
                if off - boot_off > 2.0 {
                    info!("resumed from suspend ({:.0} s), re-establishing the session", off - boot_off);
                    if !connecting && !session.is_invalid() {
                        session.shutdown();
                    }
                }
                boot_off = off;
                if let (Some(at), Some(s)) = (restore_at, spirc.as_ref()) {
                    if Instant::now() >= at {
                        restore_at = None;
                        if let Some((c, t)) = pending.take() {
                            if t.elapsed() < Duration::from_secs(120) {
                                // What the user asked for beats an automatic restore.
                                restore = None;
                                let idle = state.lock().unwrap().play != Play::Playing;
                                info!("running held command {c:?}");
                                dispatch(s, c, idle, &last);
                            }
                        } else if let Some(r) = restore.take() {
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
                                let _ = s.load(resume_request(&r.context, &r.uri, at, playing));
                            }
                        }
                    }
                }
                // Persist the position every few seconds while playing.
                if last_saved.elapsed() >= Duration::from_secs(10) {
                    last_saved = Instant::now();
                    let st = state.lock().unwrap();
                    if st.play == Play::Playing && !st.uri.is_empty() {
                        save_last(&last_path, &Last {
                            context: st.context.clone(),
                            track: st.uri.clone(),
                            position_ms: st.position(),
                        });
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

/// Where playback was when it last stopped or changed track; survives
/// restarts so a button press on the device can pick up where you left off.
#[derive(Clone, Default)]
struct Last {
    context: String,
    track: String,
    position_ms: u32,
}

fn load_last(path: &PathBuf) -> Option<Last> {
    let text = fs::read_to_string(path).ok()?;
    let mut it = text.lines();
    let context = it.next()?.to_string();
    let track = it.next()?.to_string();
    let position_ms = it.next()?.parse().ok()?;
    if track.is_empty() {
        return None;
    }
    Some(Last {
        context,
        track,
        position_ms,
    })
}

fn save_last(path: &PathBuf, last: &Last) {
    let body = format!("{}\n{}\n{}\n", last.context, last.track, last.position_ms);
    let _ = fs::write(path, body);
}

/// A load request that resumes `track` inside `context` when we know it, and
/// otherwise just the track with autoplay.
fn resume_request(context: &str, track: &str, position_ms: u32, play: bool) -> LoadRequest {
    if context.starts_with("spotify:") && !context.is_empty() {
        LoadRequest::from_context_uri(
            context.to_string(),
            LoadRequestOptions {
                start_playing: play,
                seek_to: position_ms,
                playing_track: Some(PlayingTrack::Uri(track.to_string())),
                ..Default::default()
            },
        )
    } else {
        LoadRequest::from_tracks(
            vec![track.to_string()],
            LoadRequestOptions {
                start_playing: play,
                seek_to: position_ms,
                context_options: Some(LoadContextOptions::Autoplay),
                ..Default::default()
            },
        )
    }
}

/// Run a command on a live Spirc; when nothing is loaded, play/toggle pick up
/// the last track instead.
fn dispatch(s: &Spirc, cmd: Cmd, idle: bool, last: &Option<Last>) {
    match (idle, matches!(cmd, Cmd::Play | Cmd::Toggle), last) {
        (true, true, Some(l)) => {
            info!("resuming {} at {} ms", l.track, l.position_ms);
            let _ = s.activate();
            let _ = s.load(resume_request(&l.context, &l.track, l.position_ms, true));
        }
        _ => run_cmd(s, cmd),
    }
}

/// Commands worth holding while the session is down.
fn queueable(cmd: &Cmd) -> bool {
    matches!(
        cmd,
        Cmd::Play | Cmd::Toggle | Cmd::Next | Cmd::Prev | Cmd::Skip(_) | Cmd::Load(_) | Cmd::PlayTrack { .. }
    )
}

/// Seconds since boot including time spent suspended (CLOCK_BOOTTIME-like).
fn boot_seconds() -> f64 {
    fs::read_to_string("/proc/uptime")
        .ok()
        .and_then(|t| t.split_whitespace().next()?.parse().ok())
        .unwrap_or(0.0)
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
        Cmd::Skip(n) => {
            let mut r = Ok(());
            for _ in 0..n {
                r = s.next();
                if r.is_err() {
                    break;
                }
            }
            r
        }
        Cmd::PlayTrack { context, track } => {
            // A device that has not played yet (fresh start, rebuilt
            // session) ignores load commands until it is activated.
            let _ = s.activate();
            let opts = LoadRequestOptions {
                start_playing: true,
                playing_track: Some(PlayingTrack::Uri(track.clone())),
                ..Default::default()
            };
            match context {
                Some(c) => s.load(LoadRequest::from_context_uri(c, opts)),
                None => s.load(LoadRequest::from_tracks(
                    vec![track],
                    LoadRequestOptions {
                        start_playing: true,
                        context_options: Some(LoadContextOptions::Autoplay),
                        ..Default::default()
                    },
                )),
            }
        }
        Cmd::Load(uri) => {
            let _ = s.activate();
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
