"""Review/safety checks on disposable collections; never contact AnkiWeb."""
import tempfile
import unittest
from pathlib import Path

from card_document import CardHTML, layout, media_path
from collection_access import acquire_profile
from console import Canvas
from review_backend import Session


class ReviewTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.session = Session(self.temp.name, demo=True)
        self.addCleanup(self.session.close)
        rows = self.session.decks()["decks"]
        self.deck = next(row for row in rows if row["name"].endswith("Everyday words"))

    def test_review_requires_reveal_and_valid_card_then_undoes(self):
        state = self.session.choose(self.deck["id"])
        cid = state["card_id"]
        with self.assertRaises(ValueError):
            self.session.rate(cid, 3)
        self.session.flip(cid)
        with self.assertRaises(ValueError):
            self.session.rate(cid + 1, 3)
        self.session.rate(cid, 3)
        self.assertEqual(self.session.col.get_card(cid).reps, 1)
        self.assertEqual(self.session.col.db.scalar("select count(*) from revlog"), 1)
        undone = self.session.undo()
        self.assertEqual(undone["card_id"], cid)
        self.assertEqual(self.session.col.get_card(cid).reps, 0)
        self.assertEqual(self.session.col.db.scalar("select count(*) from revlog"), 0)

    def test_profile_cannot_be_opened_twice(self):
        with self.assertRaisesRegex(RuntimeError, "already open"):
            acquire_profile(self.temp.name)

    def test_demo_sync_is_disabled(self):
        with self.assertRaisesRegex(ValueError, "disabled"):
            self.session.sync(lambda _: None)

    def test_all_four_ratings_use_native_scheduling(self):
        for rating in (1, 2, 3, 4):
            state = self.session.choose(self.deck["id"])
            cid = state["card_id"]
            self.session.flip(cid)
            self.session.rate(cid, rating)
            self.assertEqual(self.session.col.get_card(cid).reps, 1)
            self.session.undo()
            self.assertEqual(self.session.col.get_card(cid).reps, 0)

    def test_audio_tags_and_review_persist_on_reopen(self):
        state = self.session.choose(self.deck["id"])
        self.assertEqual(state["audio"], ["anki-demo.wav"])
        cid = state["card_id"]
        self.session.flip(cid)
        self.session.rate(cid, 3)
        self.session.close()
        reopened = Session(self.temp.name, demo=True)
        try:
            self.assertEqual(reopened.col.get_card(cid).reps, 1)
            self.assertEqual(reopened.col.db.scalar("select count(*) from revlog"), 1)
        finally:
            reopened.close()

    def test_cloze_is_rendered_by_upstream(self):
        row = next(row for row in self.session.decks()["decks"] if row["name"] == "Reading")
        cards = self.session.col.find_cards(f"did:{row['id']} note:Cloze")
        card = self.session.col.get_card(cards[0])
        front = "".join(token.text for token in CardHTML(card.question()).tokens)
        back = "".join(token.text for token in CardHTML(card.answer()).tokens)
        self.assertNotIn("Taipei", front)
        self.assertIn("Taipei", back)


class DocumentTests(unittest.TestCase):
    def test_anki_interval_bidi_controls_do_not_render_as_boxes(self):
        font = Path(__file__).resolve().parents[2] / ".storage/ptterm-data/unifont.bin"
        plain, localized = Canvas(font), Canvas(font)
        plain.text(0, 0, "<1m")
        localized.text(0, 0, "<\u20681\u2069m")
        self.assertEqual(plain.pixels, localized.pixels)
        self.assertEqual(localized.width("\u2068"), 0)

    def test_scripts_and_styles_are_omitted_but_cloze_and_ruby_remain(self):
        parsed = CardHTML('<style>.x{color:red}</style><script>fetch("remote")</script>'
                          '<span class="cloze">漢字</span><ruby>木<rt>き</rt></ruby>')
        text = "".join(token.text for token in parsed.tokens)
        self.assertNotIn("fetch", text)
        self.assertNotIn("color", text)
        self.assertIn("漢字木 (き)", text)
        self.assertTrue(any(token.emphasis for token in parsed.tokens if "漢字" in token.text))
        self.assertTrue(parsed.interactive)

    def test_media_is_local_and_cannot_escape_collection(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work) / "media"
            root.mkdir()
            (root / "space name.png").write_bytes(b"image")
            (Path(work) / "outside").write_bytes(b"secret")
            (root / "link").symlink_to(Path(work) / "outside")
            self.assertEqual(media_path(root, "space%20name.png"), (root / "space name.png").resolve())
            for name in ("../outside", "%2e%2e%2foutside", "https://host/image", "file:///etc/shadow", "link"):
                self.assertIsNone(media_path(root, name))

    def test_text_wraps_to_card_width_and_keeps_answer_anchor(self):
        font = Path(__file__).resolve().parents[2] / ".storage/ptterm-data/unifont.bin"
        canvas = Canvas(font)
        items, height, anchor = layout("long words " * 30 + '<hr id="answer">答案', canvas, ".", width=160)
        self.assertGreater(anchor, 0)
        self.assertGreater(height, anchor)
        for kind, x, _, text, _ in items:
            if kind == "glyph":
                self.assertLessEqual(x + canvas.width(text) * 2, 160)


if __name__ == "__main__":
    unittest.main()
