"""Single-owner Anki review session, with a worker keeping SDL responsive."""
import queue
import threading
from pathlib import Path

from collection_access import acquire_profile


class Session:
    def __init__(self, directory, auth_file=None, demo=False):
        from anki.collection import Collection
        self.directory = Path(directory)
        self.lock = acquire_profile(self.directory)
        self.col = None
        try:
            self.col = Collection(str(self.directory / "collection.anki2"))
            (self.directory / "backups").mkdir(exist_ok=True)
            self.col.create_backup(backup_folder=str(self.directory / "backups"),
                                   force=True, wait_for_completion=True)
        except Exception:
            if self.col:
                self.col.close()
            self.lock.close()
            raise
        self.auth_file, self.demo = auth_file, demo
        self.deck_id, self.deck_name = None, ""
        self.card = self.queued = None
        self.revealed, self.undo_step = False, None
        if demo and not self.col.card_count():
            self.seed_demo()

    def seed_demo(self):
        import io
        import math
        import struct
        import wave
        import zlib
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        pixels = b"".join(b"\0" + b"".join(bytes((30 + x // 4, 80 + y, 72 + x // 4))
                                         for x in range(160)) for y in range(80))
        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 160, 80, 8, 2, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))
        self.col.media.write_data("anki-demo.png", png)
        sound = io.BytesIO()
        with wave.open(sound, "wb") as stream:
            stream.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
            stream.writeframes(b"".join(struct.pack("<h", int(800 * math.sin(i * 2 * math.pi * 440 / 16000)))
                                       for i in range(4000)))
        self.col.media.write_data("anki-demo.wav", sound.getvalue())
        basic = self.col.models.by_name("Basic")
        for deck, front, back in (
            ("Japanese::Everyday words", "<b>木漏れ日</b><br>こもれび[sound:anki-demo.wav]",
             'Sunlight filtering through the trees.<br><br>木: tree　漏れる: to leak　日: sunlight'
             '<br><img src="anki-demo.png">'),
            ("Japanese::Everyday words", "<ruby>図書館<rt>としょかん</rt></ruby>", "Library"),
            ("Reading", "What makes a good study session?", "A little, every day.<br>" * 18),
        ):
            note = self.col.new_note(basic)
            note["Front"], note["Back"] = front, back
            self.col.add_note(note, self.col.decks.id(deck))
        cloze = self.col.new_note(self.col.models.by_name("Cloze"))
        cloze["Text"] = "The capital of Taiwan is {{c1::Taipei::city}}."
        self.col.add_note(cloze, self.col.decks.id("Reading"))

    def decks(self):
        self.card = self.queued = None
        self.revealed = False
        rows = []

        def visit(node, level=0):
            for child in node.children:
                if child.total_including_children:
                    rows.append({"id": child.deck_id, "name": child.name, "level": level,
                                 "new": child.new_count, "learn": child.learn_count,
                                 "review": child.review_count,
                                 "total": child.total_including_children})
                visit(child, level + 1)

        visit(self.col.sched.deck_due_tree())
        return {"view": "decks", "decks": rows, "total": self.col.card_count(),
                "can_undo": self.undo_step is not None, "demo": self.demo}

    def choose(self, deck_id):
        deck = self.col.decks.get(deck_id, default=False)
        if deck is None:
            raise ValueError("That deck no longer exists. Refresh the deck list.")
        self.deck_id, self.deck_name = deck_id, deck["name"].replace("::", " / ")
        self.col.decks.select(deck_id)
        return self.next_card()

    def next_card(self):
        if self.deck_id is None:
            return self.decks()
        queued = self.col.sched.get_queued_cards()
        self.revealed = False
        if not queued.cards:
            self.card = self.queued = None
            return {"view": "done", "deck": self.deck_name, "can_undo": self.undo_step is not None}
        self.queued = queued.cards[0]
        self.card = self.col.get_card(self.queued.card.id)
        self.card.start_timer()
        return self.card_state()

    def card_state(self):
        from anki.sound import SoundOrVideoTag
        self.card.question()  # Render both sides and collect upstream AV tags.
        tags = self.card.answer_av_tags() if self.revealed else self.card.question_av_tags()
        return {"view": "answer" if self.revealed else "question", "deck": self.deck_name,
                "card_id": self.card.id,
                "html": self.card.answer() if self.revealed else self.card.question(),
                "intervals": list(self.col.sched.describe_next_states(self.queued.states)),
                "audio": [tag.filename for tag in tags if isinstance(tag, SoundOrVideoTag)],
                "media": self.col.media.dir(), "can_undo": self.undo_step is not None}

    def flip(self, card_id):
        if self.card is None or card_id != self.card.id:
            raise ValueError("The card changed. Please try again.")
        self.revealed = True
        return self.card_state()

    def rate(self, card_id, rating):
        from anki.scheduler.v3 import CardAnswer
        if not self.revealed or self.card is None or self.card.id != card_id:
            raise ValueError("Reveal the current answer before rating it.")
        ratings = {1: CardAnswer.AGAIN, 2: CardAnswer.HARD, 3: CardAnswer.GOOD, 4: CardAnswer.EASY}
        answer = self.col.sched.build_answer(card=self.card, states=self.queued.states,
                                             rating=ratings[rating])
        self.col.sched.answer_card(answer)
        self.undo_step = self.col.undo_status().last_step
        return self.next_card()

    def undo(self):
        if self.undo_step is None or self.col.undo_status().last_step != self.undo_step:
            raise ValueError("No review is available to undo.")
        self.col.undo()
        self.undo_step = None
        return self.next_card() if self.deck_id else self.decks()

    def sync(self, progress):
        if self.demo:
            raise ValueError("Demo collection: sync is disabled.")
        from anki.sync import SyncAuth
        from sync_cli import save_auth
        from sync_client import sync_collection
        auth_file = Path(self.auth_file)
        if not auth_file.exists():
            raise ValueError("Sign in from the Mac: scripts/anki.sh login")
        auth = SyncAuth.FromString(auth_file.read_bytes())
        try:
            sync_collection(self.col, auth, backup_dir=self.directory / "backups",
                            on_progress=progress)
        finally:
            save_auth(auth_file, auth)
            # A sync invalidates undo and any previously fetched scheduler state.
            self.card = self.queued = None
            self.undo_step, self.deck_id = None, None
        return self.decks()

    def close(self):
        try:
            self.col.close()
        finally:
            self.lock.close()


class Worker(threading.Thread):
    def __init__(self, directory, auth_file, demo=False):
        super().__init__(name="anki-backend", daemon=True)
        self.directory, self.auth_file, self.demo = directory, auth_file, demo
        self.requests, self.replies = queue.Queue(), queue.Queue()
        self.session = None
        self.stopping = False

    def send(self, command, *args):
        self.requests.put((command, args))

    def cancel(self):
        if self.session:
            self.session.col.abort_sync()
            self.session.col.abort_media_sync()

    def stop(self):
        self.stopping = True
        self.cancel()
        self.send("quit")

    def run(self):
        try:
            self.session = Session(self.directory, self.auth_file, self.demo)
            self.replies.put(("state", self.session.decks()))
            while not self.stopping:
                command, args = self.requests.get()
                if command == "quit":
                    break
                try:
                    if command == "sync":
                        result = self.session.sync(lambda message: self.replies.put(("progress", message)))
                    else:
                        result = getattr(self.session, command)(*args)
                    self.replies.put(("state", result))
                except Exception as exc:
                    self.replies.put(("error", str(exc)))
        except Exception as exc:
            self.replies.put(("fatal", str(exc)))
        finally:
            if self.session:
                self.session.close()
