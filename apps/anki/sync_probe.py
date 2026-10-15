"""Real sync-protocol integration test: disposable Mac/server/handheld collections."""

import base64
import json
import os
import secrets
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from anki.collection import Collection
from anki.scheduler.v3 import CardAnswer
from anki.sync import SyncAuth

from sync_client import FullSyncRequired, sync_collection
from sync_cli import save_auth

RUNTIME = "/storage/roms/homebrew/anki-backend-probe"
PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aX1kAAAAASUVORK5CYII="
)


def require(value, message):
    if not value:
        raise AssertionError(message)


def device_phase(request):
    action = request["action"]
    folder = Path(request["folder"])
    require(folder.parent == Path(RUNTIME) and folder.name.startswith("sync-probe-"),
            "unexpected test directory")
    if action == "download":
        folder.mkdir()
    if action == "cleanup":
        if folder.exists():
            shutil.rmtree(folder)
        return {"cleaned": True}
    col = Collection(str(folder / "collection.anki2"))
    try:
        if action == "download":
            auth = col.sync_login(request["username"], request["password"], request["endpoint"])
            with tempfile.TemporaryDirectory(prefix="anki-auth-probe-", dir="/storage/.cache") as private:
                token = Path(private) / "auth.bin"
                save_auth(token, auth)
                save_auth(token, auth)
                require(token.stat().st_mode & 0o777 == 0o600, "private token mode on ext4")
                require(token.with_suffix(".bak").read_bytes() == auth.SerializeToString(),
                        "token backed up before replacement on ext4")
            try:
                sync_collection(col, auth)
            except FullSyncRequired:
                pass
            else:
                raise AssertionError("initial full download proceeded without a direction")
            try:
                sync_collection(col, auth, full="upload")
            except FullSyncRequired:
                pass
            else:
                raise AssertionError("empty handheld uploaded over populated server")
            sync_collection(col, auth, full="download")
            (folder / "test-auth.bin").write_bytes(auth.SerializeToString())
            require(col.card_count() == 1, "initial card download")
            require((Path(col.media.dir()) / "probe.png").read_bytes() == PNG,
                    "initial media download")
            return {"folder": str(folder), "cards": col.card_count(), "media": "verified"}
        if action == "review":
            # The orchestrator stops the server for this entire phase.
            require(col.db.scalar("select count(*) from revlog") == 0, "fresh review history")
            queued = col.sched.get_queued_cards().cards[0]
            card = col.get_card(queued.card.id)
            card.start_timer()
            col.sched.answer_card(col.sched.build_answer(
                card=card, states=queued.states, rating=CardAnswer.GOOD))
            note = col.get_note(card.nid)
            note["Back"] = "Edited offline on RG351V"
            col.update_note(note)
            col.media.write_data("handheld.txt", b"Media added offline on RG351V")
            require(col.get_card(card.id).reps == 1, "offline scheduling")
            return {"reviews": col.db.scalar("select count(*) from revlog"), "card_id": card.id}
        if action == "upload":
            auth = SyncAuth.FromString((folder / "test-auth.bin").read_bytes())
            result = sync_collection(col, auth, backup_dir=folder / "backups")
            require(col.card_count() == 2, "receive Mac note while uploading offline review")
            require(col.db.scalar("select count(*) from revlog") == 1, "review not duplicated")
            require(any((folder / "backups").glob("*.colpkg")), "pre-sync backup")
            (folder / "test-auth.bin").write_bytes(auth.SerializeToString())
            return result
        raise ValueError("unknown phase")
    finally:
        col.close()


