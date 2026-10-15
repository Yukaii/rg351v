"""A deliberately small, offline HTML renderer: text, cloze, ruby and images."""
from dataclasses import dataclass
from html.parser import HTMLParser
from pathlib import Path
import re
import struct
from urllib.parse import unquote, urlsplit


def media_path(root, name):
    parsed = urlsplit(name)
    name = unquote(parsed.path)
    if parsed.scheme or parsed.netloc or not name or Path(name).name != name or "\\" in name:
        return None
    path = (Path(root) / name).resolve()
    return path if path.parent == Path(root).resolve() and path.is_file() else None


def image_size(path):
    try:
        with path.open("rb") as stream:
            head = stream.read(24)
            if head.startswith(b"\x89PNG\r\n\x1a\n"):
                return struct.unpack(">II", head[16:24])
            if head[:6] in (b"GIF87a", b"GIF89a"):
                return struct.unpack("<HH", head[6:10])
            if head[:2] == b"\xff\xd8":
                stream.seek(2)
                for _ in range(512):
                    marker = stream.read(2)
                    while marker[:1] == b"\xff" and marker[1:] == b"\xff":
                        marker = b"\xff" + stream.read(1)
                    if marker[:1] != b"\xff" or marker[1] in (0xDA, 0xD9):
                        break
                    size = struct.unpack(">H", stream.read(2))[0]
                    if marker[1] in (0xC0, 0xC1, 0xC2):
                        _, h, w = struct.unpack(">BHH", stream.read(5))
                        return w, h
                    if size < 2:
                        break
                    stream.seek(size - 2, 1)
    except (OSError, ValueError, IndexError, struct.error):
        pass
    return None


@dataclass
class Token:
    kind: str
    text: str = ""
    emphasis: bool = False
    answer: bool = False


class CardHTML(HTMLParser):
    def __init__(self, source):
        super().__init__(convert_charrefs=True)
        self.tokens = []
        self.stack = []
        self.suppressed = 0
        self.interactive = False
        self.feed(source)
        self.close()

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag in ("script", "style") or self.suppressed:
            self.suppressed += 1
            if tag == "script":
                self.interactive = True
            return
        if tag in ("b", "strong", "h1", "h2", "h3") or "cloze" in attrs.get("class", "").split():
            self.stack.append(tag)
        if tag in ("div", "p", "h1", "h2", "h3", "li", "tr", "blockquote", "br"):
            self.tokens.append(Token("break"))
        if tag == "li":
            self.tokens.append(Token("text", "• "))
        elif tag == "hr":
            self.tokens.append(Token("rule", answer=attrs.get("id") == "answer"))
        elif tag == "img":
            self.tokens.append(Token("image", attrs.get("src", "")))
        elif tag == "rt":
            self.tokens.append(Token("text", " ("))
        elif tag in ("input", "textarea", "iframe", "video", "audio", "canvas"):
            self.interactive = True

    def handle_endtag(self, tag):
        if self.suppressed:
            self.suppressed -= 1
            return
        if tag in self.stack:
            self.stack.remove(tag)
        if tag in ("div", "p", "h1", "h2", "h3", "li", "tr", "blockquote"):
            self.tokens.append(Token("break"))
        elif tag == "rt":
            self.tokens.append(Token("text", ")"))

    def handle_data(self, data):
        if not self.suppressed:
            data = re.sub(r"\[anki:play:[^]]+\]", "", data)
            self.tokens.append(Token("text", re.sub(r"\s+", " ", data), bool(self.stack)))


def layout(source, canvas, root, width=568):
    """Return positioned items, content height and the answer-rule anchor."""
    parsed = CardHTML(source)
    items, x, y, anchor = [], 0, 0, 0
    line_height = 38

    def newline():
        nonlocal x, y
        if x:
            x, y = 0, y + line_height

    for token in parsed.tokens:
        if token.kind == "break":
            newline()
        elif token.kind == "rule":
            newline()
            y += 10
            items.append(("rule", 0, y, width, None))
            y += 24
            if token.answer:
                anchor = y
        elif token.kind == "image":
            newline()
            path = media_path(root, token.text)
            dimensions = image_size(path) if path else None
            if dimensions and all(0 < value <= 8192 for value in dimensions) and path.stat().st_size <= 16_000_000:
                scale = min(width / dimensions[0], 180 / dimensions[1], 1)
                w, h = max(1, int(dimensions[0] * scale)), max(1, int(dimensions[1] * scale))
                items.append(("image", (width - w) // 2, y, (w, h), path))
                y += h + 16
            else:
                items.append(("small", 0, y, "[Image unavailable]", False))
                y += line_height
        else:
            # Wrap Latin words together when possible; CJK can wrap per character.
            for word in re.findall(r"[\x21-\x7e]+|.", token.text):
                word_width = sum(canvas.width(char) * 2 for char in word)
                if x and x + word_width > width and word_width <= width:
                    newline()
                for char in word:
                    cw = canvas.width(char) * 2
                    if x + cw > width:
                        newline()
                    if char == " " and not x:
                        continue
                    items.append(("glyph", x, y, char, token.emphasis))
                    x += cw
    newline()
    if parsed.interactive:
        items.append(("small", 0, y, "Interactive content omitted", False))
        y += 24
    return items, max(y, line_height), anchor
