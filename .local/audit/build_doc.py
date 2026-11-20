#!/usr/bin/env python3
"""Assemble optimization.md from the audit reports, the verification verdicts and the fork status.

Inputs, all beside this script:
  NN-area.md    one report per audited area
  verdicts.tsv  ID|verdict|note
  status.tsv    ID|status|note   (what the fork did about it)
  head.md       the hand-written top of the document
  tail.md       the hand-written end of the document
Output: ../../optimization.md
"""
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
AREAS = [
    ("01-base-runtime.md", "Runtime, drivers, power and build", "BASE"),
    ("02-engine-host.md", "Audio engine: the render graph", "HOST"),
    ("03-sampler-looper.md", "Audio engine: sampler voices, streaming and the Looper", "SMP"),
    ("04-effects-dsp.md", "Audio engine: effects and analysis", "FX"),
    ("05-plugin-host.md", "Plug-in hosting", "PLUG"),
    ("06-shell-core.md", "Shell: service work, saving, settings", "SHELL"),
    ("07-lights-midi.md", "Lights, MIDI and controls", "LIGHT"),
    ("08-storage-io.md", "Storage, loading and background jobs", "IO"),
    ("09-ui-framework.md", "Screen: the frame loop and drawing framework", "UI"),
    ("10-ui-pages.md", "Screen: individual pages", "PAGE"),
    ("11-model-sequencing.md", "Project model and sequencing", "MODEL"),
]


def table(path):
    rows = {}
    if not path.exists():
        return rows
    for line in path.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("|")
        while len(parts) < 3:
            parts.append("")
        rows[parts[0].strip()] = (parts[1].strip(), parts[2].strip())
    return rows


def field(body, name):
    match = re.search(r"^- \*\*" + re.escape(name) + r":\*\*\s*(.*)$", body, re.M)
    return match.group(1).strip() if match else ""


def short(text, limit):
    text = re.sub(r"\s+", " ", text)
    if len(text) <= limit:
        return text
    cut = text[:limit].rsplit(" ", 1)[0].rstrip(",;:")
    return cut + " …"


def cell(text):
    return text.replace("|", "\\|")


def split_report(text, prefix):
    """Findings as (id, title, body), and the report's closing sections as {heading: text}."""
    findings, extras = [], {}
    sections = re.split(r"^## ", text, flags=re.M)
    for section in sections[1:]:
        heading, _, body = section.partition("\n")
        heading = heading.strip()
        if heading.startswith("Findings"):
            for chunk in re.split(r"^### ", body, flags=re.M)[1:]:
                title_line, _, rest = chunk.partition("\n")
                match = re.match(r"(" + prefix + r"-\d+):\s*(.*)", title_line.strip())
                if match:
                    findings.append((match.group(1), match.group(2).strip(), rest.strip()))
                else:  # an unnumbered note inside Findings (an inventory or a preface)
                    extras.setdefault("Notes", "")
                    extras["Notes"] += "#### " + title_line.strip() + "\n\n" + rest.strip() + "\n\n"
        elif heading.startswith("Coverage"):
            extras["Coverage"] = body.strip()
        else:
            extras[heading] = body.strip()
    return findings, extras


def main():
    verdicts, status = table(HERE / "verdicts.tsv"), table(HERE / "status.tsv")
    out = [(HERE / "head.md").read_text().rstrip(), ""]
    index, details, appendix = [], [], []
    counts = {}
    for file, name, prefix in AREAS:
        findings, extras = split_report((HERE / file).read_text(), prefix)
        index.append(f"### {name}\n")
        index.append("| ID | Finding | Gain | Sound or behaviour | Verified | In the fork |")
        index.append("| --- | --- | --- | --- | --- | --- |")
        details.append(f"## {name}\n")
        if "Notes" in extras:
            details.append(extras["Notes"].rstrip() + "\n")
        for ident, title, body in findings:
            verdict, note = verdicts.get(ident, ("Not checked", ""))
            done, done_note = status.get(ident, ("No change", ""))
            if verdict == "Merged" and ident not in status:  # what the fork did is under the finding it was merged into
                targets = re.findall(r"\b[A-Z]+-\d+\b", note)
                if targets:
                    done, done_note = "See " + targets[0], ""
            counts[verdict] = counts.get(verdict, 0) + 1
            index.append("| " + " | ".join(cell(x) for x in (
                ident, title, short(field(body, "Gain"), 70), short(field(body, "Sound or behaviour change"), 60),
                verdict, done)) + " |")
            details.append(f"### {ident}: {title}\n")
            details.append(f"- **Verification:** {verdict}. {note}".rstrip())
            details.append(f"- **In the fork:** {done}. {done_note}".rstrip())
            details.append("")
            if verdict == "Merged":
                continue
            details.append(body + "\n")
        index.append("")
        for heading in ("Checked and fine", "Could not determine"):
            if heading in extras and extras[heading]:
                appendix.append((heading, name, extras[heading]))
        for heading, text in extras.items():
            if heading not in ("Coverage", "Notes", "Checked and fine", "Could not determine") and text:
                appendix.append(("Other notes from the auditors", name + ": " + heading, text))
    out.append("## All findings at a glance\n")
    total = sum(counts.values())
    out.append(f"{total} findings: " + ", ".join(f"{n} {v.lower()}" for v, n in sorted(counts.items(), key=lambda c: -c[1])) + ".")
    out.append("\"Merged\" means the same issue was reported by more than one auditor and is written up once, under the ID its verification line names.\n")
    out += index
    out.append("# Findings in full\n")
    out.append("Each finding is as its auditor wrote it, with two lines added: what I found when I checked it against the code, and what the fork does about it. Costs are estimates from reading the code unless a line says it was measured.\n")
    out += details
    out.append("# Appendices\n")
    for wanted, title in (("Checked and fine", "Checked and found fine"), ("Could not determine", "Open questions that need the device"),
                          ("Other notes from the auditors", "Other notes from the auditors")):
        chosen = [(name, text) for heading, name, text in appendix if heading == wanted]
        if not chosen:
            continue
        out.append(f"## {title}\n")
        for name, text in chosen:
            out.append(f"### {name}\n")
            out.append(re.sub(r"^(#{1,4}) ", r"#### ", text, flags=re.M) + "\n")
    tail = HERE / "tail.md"
    if tail.exists():
        out.append(tail.read_text().rstrip() + "\n")
    target = HERE.parent.parent / "optimization.md"
    target.write_text("\n".join(out).rstrip() + "\n")
    print(target, target.stat().st_size, "bytes;", total, "findings;", counts)


if __name__ == "__main__":
    main()
