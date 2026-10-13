#!/usr/bin/env python3
"""Add or update one <game> in an EmulationStation gamelist.xml (runs ON the RG351V).

Usage: gamelist-set.py <gamelist.xml> <path> <name> <desc> [developer]

An existing entry with the same <path> is updated in place, so metadata ES
keeps (playcount, lastplayed, ...) survives redeploys. Stop emustation
before running: ES rewrites this file when it quits.
"""
import sys
import xml.etree.ElementTree as ET

if len(sys.argv) < 5:
    sys.exit(__doc__)
xml_path, game_path, name, desc = sys.argv[1:5]
developer = sys.argv[5] if len(sys.argv) > 5 else ""

tree = ET.parse(xml_path)
root = tree.getroot()
game = next((g for g in root.findall("game") if g.findtext("path") == game_path), None)
if game is None:
    game = ET.SubElement(root, "game")
    ET.SubElement(game, "path").text = game_path

fields = {"name": name, "desc": desc, "hidden": "false"}
if developer:
    fields["developer"] = developer
for tag, value in fields.items():
    el = game.find(tag)
    if el is None:
        el = ET.SubElement(game, tag)
    el.text = value

if hasattr(ET, "indent"):
    ET.indent(tree, space="\t")
tree.write(xml_path, encoding="utf-8", xml_declaration=True)
print("gamelist: %s -> %s" % (game_path, name))
