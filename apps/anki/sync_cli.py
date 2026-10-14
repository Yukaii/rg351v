"""Terminal login/sync for the future gamepad client. Never prints auth tokens."""

import argparse
import getpass
import json
import os
import shutil
import sys
from pathlib import Path

from anki.collection import Collection
from anki.sync import SyncAuth

from sync_client import FullSyncRequired, sync_collection


def save_auth(filename, auth):
    filename.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if filename.exists():
        shutil.copy2(filename, filename.with_suffix(".bak"))
    temporary = filename.with_suffix(".new")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(auth.SerializeToString())
    os.replace(temporary, filename)
    os.chmod(filename, 0o600)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path,
                        default=Path("/storage/roms/homebrew/anki-data"))
    parser.add_argument("--auth-file", type=Path,
                        default=Path("/storage/.config/rg351v/anki/auth.bin"))
    commands = parser.add_subparsers(dest="command", required=True)
    login = commands.add_parser("login")
    login.add_argument("--endpoint", help="self-hosted URL; default is AnkiWeb")
    commands.add_parser("status")
    sync = commands.add_parser("sync")
    sync.add_argument("--full", choices=("download", "upload"))
    args = parser.parse_args()
    args.data_dir.mkdir(parents=True, exist_ok=True)
    col = Collection(str(args.data_dir / "collection.anki2"))
    try:
        if args.command == "login":
            if not sys.stdin.isatty():
                parser.error("login requires an interactive terminal")
            username = input("Anki username/email: ").strip()
            password = getpass.getpass("Password: ")
            auth = col.sync_login(username, password, args.endpoint)
            save_auth(args.auth_file, auth)
            print("Logged in. Saved a sync token; the password is not stored.")
        else:
            if not args.auth_file.exists():
                parser.error("log in first: scripts/anki.sh login")
            auth = SyncAuth.FromString(args.auth_file.read_bytes())
            auth.io_timeout_secs = 30
            if args.command == "status":
                status = col.sync_status(auth)
                if status.new_endpoint:
                    auth.endpoint = status.new_endpoint
                    save_auth(args.auth_file, auth)
                print(json.dumps({"cards": col.card_count(),
                                  "reviews": col.db.scalar("select count(*) from revlog"),
                                  "required": status.Required.Name(status.required)}))
            else:
                try:
                    result = sync_collection(col, auth, full=args.full,
                                             backup_dir=args.data_dir / "backups")
                finally:
                    # Retain a server redirect even when a full-sync choice is pending.
                    save_auth(args.auth_file, auth)
                print(json.dumps(result))
    finally:
        col.close()


if __name__ == "__main__":
    try:
        main()
    except FullSyncRequired as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(2)
