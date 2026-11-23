#!/usr/bin/env python3
"""Build the GitHub Pages Profile Studio bundle from vendor/mk12-vcu.

Reads vendor/mk12-vcu/tools/profile_editor/index.html and
vendor/mk12-vcu/profiles/base.yaml, injects the live base.yaml defaults,
aligns the FNV-1a profile hash with sil/profile_loader.c, and updates the
Assetto Sim tab to document the bfr_vcu.exe drag-and-drop / --profile workflow.

Usage:
  python3 sil/build_pages.py [OUT_DIR]
"""

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VCU_DIR = ROOT / "vendor" / "mk12-vcu"
EDITOR_DIR = VCU_DIR / "tools" / "profile_editor"
BASE_YAML = VCU_DIR / "profiles" / "base.yaml"
DEFAULT_OUT = ROOT / "sil" / "build" / "pages"
KIT_ACTIONS_URL = "https://github.com/bruinformula/mk12-assetto-sim/actions/workflows/sil.yml"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"build_pages: expected 1 match for {label}, found {count}")
    return text.replace(old, new, 1)


def parse_base_yaml(path: Path) -> dict:
    """Parse vendor/mk12-vcu/profiles/base.yaml using gen_profile.py's subset."""
    data: dict = {}
    sec = None
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = re.sub(r"(^|\s)#.*$", "", raw).rstrip()
        if not line.strip():
            continue
        m = re.match(r"^(\s*)([a-z_][a-z0-9_]*):\s*(.*)$", line, re.I)
        if not m:
            raise ValueError(f"{path}:{lineno}: cannot parse {raw!r}")
        indent, key, val = m.groups()
        if not indent:
            if val == "":
                sec = data[key] = {}
            else:
                data[key] = val
                sec = None
        else:
            if sec is None:
                raise ValueError(f"{path}:{lineno}: key outside section")
            if val.startswith("[") and val.endswith("]"):
                sec[key] = [
                    int(v) if (v := float(x.strip())).is_integer() else v
                    for x in val[1:-1].split(",")
                    if x.strip()
                ]
            else:
                v = float(val)
                sec[key] = int(v) if v.is_integer() else v
    for required in ("name", "torque", "pedal", "regen", "filter"):
        if required not in data:
            raise ValueError(f"{path}: missing {required}")
    return data


