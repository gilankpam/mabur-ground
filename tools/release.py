#!/usr/bin/env python3
"""mabur-ground releases: the pin rule, the GS version rule, release notes.

GS releases are tagged v0.MINOR.PATCH, and MINOR is the drone release minor
they pair with: GS v0.M.x works with drone v0.M.x. The drone<->GS contract
(RC_VERSION, the CAP_* constants, the drone parameter table) is read by
mabur's own tools/release.py from the mabur checkout, so there is one
definition of it.

Pin rule: a PR may pin third_party/mabur to a mabur branch commit; master and
v* tags must pin a commit on mabur master.

  release.py pin-check [--ref REF]       exit 1 if REF's pin is off mabur master
  release.py check <tag>                 exit 1 if <tag> breaks either rule
  release.py package-meta --version V --sha S --board B --out DIR
  release.py notes --version V --sha S --build-info F [--build-info F ...] --files DIR

The mabur checkout is third_party/mabur (--mabur DIR to override); it needs
mabur's tags and origin/master fetched.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import os
import re
import subprocess
import sys

TAG_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")


def _git(repo, *args, check=True):
    return subprocess.run(["git", "-C", repo, *args], check=check,
                          capture_output=True, text=True)


def _key(tag):
    return tuple(int(x) for x in TAG_RE.match(tag).groups())


def load_mabur(mabur_dir):
    """mabur's tools/release.py, imported from the checkout."""
    path = os.path.join(mabur_dir, "tools", "release.py")
    spec = importlib.util.spec_from_file_location("mabur_release", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def pin(ref="HEAD"):
    """The mabur commit `ref` pins as third_party/mabur."""
    return _git(".", "rev-parse", f"{ref}:third_party/mabur").stdout.strip()


def pin_on_master(mabur_dir, sha, master="origin/master"):
    return _git(mabur_dir, "merge-base", "--is-ancestor", sha, master,
                check=False).returncode == 0


def _drone_tags(mabur_dir):
    tags = _git(mabur_dir, "tag", "--list", "v*").stdout.split()
    return sorted((t for t in tags if TAG_RE.match(t)), key=_key, reverse=True)


def drone_tag_for(mabur_dir, minor):
    """The newest mabur v0.<minor>.* tag, or None."""
    for t in _drone_tags(mabur_dir):
        if _key(t)[:2] == (0, minor):
            return t
    return None


def contract_in(mabur_dir, ref):
    """The contract at `ref` of the mabur repo, read by mabur's own code."""
    mod = load_mabur(mabur_dir)
    with contextlib.chdir(mabur_dir):
        return mod.contract_at(ref)


def paired_drone_tag(mabur_dir, contract):
    """The newest mabur v* tag whose contract equals `contract`, or None."""
    for t in _drone_tags(mabur_dir):
        if contract_in(mabur_dir, t) == contract:
            return t
    return None


def check_tag(tag, ground_tags, pin_sha, pin_ok, drone_tag, changes):
    """None when `tag` follows the pin and version rules, else the reason."""
    m = TAG_RE.match(tag)
    if not m:
        return f"{tag}: release tags are vMAJOR.MINOR.PATCH (e.g. v0.1.0)"
    minor = int(m.group(2))
    if not pin_ok:
        return (f"{tag} pins mabur {pin_sha[:8]}, which is not on mabur master. "
                f"Merge the mabur PR first and move the pin to its merge commit.")
    if drone_tag is None:
        return (f"{tag}: mabur has no v0.{minor}.* release, so no drone pairs with "
                f"this GS minor: tag mabur v0.{minor}.0 first.")
    if changes:
        return (f"{tag}: the contract at the pin ({pin_sha[:8]}) differs from drone "
                f"{drone_tag} ({'; '.join(changes)}). A GS v0.{minor}.* must pair "
                f"with drone v0.{minor}.*: tag the next mabur minor first, then the "
                f"GS with the same minor.")
    above = [t for t in ground_tags if TAG_RE.match(t) and _key(t) >= _key(tag)]
    if above:
        highest = max(above, key=_key)
        return f"{tag} is not above {highest}; releases only move forward."
    return None


def changelog(repo, sha, cap=50):
    """First-parent history since the previous v* tag, as "- text" lines, newest first.

    A GitHub merge commit reads as its PR title (the body's first line) plus
    the PR number; anything else reads as its subject.
    """
    prev = _git(repo, "describe", "--tags", "--abbrev=0", "--match", "v*", f"{sha}^",
                check=False)
    rng = f"{prev.stdout.strip()}..{sha}" if prev.returncode == 0 else sha
    log = _git(repo, "log", "--first-parent", "--format=%s%x00%b%x1e", rng).stdout
    lines = []
    for rec in log.split("\x1e"):
        if not rec.strip():
            continue
        subject, _, body = rec.partition("\x00")
        subject = subject.strip()
        m = re.match(r"Merge pull request #(\d+) from ", subject)
        if m:
            title = next((l.strip() for l in body.splitlines() if l.strip()), subject)
            lines.append(f"- {title} (#{m.group(1)})")
        else:
            lines.append(f"- {subject}")
    if not lines:
        return ["- No changes"]
    if len(lines) > cap:
        lines = lines[:cap] + [f"- \u2026 and {len(lines) - cap} more"]
    return lines


def version_file(version, board, drone):
    """The VERSION file shipped in an upgrade package."""
    return f"version={version}\nboard={board}\ndrone={drone or ''}\n"


def notes(version, sha, pin_sha, paired_tag, contract, infos, files, changes):
    """The release body, as markdown."""
    caps = " ".join(k.replace("CAP_", "")
                    for k in sorted(contract["caps"], key=contract["caps"].get))
    wire = (f"RC_VERSION {contract['rc_version']}, caps {caps}, "
            f"{len(contract['params'])} drone parameter rows")
    lines = [f"mabur-ground {version} (commit `{sha[:8]}`)", ""]
    if paired_tag:
        lines.append(f"**Pairs with drone {paired_tag}** (wire contract {wire}).")
    else:
        lines.append(f"**Pin contract matches no drone release yet** ({wire}).")
    lines += ["", "**Built from**", f"- mabur-ground `{sha}`", f"- mabur `{pin_sha}`"]
    for info in infos:
        lines.append(f"- {info.get('board', '?')}: devourer `{info.get('devourer', 'unknown')}`, "
                     f"Buildroot {info.get('buildroot', 'unknown')}, "
                     f"kernel `{info.get('kernel', 'unknown')}` "
                     f"(sha256 `{info.get('kernel_sha256', 'unknown')}`)")
    lines += ["", "**Changes**", *changes]
    lines += ["", "| file | md5 |", "|---|---|"]
    lines += [f"| `{name}` | `{md5}` |" for name, md5 in sorted(files.items())]
    return "\n".join(lines) + "\n"


def _read_info(path):
    info = {}
    with open(path) as f:
        for line in f:
            if "=" in line:
                k, v = line.strip().split("=", 1)
                info[k] = v
    return info


def _md5s(directory):
    out = {}
    for name in sorted(os.listdir(directory)):
        p = os.path.join(directory, name)
        if os.path.isfile(p) and not name.endswith(".txt"):
            h = hashlib.md5()
            with open(p, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
            out[name] = h.hexdigest()
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mabur", default="third_party/mabur")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pin-check")
    p.add_argument("--ref", default="HEAD")
    k = sub.add_parser("check")
    k.add_argument("tag")
    n = sub.add_parser("notes")
    n.add_argument("--version", required=True)
    n.add_argument("--sha", required=True)
    n.add_argument("--build-info", action="append", default=[])
    n.add_argument("--files", required=True)
    m = sub.add_parser("package-meta")
    m.add_argument("--version", required=True)
    m.add_argument("--sha", required=True)
    m.add_argument("--board", required=True)
    m.add_argument("--out", required=True)
    a = ap.parse_args(argv)

    if a.cmd == "pin-check":
        sha = pin(a.ref)
        if not pin_on_master(a.mabur, sha):
            print(f"error: {a.ref} pins mabur {sha[:8]}, which is not on mabur master",
                  file=sys.stderr)
            return 1
        print(f"pin {sha[:8]} is on mabur master")
        return 0
    if a.cmd == "check":
        m = TAG_RE.match(a.tag)
        sha = pin(a.tag) if m else ""
        ok = bool(m) and pin_on_master(a.mabur, sha)
        drone = drone_tag_for(a.mabur, int(m.group(2))) if m else None
        changes = []
        if m and drone:
            changes = load_mabur(a.mabur).diff(contract_in(a.mabur, drone),
                                               contract_in(a.mabur, sha))
        ground = [t for t in _git(".", "tag", "--list", "v*").stdout.split() if t != a.tag]
        err = check_tag(a.tag, ground, sha, ok, drone, changes)
        if err:
            print("error: " + err, file=sys.stderr)
            return 1
        print(f"{a.tag}: ok (pin {sha[:8]} on mabur master; contract of {drone})")
        return 0
    sha_pin = pin(a.sha)
    contract = contract_in(a.mabur, sha_pin)
    if a.cmd == "package-meta":
        drone = paired_drone_tag(a.mabur, contract)
        with open(os.path.join(a.out, "VERSION"), "w") as f:
            f.write(version_file(a.version, a.board, drone))
        with open(os.path.join(a.out, "CHANGELOG"), "w") as f:
            f.write("\n".join(changelog(".", a.sha)) + "\n")
        return 0
    sys.stdout.write(notes(a.version, a.sha, sha_pin, paired_drone_tag(a.mabur, contract),
                           contract, [_read_info(f) for f in a.build_info], _md5s(a.files),
                           changelog(".", a.sha)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
