#!/usr/bin/env python3
"""Export untranslated strings from a Qt .ts catalogue and merge answers back.

Two commands, both operating on i18n/lightning_<code>.ts in place:

    catalog.py export <code> [--start N] [--count N] [--numerus-only]
        Prints TSV: <index>\t<numerus?>\t<source>. `source` has literal tabs
        and newlines escaped as \\t and \\n so one message is always one line.

    catalog.py merge <code> <answers.tsv>
        Reads TSV: <index>\t<translation>[\t<plural form>...] and writes each
        into the message at that index, clearing type="unfinished".

    catalog.py status
        Per-language counts.

Merging REFUSES a translation that does not carry the same %1..%9 / %n
placeholders as its source, and reports each refusal. A dropped placeholder is
not a cosmetic problem: Qt substitutes arguments positionally, so a missing %1
silently loses the room name and an extra one prints a literal "%2".
"""
import re
import sys
import xml.etree.ElementTree as ET

PLACEHOLDER = re.compile(r"%(n|[1-9])")


def ts_path(code):
    return f"i18n/lightning_{code}.ts"


def load(code):
    parser = ET.XMLParser(target=ET.TreeBuilder(insert_comments=False))
    tree = ET.parse(ts_path(code), parser=parser)
    return tree


def messages(tree):
    """Every <message>, in document order, paired with its context name."""
    out = []
    for context in tree.getroot().findall("context"):
        name = context.findtext("name") or ""
        for message in context.findall("message"):
            out.append((name, message))
    return out


def escape(text):
    return (text or "").replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")


def unescape(text):
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            n = text[i + 1]
            if n == "n":
                out.append("\n"); i += 2; continue
            if n == "t":
                out.append("\t"); i += 2; continue
            if n == "\\":
                out.append("\\"); i += 2; continue
        out.append(c)
        i += 1
    return "".join(out)


def is_unfinished(message):
    translation = message.find("translation")
    return translation is not None and translation.get("type") == "unfinished"


def cmd_export(code, start, count, numerus_only):
    tree = load(code)
    shown = 0
    for index, (context, message) in enumerate(messages(tree)):
        if not is_unfinished(message):
            continue
        numerus = message.get("numerus") == "yes"
        if numerus_only and not numerus:
            continue
        if index < start:
            continue
        source = message.findtext("source") or ""
        comment = message.findtext("extracomment") or message.findtext("comment") or ""
        print("\t".join([
            str(index),
            "n" if numerus else "-",
            escape(source),
            escape(comment),
        ]))
        shown += 1
        if count and shown >= count:
            break


def cmd_merge(code, answers):
    tree = load(code)
    all_messages = messages(tree)
    applied = 0
    refused = []
    for line in open(answers, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) < 2:
            refused.append((line[:60], "malformed"))
            continue
        try:
            index = int(parts[0])
        except ValueError:
            refused.append((line[:60], "bad index"))
            continue
        if index < 0 or index >= len(all_messages):
            refused.append((parts[0], "index out of range"))
            continue
        message = all_messages[index][1]
        source = message.findtext("source") or ""
        forms = [unescape(p) for p in parts[1:] if p != ""]
        if not forms:
            refused.append((parts[0], "empty translation"))
            continue
        wanted = sorted(PLACEHOLDER.findall(source))
        bad = None
        for form in forms:
            if sorted(PLACEHOLDER.findall(form)) != wanted:
                bad = form
                break
        if bad is not None:
            refused.append((parts[0], f"placeholder mismatch: {source!r} -> {bad!r}"))
            continue
        translation = message.find("translation")
        if translation is None:
            translation = ET.SubElement(message, "translation")
        numerus = message.get("numerus") == "yes"
        for child in list(translation):
            translation.remove(child)
        translation.text = None
        if numerus:
            for form in forms:
                node = ET.SubElement(translation, "numerusform")
                node.text = form
        else:
            translation.text = forms[0]
        if "type" in translation.attrib:
            del translation.attrib["type"]
        applied += 1
    tree.write(ts_path(code), encoding="utf-8", xml_declaration=True)
    print(f"applied={applied} refused={len(refused)}")
    for entry in refused[:20]:
        print("  REFUSED", entry[0], entry[1])


def cmd_status():
    import glob
    for path in sorted(glob.glob("i18n/lightning_*.ts")):
        code = path.split("lightning_")[1][:-3]
        tree = load(code)
        total = 0
        done = 0
        numerus_left = 0
        for _, message in messages(tree):
            total += 1
            if is_unfinished(message):
                if message.get("numerus") == "yes":
                    numerus_left += 1
            else:
                done += 1
        print(f"{code:6s} {done:5d}/{total:5d} done, {total - done:5d} left "
              f"({numerus_left} of them plural)")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    command = sys.argv[1]
    if command == "export":
        code = sys.argv[2]
        start = 0
        count = 0
        numerus_only = False
        rest = sys.argv[3:]
        i = 0
        while i < len(rest):
            if rest[i] == "--start":
                start = int(rest[i + 1]); i += 2
            elif rest[i] == "--count":
                count = int(rest[i + 1]); i += 2
            elif rest[i] == "--numerus-only":
                numerus_only = True; i += 1
            else:
                i += 1
        cmd_export(code, start, count, numerus_only)
        return 0
    if command == "merge":
        cmd_merge(sys.argv[2], sys.argv[3])
        return 0
    if command == "status":
        cmd_status()
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
