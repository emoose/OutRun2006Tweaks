#!/usr/bin/env python3
"""Static OR2006C2C.EXE HUD/render anchor analyzer.

No third-party packages are required. The script parses PE32 headers, fingerprints
known OutRun rendering functions, finds direct x86 CALL rel32 references to known
HUD/sprite targets, and emits JSON + Markdown suitable for CI artifacts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from dataclasses import dataclass
from pathlib import Path

KNOWN_TARGETS = {
    0x02CFE0: "put_sprite_ex",
    0x029580: "sprani_play_ae_auth_alpha",
    0x02D280: "put_clip_sprite",
    0x02CCB0: "sprSetFontPriority",
    0x02CA60: "sprSetPrintFont",
    0x02CCA0: "sprSetFontColor",
    0x02CC60: "sprSetFontScale",
    0x02CC00: "sprLocateP",
    0x02CCE0: "sprPrintf",
    0x02CDD0: "Sumo_Printf",
    0x049940: "Calc3D2D",
    0x0BAD20: "RankMarker_sub_4BAD20",
}

KNOWN_CALL_SITES = {
    0x0BB0FB: "RankMarker sprani #1",
    0x0BB133: "RankMarker sprani #2",
    0x0BB16C: "RankMarker sprani #3",
    0x0BB1A5: "RankMarker sprani #4",
    0x0BB21F: "RankMarker clip #1",
    0x0BB241: "RankMarker clip #2",
    0x0BB271: "RankMarker clip #3",
    0x0BB2BC: "RankMarker clip #4",
    0x0BB2D0: "RankMarker clip #5",
}

# Mirrors src/vr/hud_semantics.hpp. These ranges come from the shipped
# hooks_uiscaling.cpp reverse engineering and are intentionally semantic,
# rather than D3D primitive-count heuristics.
SEMANTIC_RANGES = (
    (0x05B300, 0x05B700, "HeartDisp_car_heart", "WORLD_HEART", "WORLD_BILLBOARD"),
    (0x081A00, 0x081B00, "C2C_Fruit", "HUD_FRUIT", "SCREEN_HUD"),
    (0x081B00, 0x081C00, "C2C_Heart", "HUD_HEART_TOTAL", "SCREEN_HUD"),
    (0x096A80, 0x096D00, "C2CSpeechBubble", "HUD_GF_SPEECH", "SCREEN_HUD"),
    (0x0B9000, 0x0B9200, "DispGearPosition", "HUD_GEAR_REV", "SCREEN_HUD"),
    (0x0B9E00, 0x0BA100, "DispRank", "HUD_RANK", "SCREEN_HUD"),
    (0x0BAD20, 0x0BB320, "RankMarker/sub_4BAD20", "WORLD_RIVAL_MARKER", "WORLD_BILLBOARD"),
    (0x0BD2E0, 0x0BD360, "C2CTestSlipstream", "HUD_SLIPSTREAM", "SCREEN_HUD"),
    (0x0BD360, 0x0BD500, "C2CDontLoseGF", "HUD_GF_WARNING", "SCREEN_HUD"),
    (0x0BD900, 0x0BE100, "GhostGap", "HUD_GHOST", "SCREEN_HUD"),
    (0x0BE300, 0x0BEA40, "DispTimeAttack2D", "HUD_TIME_ATTACK", "SCREEN_HUD"),
    (0x0BEA40, 0x0BEB20, "NaviPub_DispTimeAttackGoal", "HUD_GOAL_TIME", "SCREEN_HUD"),
    (0x0BEB60, 0x0BEBC0, "NaviPub_Disp_Rival", "HUD_RIVAL", "SCREEN_HUD"),
    (0x0BEBC0, 0x0BEC50, "NaviPub_Disp_Heart", "HUD_HEART_TOTAL", "SCREEN_HUD"),
    (0x0BEC50, 0x0BECA0, "NaviPub_Disp_Rival", "HUD_RIVAL", "SCREEN_HUD"),
    (0x0BECA0, 0x0BED20, "NaviPub_Disp_Heart", "HUD_HEART_TOTAL", "SCREEN_HUD"),
    (0x0BED20, 0x0BEE80, "NaviPub_Disp", "HUD_NAV_GENERIC", "SCREEN_HUD"),
    (0x0FC800, 0x0FC8A0, "C2CSpeechBubbleGF_RankEmoji", "HUD_RANK_EMOJI", "SCREEN_HUD"),
    (0x0FC8A0, 0x0FC900, "C2CSpeechBubbleGF_RankText", "HUD_RANK_TEXT", "SCREEN_HUD"),
    (0x0FC900, 0x0FCC00, "C2CSpeechBubbleGF", "HUD_GF_SPEECH", "SCREEN_HUD"),
    (0x0FCD80, 0x0FD080, "C2CSpeechBubbleGF", "HUD_GF_SPEECH", "SCREEN_HUD"),
    (0x0FD560, 0x0FD680, "C2CSpeechBubbleGF", "HUD_GF_SPEECH", "SCREEN_HUD"),
    (0x0FE860, 0x0FE900, "C2CSpeechBubbleGF", "HUD_GF_SPEECH", "SCREEN_HUD"),
)


def classify_semantic(call_rva: int) -> tuple[str, str, str]:
    for begin, end, area, semantic, space_policy in SEMANTIC_RANGES:
        if begin <= call_rva < end:
            return area, semantic, space_policy
    return "", "UNKNOWN", "UNKNOWN"


HUD_WORDS = re.compile(
    rb"(rank|rival|score|position|goal|time|yes|no|retry|stage|ghost|lap|"
    rb"checkpoint|game.?over|outrun)",
    re.IGNORECASE,
)


@dataclass
class Section:
    name: str
    virtual_size: int
    virtual_address: int
    raw_size: int
    raw_pointer: int

    def contains_rva(self, rva: int) -> bool:
        return self.virtual_address <= rva < (
            self.virtual_address + max(self.virtual_size, self.raw_size)
        )

    def rva_to_offset(self, rva: int) -> int:
        return self.raw_pointer + (rva - self.virtual_address)


@dataclass
class PE:
    data: bytes
    image_base: int
    entry_rva: int
    timestamp: int
    size_of_image: int
    sections: list[Section]

    def section(self, name: str) -> Section | None:
        return next((s for s in self.sections if s.name == name), None)

    def bytes_at_rva(self, rva: int, size: int) -> bytes:
        for section in self.sections:
            if section.contains_rva(rva):
                off = section.rva_to_offset(rva)
                return self.data[off : off + size]
        return b""


def u16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def parse_pe(data: bytes) -> PE:
    if data[:2] != b"MZ":
        raise ValueError("not an MZ executable")
    pe_off = u32(data, 0x3C)
    if data[pe_off : pe_off + 4] != b"PE\0\0":
        raise ValueError("PE signature not found")

    file_header = pe_off + 4
    section_count = u16(data, file_header + 2)
    timestamp = u32(data, file_header + 4)
    optional_size = u16(data, file_header + 16)
    optional = file_header + 20
    magic = u16(data, optional)
    if magic != 0x10B:
        raise ValueError(f"expected PE32/x86 optional header, got 0x{magic:04x}")

    entry_rva = u32(data, optional + 16)
    image_base = u32(data, optional + 28)
    size_of_image = u32(data, optional + 56)

    sections: list[Section] = []
    section_off = optional + optional_size
    for i in range(section_count):
        off = section_off + i * 40
        name = data[off : off + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        sections.append(
            Section(
                name=name,
                virtual_size=u32(data, off + 8),
                virtual_address=u32(data, off + 12),
                raw_size=u32(data, off + 16),
                raw_pointer=u32(data, off + 20),
            )
        )

    return PE(
        data=data,
        image_base=image_base,
        entry_rva=entry_rva,
        timestamp=timestamp,
        size_of_image=size_of_image,
        sections=sections,
    )


def guess_function_start(text: bytes, text_rva: int, index: int) -> int | None:
    lo = max(0, index - 0x500)
    prologues = (b"\x55\x8b\xec", b"\x53\x56\x57", b"\x56\x8b\xf1")
    for pos in range(index, lo - 1, -1):
        if any(text.startswith(p, pos) for p in prologues):
            return text_rva + pos
    return None


def find_calls(pe: PE) -> list[dict]:
    text_section = pe.section(".text")
    if not text_section:
        raise ValueError(".text section not found")

    text = pe.data[
        text_section.raw_pointer :
        text_section.raw_pointer + text_section.raw_size
    ]
    found: list[dict] = []

    for i in range(0, max(0, len(text) - 5)):
        if text[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", text, i + 1)[0]
        call_rva = text_section.virtual_address + i
        next_va = pe.image_base + call_rva + 5
        target_va = (next_va + rel) & 0xFFFFFFFF
        target_rva = (target_va - pe.image_base) & 0xFFFFFFFF
        name = KNOWN_TARGETS.get(target_rva)
        if not name:
            continue
        area, semantic, space_policy = classify_semantic(call_rva)
        found.append(
            {
                "call_rva": call_rva,
                "target_rva": target_rva,
                "target": name,
                "function_start_guess_rva": guess_function_start(
                    text, text_section.virtual_address, i
                ),
                "known_call_site": KNOWN_CALL_SITES.get(call_rva, ""),
                "known_area": area,
                "semantic": semantic,
                "space_policy": space_policy,
            }
        )
    return found


def extract_hud_strings(pe: PE) -> list[dict]:
    results: list[dict] = []
    for section in pe.sections:
        blob = pe.data[section.raw_pointer : section.raw_pointer + section.raw_size]
        for match in re.finditer(rb"[ -~]{4,120}", blob):
            value = match.group(0)
            if not HUD_WORDS.search(value):
                continue
            results.append(
                {
                    "rva": section.virtual_address + match.start(),
                    "section": section.name,
                    "text": value.decode("ascii", "replace"),
                }
            )
    return results[:500]


def symbol_fingerprints(pe: PE) -> list[dict]:
    out = []
    for rva, name in sorted(KNOWN_TARGETS.items()):
        out.append(
            {
                "rva": rva,
                "name": name,
                "bytes24": pe.bytes_at_rva(rva, 24).hex(" "),
            }
        )
    return out


def hexrva(value: int | None) -> str:
    return "-" if value is None else f"0x{value:08X}"


def render_markdown(report: dict) -> str:
    lines = [
        "# OR2006C2C EXE / HUD static analysis",
        "",
        "Generated automatically from the public replacement EXE used by the upstream OutRun2006Tweaks build.",
        "",
        "## EXE identity",
        "",
        f"- SHA-256: {report['sha256']}",
        f"- Size: {report['file_size']} bytes",
        f"- PE timestamp: {report['pe']['timestamp']}",
        f"- Image base: {hexrva(report['pe']['image_base'])}",
        f"- Entry RVA: {hexrva(report['pe']['entry_rva'])}",
        f"- SizeOfImage: {report['pe']['size_of_image']} bytes",
        f"- Known HUD call-site validation: {report['known_call_sites_found']}/{report['known_call_sites_expected']}",
        "",
        "## Known HUD/render anchors",
        "",
        "| RVA | Symbol | First 24 bytes |",
        "|---:|---|---|",
    ]
    for sym in report["symbols"]:
        lines.append(
            f"| {hexrva(sym['rva'])} | {sym['name']} | {sym['bytes24']} |"
        )

    lines += [
        "",
        "## Direct CALL references into HUD/sprite anchors",
        "",
        "| Call RVA | Function start guess | Target | Known site | Semantic | Space |",
        "|---:|---:|---|---|---|---|",
    ]
    for call in report["calls"]:
        lines.append(
            f"| {hexrva(call['call_rva'])} | "
            f"{hexrva(call['function_start_guess_rva'])} | "
            f"{call['target']} | {call['known_call_site']} | "
            f"{call['semantic']} | {call['space_policy']} |"
        )

    lines += [
        "",
        "## HUD-related ASCII strings",
        "",
        "| RVA | Section | Text |",
        "|---:|---|---|",
    ]
    for item in report["hud_strings"]:
        text = item["text"].replace("|", "\\|")
        lines.append(
            f"| {hexrva(item['rva'])} | {item['section']} | {text} |"
        )

    lines += [
        "",
        "## Runtime correlation",
        "",
        "The VR HUD inspector writes OutRun2006Tweaks-hudtrace.csv. "
        "Match its call_rva column against this report. UIScaling-derived semantic "
        "classification labels Time Attack, Rank, REV/gear, Ghost, goal time, "
        "Heart, Rival, girlfriend speech/rank UI, and protects world-attached "
        "Rival/Heart billboards from screen-HUD treatment. Previously unknown "
        "callers remain concrete reverse-engineering targets without requiring "
        "an interactive debugger.",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    args = parser.parse_args()

    data = args.exe.read_bytes()
    pe = parse_pe(data)
    calls = find_calls(pe)
    found_call_rvas = {item["call_rva"] for item in calls}
    missing_known_call_sites = [
        {"rva": rva, "label": label}
        for rva, label in KNOWN_CALL_SITES.items()
        if rva not in found_call_rvas
    ]

    report = {
        "source": str(args.exe),
        "sha256": hashlib.sha256(data).hexdigest(),
        "file_size": len(data),
        "pe": {
            "image_base": pe.image_base,
            "entry_rva": pe.entry_rva,
            "timestamp": pe.timestamp,
            "size_of_image": pe.size_of_image,
            "sections": [s.__dict__ for s in pe.sections],
        },
        "symbols": symbol_fingerprints(pe),
        "calls": calls,
        "known_call_sites_expected": len(KNOWN_CALL_SITES),
        "known_call_sites_found": len(KNOWN_CALL_SITES) - len(missing_known_call_sites),
        "missing_known_call_sites": missing_known_call_sites,
        "hud_strings": extract_hud_strings(pe),
    }

    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "OR2006C2C_EXE_ANALYSIS.json").write_text(
        json.dumps(report, indent=2), encoding="utf-8"
    )
    (args.out_dir / "OR2006C2C_EXE_ANALYSIS.md").write_text(
        render_markdown(report), encoding="utf-8"
    )

    print(f"sha256={report['sha256']}")
    print(f"calls={len(report['calls'])}")
    print(
        f"known_call_sites={report['known_call_sites_found']}/"
        f"{report['known_call_sites_expected']}"
    )
    print(f"hud_strings={len(report['hud_strings'])}")
    if missing_known_call_sites:
        for item in missing_known_call_sites:
            print(
                f"missing_known_call_site=0x{item['rva']:08X} "
                f"{item['label']}"
            )
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())