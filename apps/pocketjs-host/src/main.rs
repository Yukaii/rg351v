//! pocketjs-host: boot a PocketJS bundle headless and time every phase.
//!
//! Env:
//!   POCKET_JS / POCKET_PAK   bundle files (default app.js / app.pak)
//!   BENCH_FRAMES             frames to run (default 600)
//!   BENCH_FPS                0 = as fast as possible, N = pace to N fps
//!   BENCH_LOGICAL            WxH logical viewport (default 480x272)
//!   BENCH_DENSITY            raster density (default 2)
//!   BENCH_FULL               1 = repaint every pixel every frame (worst case)
//!
//! Output is one `key value` pair per line so scripts can parse it.
use std::time::{Duration, Instant};

use anyhow::{ensure, Context, Result};
use pocket_mod::Guest;
use pocket_ui_surface::UiSurface;
use pocketjs_core::damage::{DamagePolicy, DamageTracker};
use pocketjs_core::raster;
use pocketjs_core::spec::{btn, ANALOG_CENTER};

// Must match the target profile the bundle was compiled for
// (contracts/spec/platforms.ts: pocketbook = hostAbi 5).
const HOST_ID: &str = "pocketbook";
const HOST_ABI: u32 = 5;

fn env_u32(name: &str, default: u32) -> u32 {
    std::env::var(name)
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

/// Scripted input: walk down a list, activate, and go back, so focus moves,
/// scrolls and screen changes all get exercised.
fn buttons_for(frame: u32) -> u32 {
    match frame % 60 {
        10 | 20 | 30 => btn::DOWN,
        40 => btn::CROSS,
        50 => btn::CIRCLE,
        _ => 0,
    }
}

struct Stats(Vec<f64>);

impl Stats {
    fn report(&mut self, name: &str) {
        self.0.sort_by(|a, b| a.partial_cmp(b).unwrap());
        let n = self.0.len().max(1);
        let sum: f64 = self.0.iter().sum();
        let p = |q: f64| self.0.get(((n as f64 * q) as usize).min(n - 1)).copied().unwrap_or(0.0);
        println!("{name}_avg_ms {:.3}", sum / n as f64);
        println!("{name}_p50_ms {:.3}", p(0.5));
        println!("{name}_p95_ms {:.3}", p(0.95));
        println!("{name}_max_ms {:.3}", self.0.last().copied().unwrap_or(0.0));
    }
}

/// utime + stime of this process in seconds (clock ticks are 100 Hz).
fn cpu_seconds() -> f64 {
    let stat = std::fs::read_to_string("/proc/self/stat").unwrap_or_default();
    let after = stat.rsplit(')').next().unwrap_or("");
    let f: Vec<&str> = after.split_whitespace().collect();
    let tick = |i: usize| f.get(i).and_then(|v| v.parse::<f64>().ok()).unwrap_or(0.0);
    (tick(11) + tick(12)) / 100.0
}

fn status_kb(key: &str) -> u64 {
    std::fs::read_to_string("/proc/self/status")
        .unwrap_or_default()
        .lines()
        .find_map(|l| l.strip_prefix(key))
        .and_then(|v| v.trim_start_matches(':').split_whitespace().next()?.parse().ok())
        .unwrap_or(0)
}

fn main() -> Result<()> {
    let frames = env_u32("BENCH_FRAMES", 600);
    let fps = env_u32("BENCH_FPS", 0);
    let density = env_u32("BENCH_DENSITY", 2);
    let full = env_u32("BENCH_FULL", 0) != 0;
    let (lw, lh) = std::env::var("BENCH_LOGICAL")
        .ok()
        .and_then(|v| {
            let (w, h) = v.split_once('x')?;
            Some((w.parse().ok()?, h.parse().ok()?))
        })
        .unwrap_or((480u32, 272u32));

    let js_path = std::env::var("POCKET_JS").unwrap_or_else(|_| "app.js".into());
    let pak_path = std::env::var("POCKET_PAK").unwrap_or_else(|_| "app.pak".into());
    let pak = std::fs::read(&pak_path).with_context(|| format!("reading {pak_path}"))?;
    let js = std::fs::read_to_string(&js_path).with_context(|| format!("reading {js_path}"))?;

    let boot = Instant::now();
    let surface = UiSurface::new_with_density((lw as f32, lh as f32), density);
    surface.set_identity(HOST_ID, HOST_ABI);
    surface.feed_pak(&pak);
    let guest = Guest::new()?;
    surface.mount(&guest)?;
    guest.eval("app", &js)?;
    ensure!(guest.has_frame(), "bundle installed no frame()");

    let (rw, rh) = ((lw * density) as usize, (lh * density) as usize);
    let mut rgba = vec![0u8; rw * rh * 4];
    let mut tracker: DamageTracker = DamageTracker::new();
    let mut raster_frame = |surface: &UiSurface, rgba: &mut Vec<u8>| -> bool {
        if full {
            tracker.invalidate();
        }
        surface.with_ui(|ui| {
            let words = ui.draw().words.clone();
            match raster::render_scaled_incremental(
                ui,
                &words,
                rgba,
                density,
                &mut tracker,
                DamagePolicy::default(),
            ) {
                Ok(plan) => !plan.is_empty(),
                Err(_) => {
                    raster::render_scaled(ui, &words, rgba, density);
                    tracker.invalidate();
                    true
                }
            }
        })
    };

    // First paint counts as boot, not as a steady-state frame.
    guest.frame_with_touches(0, ANALOG_CENTER, &[])?;
    surface.tick();
    raster_frame(&surface, &mut rgba);
    let boot_ms = boot.elapsed().as_secs_f64() * 1000.0;

    let (mut t_guest, mut t_tick, mut t_raster, mut t_total) =
        (Stats(vec![]), Stats(vec![]), Stats(vec![]), Stats(vec![]));
    let mut painted = 0u32;
    let period = if fps > 0 {
        Some(Duration::from_secs_f64(1.0 / fps as f64))
    } else {
        None
    };
    let cpu0 = cpu_seconds();
    let wall0 = Instant::now();
    let mut next = Instant::now();
    for i in 0..frames {
        let t0 = Instant::now();
        guest.frame_with_touches(buttons_for(i), ANALOG_CENTER, &[])?;
        let t1 = Instant::now();
        surface.tick();
        let t2 = Instant::now();
        if raster_frame(&surface, &mut rgba) {
            painted += 1;
        }
        let t3 = Instant::now();
        let ms = |a: Instant, b: Instant| b.duration_since(a).as_secs_f64() * 1000.0;
        t_guest.0.push(ms(t0, t1));
        t_tick.0.push(ms(t1, t2));
        t_raster.0.push(ms(t2, t3));
        t_total.0.push(ms(t0, t3));
        if let Some(p) = period {
            next += p;
            let now = Instant::now();
            if next > now {
                std::thread::sleep(next - now);
            } else {
                next = now;
            }
        }
    }
    let wall = wall0.elapsed().as_secs_f64();
    let cpu = cpu_seconds() - cpu0;

    println!("frames {frames}");
    println!("render {rw}x{rh} logical {lw}x{lh} density {density}");
    println!("boot_ms {boot_ms:.1}");
    println!("full_repaint {}", full as u32);
    println!("painted_frames {painted}");
    println!("wall_s {wall:.2}");
    println!("cpu_s {cpu:.2}");
    println!("cpu_pct_of_one_core {:.1}", cpu / wall * 100.0);
    println!("rss_kb {}", status_kb("VmRSS"));
    println!("peak_rss_kb {}", status_kb("VmHWM"));
    t_guest.report("guest");
    t_tick.report("tick");
    t_raster.report("raster");
    t_total.report("frame");
    Ok(())
}
