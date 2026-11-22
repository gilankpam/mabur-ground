#!/usr/bin/env python3
"""tools/release.py: the pin rule and the GS version rule.

check_tag is pure. The git-backed helpers run against a throwaway mabur
repo built here, carrying the build's mabur's own tools/release.py so the
contract is read by mabur's code, exactly as on a real check.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
# The mabur checkout the build uses (ctest passes CMake's MABUR_DIR): mabur's
# CI builds this repo against its own tree with the submodule left empty.
MABUR = os.environ.get("MABUR_DIR") or os.path.join(ROOT, "third_party", "mabur")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import release  # noqa: E402

PIN = "a" * 40

RC_PROTO = """
constexpr uint8_t RC_VERSION = {rc};
constexpr uint16_t CAP_FRAME_WIRE = 0x0001;
"""
PARAMS = """
static const ParamRow kTable[] = {
    {"RADIO_LDPC", ParamType::kBool, "radio", "ldpc"},
};
"""


def git(repo, *args):
    return subprocess.run(["git", "-C", repo, *args], check=True,
                          capture_output=True, text=True).stdout.strip()


def commit_contract(repo, rc, msg):
    inc = os.path.join(repo, "common", "include", "mabur")
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, "rc_proto.h"), "w") as f:
        f.write(RC_PROTO.format(rc=rc))
    with open(os.path.join(inc, "config_params.h"), "w") as f:
        f.write(PARAMS)
    with open(os.path.join(repo, "log.txt"), "a") as f:
        f.write(msg + "\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", msg)
    return git(repo, "rev-parse", "HEAD")


class CheckTag(unittest.TestCase):
    def test_tag_format(self):
        self.assertIn("vMAJOR.MINOR.PATCH",
                      release.check_tag("0.1.0", [], PIN, True, "v0.1.0", []))

    def test_pin_off_master_names_the_pin(self):
        err = release.check_tag("v0.1.0", [], PIN, False, "v0.1.0", [])
        self.assertIn("not on mabur master", err)
        self.assertIn("aaaaaaaa", err)

    def test_minor_without_a_drone_release(self):
        self.assertIn("tag mabur v0.2.0 first",
                      release.check_tag("v0.2.0", [], PIN, True, None, []))

    def test_contract_differs_from_the_drone_release(self):
        err = release.check_tag("v0.1.1", ["v0.1.0"], PIN, True, "v0.1.0",
                                ["RC_VERSION 18 -> 19"])
        self.assertIn("RC_VERSION 18 -> 19", err)
        self.assertIn("v0.1.0", err)

    def test_tag_not_above_existing(self):
        self.assertIn("not above v0.1.1",
                      release.check_tag("v0.1.1", ["v0.1.0", "v0.1.1"], PIN, True,
                                        "v0.1.0", []))
        self.assertIn("not above v0.1.10",
                      release.check_tag("v0.1.9", ["v0.1.10"], PIN, True, "v0.1.0", []))

    def test_ok(self):
        self.assertIsNone(release.check_tag("v0.1.0", [], PIN, True, "v0.1.0", []))
        self.assertIsNone(release.check_tag("v0.2.0", ["v0.1.3"], PIN, True, "v0.2.1", []))


class MaburRepo(unittest.TestCase):
    """c1 (RC 18, v0.1.0) -- c3 (RC 18, v0.1.1) on master; c2 (RC 19) on a side branch."""

    @classmethod
    def setUpClass(cls):
        cls.repo = tempfile.mkdtemp()
        git(cls.repo, "init", "-q", "-b", "master")
        git(cls.repo, "config", "user.email", "t@example.com")
        git(cls.repo, "config", "user.name", "t")
        os.makedirs(os.path.join(cls.repo, "tools"))
        shutil.copy(os.path.join(MABUR, "tools", "release.py"),
                    os.path.join(cls.repo, "tools", "release.py"))
        cls.c1 = commit_contract(cls.repo, 18, "c1")
        git(cls.repo, "tag", "v0.1.0")
        git(cls.repo, "switch", "-q", "-c", "side")
        cls.c2 = commit_contract(cls.repo, 19, "c2")
        git(cls.repo, "switch", "-q", "master")
        cls.c3 = commit_contract(cls.repo, 18, "c3")
        git(cls.repo, "tag", "v0.1.1")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.repo)

    def test_pin_on_master(self):
        self.assertTrue(release.pin_on_master(self.repo, self.c1, "master"))
        self.assertFalse(release.pin_on_master(self.repo, self.c2, "master"))

    def test_drone_tag_for(self):
        self.assertEqual(release.drone_tag_for(self.repo, 1), "v0.1.1")
        self.assertIsNone(release.drone_tag_for(self.repo, 2))

    def test_contract_in(self):
        self.assertEqual(release.contract_in(self.repo, self.c2)["rc_version"], 19)
        self.assertEqual(release.contract_in(self.repo, "v0.1.0")["rc_version"], 18)

    def test_paired_drone_tag(self):
        self.assertEqual(release.paired_drone_tag(self.repo, release.contract_in(self.repo, self.c1)),
                         "v0.1.1")
        self.assertIsNone(release.paired_drone_tag(self.repo, release.contract_in(self.repo, self.c2)))

    def test_contract_change_reaches_check_tag(self):
        m = release.load_mabur(self.repo)
        changes = m.diff(release.contract_in(self.repo, "v0.1.1"),
                         release.contract_in(self.repo, self.c2))
        err = release.check_tag("v0.1.2", ["v0.1.1"], self.c2, False, "v0.1.1", changes)
        self.assertIn("not on mabur master", err)
        err = release.check_tag("v0.1.2", ["v0.1.1"], self.c2, True, "v0.1.1", changes)
        self.assertIn("RC_VERSION 18 -> 19", err)


class Notes(unittest.TestCase):
    CONTRACT = {"rc_version": 18, "caps": {"CAP_FRAME_WIRE": 1}, "params": ["RADIO_LDPC"]}

    def test_paired(self):
        body = release.notes("v0.1.0", "b" * 40, PIN, "v0.1.0", self.CONTRACT,
                             [{"board": "radxa_zero3", "devourer": "d" * 40,
                               "buildroot": "2025.08.1", "kernel": "k.tar.gz",
                               "kernel_sha256": "e" * 64}],
                             {"x.img": "f" * 32}, ["- fix: x"])
        self.assertIn("mabur-ground v0.1.0 (commit `bbbbbbbb`)", body)
        self.assertIn("Pairs with drone v0.1.0", body)
        self.assertIn("RC_VERSION 18", body)
        self.assertIn("radxa_zero3", body)
        self.assertIn("`x.img`", body)

    def test_notes_changes_section(self):
        body = release.notes("v0.1.0", "b" * 40, PIN, "v0.1.0", self.CONTRACT,
                             [{"board": "radxa_zero3"}], {"x.img": "f" * 32}, ["- fix: x"])
        self.assertIn("\n**Changes**\n- fix: x\n", body)
        self.assertLess(body.index("radxa_zero3"), body.index("**Changes**"))
        self.assertLess(body.index("**Changes**"), body.index("| file | md5 |"))

    def test_unpaired(self):
        body = release.notes("snapshot-bbbbbbbb", "b" * 40, PIN, None, self.CONTRACT, [], {}, [])
        self.assertIn("matches no drone release yet", body)


class Changelog(unittest.TestCase):
    def setUp(self):
        self.repo = tempfile.mkdtemp()
        git(self.repo, "init", "-q", "-b", "master")
        git(self.repo, "config", "user.email", "t@example.com")
        git(self.repo, "config", "user.name", "t")
        self.n = 0

    def tearDown(self):
        shutil.rmtree(self.repo)

    def commit(self, msg):
        self.n += 1
        git(self.repo, "commit", "-q", "--allow-empty", "-m", msg)

    def head(self):
        return git(self.repo, "rev-parse", "HEAD")

    def test_changelog_since_previous_tag(self):
        self.commit("old")
        git(self.repo, "tag", "v0.1.0")
        self.commit("fix: x")
        git(self.repo, "switch", "-q", "-c", "feat")
        self.commit("inner")
        git(self.repo, "switch", "-q", "master")
        git(self.repo, "merge", "-q", "--no-ff", "feat", "-m",
            "Merge pull request #5 from a/b", "-m", "Wire the buttons")
        self.assertEqual(release.changelog(self.repo, self.head()),
                         ["- Wire the buttons (#5)", "- fix: x"])

    def test_changelog_excludes_own_tag(self):
        self.commit("old")
        git(self.repo, "tag", "v0.1.0")
        self.commit("a")
        self.commit("b")
        git(self.repo, "tag", "v0.2.0")
        self.assertEqual(release.changelog(self.repo, self.head()), ["- b", "- a"])

    def test_changelog_empty(self):
        # No real history yields an empty range (the walk starts at sha^), so
        # stub git to return one.
        class R:
            returncode, stdout = 0, ""
        orig = release._git
        release._git = lambda *a, **k: R()
        try:
            self.assertEqual(release.changelog(self.repo, "HEAD"), ["- No changes"])
        finally:
            release._git = orig

    def test_changelog_cap(self):
        for i in range(60):
            self.commit(f"c{i}")
        out = release.changelog(self.repo, self.head())
        self.assertEqual(len(out), 51)
        self.assertEqual(out[-1], "- \u2026 and 10 more")


class VersionFile(unittest.TestCase):
    def test_version_file(self):
        self.assertEqual(release.version_file("v0.2.0", "radxa_zero3", "v0.2.0"),
                         "version=v0.2.0\nboard=radxa_zero3\ndrone=v0.2.0\n")
        self.assertIn("\ndrone=\n", release.version_file("v0.2.0", "radxa_zero3", None))


if __name__ == "__main__":
    unittest.main()
