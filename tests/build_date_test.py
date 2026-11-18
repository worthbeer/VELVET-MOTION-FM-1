#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The ABOUT page's build date (tools/build.py build_date): fixed per source, in __DATE__'s format.

Run from the repo root: python3 tests/build_date_test.py
"""
import os
import re
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build as B  # noqa: E402


class BuildDateTest(unittest.TestCase):
    def test_format_matches_compiler_date(self):
        # 2026-11-16 20:32:54 UTC (the CI run of the first VELVET MOTION build) and a single-digit day, space-padded
        self.assertEqual(B.format_build_date(1794861174), "Nov 16 2026")
        self.assertEqual(B.format_build_date(1793750400), "Nov  4 2026")
        self.assertEqual(len(B.format_build_date(1794861174)), len("Nov 16 2026"))
        self.assertRegex(B.format_build_date(0), r"^[A-Z][a-z]{2} [ 0-9]\d \d{4}$")

    def test_every_month_is_english_and_distinct(self):
        names = {B.format_build_date(1767225600 + m * 31 * 86400)[:3] for m in range(12)}
        self.assertTrue(names <= set(B.MONTHS))
        self.assertGreaterEqual(len(names), 11)       # 31-day steps from 1 Jan can skip February, never more

    def test_source_date_epoch_wins(self):
        with mock.patch.dict(os.environ, {"SOURCE_DATE_EPOCH": "1794861174"}):
            self.assertEqual(B.build_date(), "Nov 16 2026")

    def test_same_input_same_date(self):
        with mock.patch.dict(os.environ, {"SOURCE_DATE_EPOCH": "1793750400"}):
            self.assertEqual(B.build_date(), B.build_date())

    def test_commit_date_when_no_variable(self):
        env = {k: v for k, v in os.environ.items() if k != "SOURCE_DATE_EPOCH"}
        with mock.patch.dict(os.environ, env, clear=True):
            d = B.build_date()
        # in a git checkout: a date; in a tree without git: None (the firmware then uses __DATE__)
        self.assertTrue(d is None or re.fullmatch(r"[A-Z][a-z]{2} [ 0-9]\d \d{4}", d))

    def test_no_git_and_no_variable_is_none(self):
        env = {k: v for k, v in os.environ.items() if k != "SOURCE_DATE_EPOCH"}
        with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(B.subprocess, "run", side_effect=OSError):
            self.assertIsNone(B.build_date())

    def test_bad_variable_is_none(self):
        with mock.patch.dict(os.environ, {"SOURCE_DATE_EPOCH": "not-a-number"}):
            self.assertIsNone(B.build_date())


if __name__ == "__main__":
    unittest.main()