def vcu_short_sha() -> str:
    try:
        out = subprocess.check_output(
            ["git", "-C", str(VCU_DIR), "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
        return out
    except Exception:
        return "assetto"


def build(out_dir: Path) -> None:
    if not (EDITOR_DIR / "index.html").exists():
        raise FileNotFoundError(
            f"Missing {EDITOR_DIR / 'index.html'}. Run: git submodule update --init"
        )

    base = parse_base_yaml(BASE_YAML)
    sha = vcu_short_sha()
    html = (EDITOR_DIR / "index.html").read_text(encoding="utf-8")

    # 1. Update header badge to show live mk12-vcu submodule sync.
    html = replace_once(
        html,
        '<span class="sample-badge">Design preview · sample data</span>',
        f'<span class="sample-badge">Synced · mk12-vcu/assetto ({sha})</span>',
        "header sample badge",
    )

    # 2. Point GitHub Propose links at the assetto branch.
    html = replace_once(
        html,
        'const BRANCH = "main";',
        'const BRANCH = "assetto";',
        "BRANCH constant",
    )

    # 3. Inject real base.yaml defaults into BASE.
    base_pat = re.compile(r"// [^\n]*\nconst BASE = \{[\s\S]*?\};")
    new_base = (
        f"// Injected from vendor/mk12-vcu/profiles/base.yaml ({sha}).\n"
        f"const BASE = {json.dumps(base, separators=(',', ':'))};"
    )
    html, n_base = base_pat.subn(new_base, html, count=1)
    if n_base != 1:
        raise RuntimeError(f"build_pages: expected 1 match for BASE profile injection, found {n_base}")

    # 3b. Refresh cached base profile from BASE and drop removed legacy sample profiles.
    old_load_cache = (
        '  if (d && d.profiles) { profiles = d.profiles; current = d.current in profiles ? d.current : current;\n'
        "    saved = d.saved || saved; }"
    )
    new_load_cache = (
        '  if (d && d.profiles) { profiles = d.profiles; saved = d.saved || saved;\n'
        '    for (const legacy of ["endurance", "idan_autox"]) {\n'
        "      if (profiles[legacy] && saved[legacy] === toYaml(profiles[legacy])) { delete profiles[legacy]; delete saved[legacy]; }\n"
        "    }\n"
        "    if (!profiles.base || saved.base === toYaml(profiles.base)) {\n"
        "      profiles.base = structuredClone(BASE); saved.base = toYaml(profiles.base);\n"
        "    }\n"
        '    current = d.current in profiles ? d.current : "base"; }'
    )
    html = replace_once(html, old_load_cache, new_load_cache, "refresh unmodified base from firmware")

    # 4. Align FNV-1a hash() with sil/profile_loader.c (fixed key order + num() formatting).
    old_hash = (
        "function hash(e) {\n"
        '  const s = JSON.stringify(e, (k, v) => k === "name" ? undefined : v);\n'
        "  let h = 0x811c9dc5;\n"
        "  for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 0x01000193); }\n"
        '  return (h >>> 0).toString(16).padStart(8, "0");\n'
        "}"
    )
    new_hash = (
        "function hash(e) {\n"
        "  const canon = JSON.stringify({\n"
        "    torque: { max_nm: +num(e.torque.max_nm), pedal_table: e.torque.pedal_table.map(v => +num(v)) },\n"
        "    pedal: { coast_end: +num(e.pedal.coast_end), drive_start: +num(e.pedal.drive_start) },\n"
        "    regen: { liftoff_nm: +num(e.regen.liftoff_nm), fade_lo_kph: +num(e.regen.fade_lo_kph),\n"
        "             fade_hi_kph: +num(e.regen.fade_hi_kph), ramp_nm_s: +num(e.regen.ramp_nm_s) },\n"
        "    filter: { tau_s: +num(e.filter.tau_s), slew_nm_s: +num(e.filter.slew_nm_s) },\n"
        "  });\n"
        "  let h = 0x811c9dc5;\n"
        "  for (let i = 0; i < canon.length; i++) { h ^= canon.charCodeAt(i); h = Math.imul(h, 0x01000193); }\n"
        '  return (h >>> 0).toString(16).padStart(8, "0");\n'
        "}"
    )
    html = replace_once(html, old_hash, new_hash, "canonical FNV-1a hash()")

    # 5. Update Assetto Sim Setup -> Package card to the live bfr_vcu.exe SIL kit workflow.
    old_sim_pkg = (
        '          <aside class="card curve-card sim-package"><div class="card-head"><div><div class="eyebrow">Assetto Corsa / bfr_mk12</div><h2>Package preview</h2></div><span class="sim-planned">Planned</span></div>\n'
        '            <div class="sim-file-tree"><div class="sim-tree-root"><svg width="18" height="15" viewBox="0 0 18 15" aria-hidden="true"><path d="M1 4V1h6l2 3h8v10H1Z" fill="none" stroke="currentColor"/></svg><span>bfr_mk12 / data</span></div><div class="sim-tree-file"><span aria-hidden="true">└</span><code>profiles.lua</code><small>Profile values</small></div><div class="sim-tree-file"><span aria-hidden="true">└</span><code>profiles_index.ini</code><small>Names &amp; identifiers</small></div></div>\n'
        '            <p class="sim-package-note">This is the proposed package layout. Assetto export is not available in this preview.</p><button id="simPackExport" class="sim-pack-button" disabled>Export Assetto package <span aria-hidden="true">↗</span></button>\n'
        '            <div class="sim-source-download"><span>Available now</span><button id="simSaveYaml" class="sim-save">Save profile YAML <span aria-hidden="true">↗</span></button><p>Source settings for this profile. The YAML is not an installable Assetto package.</p></div>\n'
        "          </aside>"
    )
    new_sim_pkg = (
        '          <aside class="card curve-card sim-package"><div class="card-head"><div><div class="eyebrow">Assetto Corsa / bfr_mk12</div><h2>SIL VCU Kit</h2></div><span class="sim-planned">Live</span></div>\n'
        '            <div class="sim-file-tree"><div class="sim-tree-root"><svg width="18" height="15" viewBox="0 0 18 15" aria-hidden="true"><path d="M1 4V1h6l2 3h8v10H1Z" fill="none" stroke="currentColor"/></svg><span>bfr_vcu_kit</span></div><div class="sim-tree-file"><span aria-hidden="true">├</span><code>bfr_vcu.exe</code><small>Real MK12 C firmware</small></div><div class="sim-tree-file"><span aria-hidden="true">├</span><code>base.yaml</code><small>Default firmware map</small></div><div class="sim-tree-file"><span aria-hidden="true">└</span><code>data/vcu_bridge.lua</code><small>333 Hz CSP bridge</small></div></div>\n'
        '            <p class="sim-package-note">Download the Windows kit once from CI, then drag any exported <code>.yaml</code> directly onto <code>bfr_vcu.exe</code>.</p><button id="simPackExport" class="sim-pack-button">Download bfr_vcu_kit <span aria-hidden="true">↗</span></button>\n'
        '            <div class="sim-source-download"><span>Step 1 · Export profile</span><button id="simSaveYaml" class="sim-save">Save profile YAML <span aria-hidden="true">↗</span></button><p>Drag the saved <code>.yaml</code> onto <code>bfr_vcu.exe</code> or run <code>bfr_vcu.exe --profile &lt;file&gt;.yaml</code>.</p></div>\n'
        "          </aside>"
    )
    html = replace_once(html, old_sim_pkg, new_sim_pkg, "Assetto Sim package card")

    # 6. Update Assetto Sim Setup -> Install guide tab to reflect bfr_vcu.exe workflow.
    old_install = (
        '      <div data-sim-pane="install" hidden>\n'
        '        <div class="sim-guide-intro"><div><h2>From profile to car folder.</h2><p>Installation flow preview for a future Assetto export. Requires the team’s profile-compatible MK12 car and Custom Shaders Patch.</p></div><span class="sim-guide-tag">Guide preview</span></div>\n'
        '        <div class="sim-install-layout"><div class="card sim-install-steps"><article><span class="sim-step-number">01</span><div><h3>Open the installed MK12 car</h3><p>Use the team’s car folder inside your Assetto installation. Keep a backup before replacing any files.</p><div class="sim-path"><code>assettocorsa\\content\\cars\\bfr_mk12\\data\\</code><button id="simCopyPath" aria-label="Copy relative Assetto car data path">Copy path</button></div><small>Relative to the folder containing your Assetto installation.</small></div></article><article><span class="sim-step-number">02</span><div><h3>Copy the exported files</h3><p>Once package export is available, copy its contents into the matching car folder and preserve the folder structure. The package README will list the exact files.</p><p class="sim-inline-note">A packed <code>data.acd</code> takes precedence over the <code>data</code> folder. Follow the team’s car-install instructions for an unpacked setup.</p></div></article><article><span class="sim-step-number">03</span><div><h3>Confirm the selected profile in-game</h3><p>The planned profile-aware car app should show the selected profile’s name and generated version. Match those before evaluating a change.</p><div class="sim-expected-label"><span>Expected profile name</span><strong id="simExpectedName"></strong><small>In-game profile display is planned.</small></div></div></article></div>'
    )
    new_install = (
        '      <div data-sim-pane="install" hidden>\n'
        '        <div class="sim-guide-intro"><div><h2>From profile to Assetto Corsa.</h2><p>Run the real MK12 VCU firmware alongside Assetto Corsa with zero Python, git, or compiling on Windows. Requires Custom Shaders Patch.</p></div><span class="sim-guide-tag">SIL v2</span></div>\n'
        '        <div class="sim-install-layout"><div class="card sim-install-steps"><article><span class="sim-step-number">01</span><div><h3>Install the SIL car script (one-time)</h3><p>Download <code>bfr_vcu_kit</code> from GitHub Actions and copy its <code>data\\</code> files (<code>script.lua</code>, <code>vcu_bridge.lua</code>, <code>brakes.lua</code>, <code>drivetrain.ini</code>) into your MK12 car folder.</p><div class="sim-path"><code>assettocorsa\\content\\cars\\bfr_mk12\\data\\</code><button id="simCopyPath" aria-label="Copy relative Assetto car data path">Copy path</button></div><p class="sim-inline-note">Rename or remove any packed <code>data.acd</code> in <code>bfr_mk12\\</code> so Assetto Corsa reads the <code>data\\</code> folder.</p></div></article><article><span class="sim-step-number">02</span><div><h3>Drag your .yaml onto bfr_vcu.exe</h3><p>Click <b>Save profile YAML</b> to download your profile, then drag and drop the <code>.yaml</code> file directly onto <code>bfr_vcu.exe</code> (or run <code>bfr_vcu.exe --profile name.yaml</code>).</p><p class="sim-inline-note">TC runs in shadow mode by default (100 Hz logs saved in <code>logs\\</code>). Pass <code>--tc-active</code> from a terminal for live TC cuts, or <code>--sleep</code> for lower CPU usage.</p></div></article><article><span class="sim-step-number">03</span><div><h3>Verify the profile hash in Lua Debug</h3><p>Start Assetto Corsa in 1st gear and open CSP’s <b>Lua Debug</b> app. Confirm <code>01 VCU link</code> shows <code>CONNECTED</code> and <code>02 profile hash</code> matches below:</p><div class="sim-expected-label"><span>Expected profile &amp; hash</span><strong id="simExpectedName"></strong><small>Matches bfr_vcu.exe console and CSP Lua Debug (02 profile hash).</small></div></div></article></div>'
    )
    html = replace_once(html, old_install, new_install, "Assetto Sim install guide")

    # 7. Show "<name> · <hash>" in simExpectedName, enable simPackExport, and wire its click handler.
    html = replace_once(
        html,
        "simExpectedName:simSetup.profile,",
        "simExpectedName:`${simSetup.profile} · ${hash(e)}`,",
        "simExpectedName hash display",
    )
    html = replace_once(
        html,
        'document.getElementById("simPackExport").disabled=true;',
        'document.getElementById("simPackExport").disabled=false;',
        "enable simPackExport button",
    )
    html = replace_once(
        html,
        'document.getElementById("simSaveYaml").addEventListener("click",()=>exportProfileYaml(simSetup.profile));',
        (
            'document.getElementById("simSaveYaml").addEventListener("click",()=>exportProfileYaml(simSetup.profile));\n'
            f'document.getElementById("simPackExport").addEventListener("click",()=>window.open("{KIT_ACTIONS_URL}","_blank","noopener"));'
        ),
        "wire simPackExport download link",
    )

    # Also update tab label subtitle slightly.
    html = replace_once(
        html,
        "<small>Preview the handoff to Assetto</small>",
        "<small>Run with bfr_vcu.exe</small>",
        "sim install step subtitle",
    )

    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "index.html").write_text(html, encoding="utf-8")
    shutil.copy2(EDITOR_DIR / "THIRD_PARTY_NOTICES.md", out_dir / "THIRD_PARTY_NOTICES.md")
    shutil.copy2(BASE_YAML, out_dir / "base.yaml")
    print(f"built Profile Studio in {out_dir} (base.yaml from mk12-vcu {sha})")


if __name__ == "__main__":
    dest = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_OUT
    build(dest)
