"""Exercise the upstream backend using only a disposable offline collection."""

import faulthandler
import json
import platform
import tempfile
import time
from pathlib import Path

faulthandler.dump_traceback_later(20)
IMPORT_STARTED = time.monotonic()

from anki.buildinfo import version
from anki.collection import Collection
from anki.scheduler.v3 import CardAnswer


def main():
    started = time.monotonic()
    report = {
        "anki": version,
        "python": platform.python_version(),
        "kernel": platform.release(),
        "machine": platform.machine(),
        "import_seconds": round(time.monotonic() - IMPORT_STARTED, 3),
        "checks": [],
    }

    def check(condition, name):
        if not condition:
            raise AssertionError(name)
        report["checks"].append(name)

    # Test on the same filesystem as the bundled runtime, including SQLite I/O.
    with tempfile.TemporaryDirectory(prefix="probe-", dir=Path(__file__).parent) as work:
        filename = str(Path(work) / "collection.anki2")
        col = Collection(filename)
        try:
            check(col.v3_scheduler(), "v3 scheduler available")
            deck_id = col.decks.id("RG351V probe")
            col.decks.select(deck_id)
            note = col.new_note(col.models.by_name("Basic"))
            note["Front"] = "RG351V <b>question</b> 中文"
            note["Back"] = "Anki backend answer"
            col.add_note(note, deck_id)
            check(col.card_count() == 1, "create note and card")
            queued = col.sched.get_queued_cards().cards[0]
            card = col.get_card(queued.card.id)
            check("question" in card.question(), "render question HTML")
            check("backend answer" in card.answer(), "render answer HTML")
            labels = col.sched.describe_next_states(queued.states)
            check(len(labels) == 4, "four scheduling choices")
            card.start_timer()
            answer = col.sched.build_answer(
                card=card, states=queued.states, rating=CardAnswer.GOOD
            )
            col.sched.answer_card(answer)
            check(col.db.scalar("select count(*) from revlog") == 1, "record offline review")
            check(col.get_card(card.id).reps == 1, "update card scheduling")
            col.undo()
            check(col.db.scalar("select count(*) from revlog") == 0, "undo review")
            col.redo()
            check(col.db.scalar("select count(*) from revlog") == 1, "redo review")
            media_name = col.media.write_data("probe.txt", b"RG351V media probe")
            check((Path(col.media.dir()) / media_name).read_bytes() == b"RG351V media probe",
                  "write and read local media")
            card_id = card.id
        finally:
            col.close()
        col = Collection(filename)
        try:
            check(col.get_card(card_id).reps == 1, "review persists after reopening")
            check(col.db.scalar("select count(*) from revlog") == 1,
                  "review history persists after reopening")
        finally:
            col.close()

    faulthandler.cancel_dump_traceback_later()
    report["elapsed_seconds"] = round(time.monotonic() - started, 3)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