def main():
    host = os.environ.get("RG351V_HOST", "root@RG351V.local")
    ssh = ["ssh", "-4", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", host]
    name = subprocess.check_output([*ssh, "hostname"], text=True).strip()
    require(name == "RG351V", f"refusing device writes to {name}")
    here = Path(__file__).resolve().parent
    subprocess.run(["scp", "-4", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
                    str(here / "sync_client.py"), str(here / "sync_cli.py"),
                    str(here / "collection_access.py"), str(here / "sync_probe.py"),
                    f"{host}:{RUNTIME}/"], check=True)

    def phase(action, **kwargs):
        command = (f"timeout -s KILL 240 env PYTHONDONTWRITEBYTECODE=1 "
                   f"PYTHONPATH={RUNTIME}/site-packages "
                   f"python3 {RUNTIME}/sync_probe.py --device")
        result = subprocess.run([*ssh, command], input=json.dumps({"action": action, **kwargs}),
                                text=True, capture_output=True, timeout=250)
        if result.returncode:
            raise RuntimeError(f"device {action} failed: {result.stderr}")
        return json.loads(result.stdout)

    with tempfile.TemporaryDirectory(prefix="sync-roundtrip-", dir=here.parents[1] / ".storage") as work:
        work = Path(work)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        remote_port = secrets.randbelow(20000) + 40000
        endpoint = f"http://127.0.0.1:{port}/"
        username, password = "rg351v-test", secrets.token_hex(16)
        server_env = {**os.environ, "SYNC_HOST": "127.0.0.1", "SYNC_PORT": str(port),
                      "SYNC_BASE": str(work / "server"), "SYNC_USER1": f"{username}:{password}"}
        server = None
        tunnel = None
        device_folder = None
        col = None
        log = open(work / "server.log", "w+")

        def start_server():
            process = subprocess.Popen([sys.executable, "-m", "anki.syncserver"],
                                       env=server_env, stdout=log, stderr=log)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    log.seek(0)
                    raise RuntimeError("test server exited: " + log.read())
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                        return process
                except OSError:
                    time.sleep(0.1)
            process.kill()
            process.wait()
            raise TimeoutError("test server startup")

        def stop(process):
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

        try:
            server = start_server()
            tunnel = subprocess.Popen([*ssh[:-1], "-o", "ExitOnForwardFailure=yes", "-N",
                                       "-R", f"127.0.0.1:{remote_port}:127.0.0.1:{port}", host],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            time.sleep(0.5)
            require(tunnel.poll() is None, "SSH test tunnel startup")
            (work / "desktop").mkdir()
            col = Collection(str(work / "desktop" / "collection.anki2"))
            deck_id = col.decks.id("RG351V sync test")
            col.decks.select(deck_id)
            note = col.new_note(col.models.by_name("Basic"))
            note["Front"] = 'Sync question 中文 <img src="probe.png">'
            note["Back"] = "Original Mac answer"
            col.add_note(note, deck_id)
            col.media.write_data("probe.png", PNG)
            auth = col.sync_login(username, password, endpoint)
            sync_collection(col, auth, full="upload")
            print("PASS Mac seeded upstream server with card and image", flush=True)

            device_folder = f"{RUNTIME}/sync-probe-{secrets.token_hex(8)}"
            downloaded = phase("download", folder=device_folder, username=username, password=password,
                               endpoint=f"http://127.0.0.1:{remote_port}/")
            require(downloaded["folder"] == device_folder, "test directory matches")
            print("PASS handheld login, private ext4 token, explicit full download, card and media", flush=True)

            stop(server)
            reviewed = phase("review", folder=device_folder)
            require(reviewed["reviews"] == 1, "offline review count")
            print("PASS handheld reviewed and edited with server stopped", flush=True)
            second = col.new_note(col.models.by_name("Basic"))
            second["Front"] = "Added on Mac while handheld was offline"
            second["Back"] = "Independent desktop change"
            col.add_note(second, deck_id)

            server = start_server()
            sync_collection(col, auth)
            phase("upload", folder=device_folder)
            sync_collection(col, auth)
            require(col.db.scalar("select count(*) from revlog") == 1, "Mac received review")
            require(col.get_card(reviewed["card_id"]).reps == 1, "Mac received scheduling")
            require(col.get_note(note.id)["Back"] == "Edited offline on RG351V", "Mac received edit")
            require(col.card_count() == 2, "concurrent Mac addition preserved")
            require((Path(col.media.dir()) / "handheld.txt").read_bytes()
                    == b"Media added offline on RG351V", "Mac received handheld media")
            print("PASS bidirectional incremental sync: review, scheduling, edit, new card, media", flush=True)
            phase("upload", folder=device_folder)
            sync_collection(col, auth)
            require(col.db.scalar("select count(*) from revlog") == 1, "repeat sync is idempotent")
            print("PASS repeated sync has no duplicate reviews; pre-sync backups created", flush=True)
            col.close()
            col = None
            save_auth(work / "auth.bin", auth)
            cli = [sys.executable, str(here / "sync_cli.py"),
                   "--data-dir", str(work / "desktop"), "--auth-file", str(work / "auth.bin")]
            status = json.loads(subprocess.check_output([*cli, "status"], text=True))
            require(status["cards"] == 2 and status["reviews"] == 1, "CLI status")
            result = json.loads(subprocess.check_output([*cli, "sync"], text=True))
            require(result["media"] == "complete", "CLI sync")
            require((work / "auth.bin").stat().st_mode & 0o777 == 0o600, "private auth file")
            require((work / "auth.bak").exists(), "auth file backed up")
            print("PASS terminal status/sync, private token storage and token backup", flush=True)
            from review_backend import Session
            session = Session(work / "desktop", work / "auth.bin")
            try:
                session.choose(deck_id)
                state = session.sync(lambda _: None)
                require(state["view"] == "decks", "UI sync returns to deck selection")
                require(session.card is None and session.undo_step is None,
                        "UI sync discards stale card and undo state")
            finally:
                session.close()
            print("PASS review UI backend sync and scheduler-state invalidation", flush=True)
        finally:
            try:
                if col is not None:
                    col.close()
                if device_folder:
                    phase("cleanup", folder=device_folder)
            finally:
                stop(tunnel)
                stop(server)
                log.close()


if __name__ == "__main__":
    if sys.argv[1:] == ["--device"]:
        print(json.dumps(device_phase(json.load(sys.stdin))))
    else:
        main()
