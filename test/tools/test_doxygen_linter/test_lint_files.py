"""Tests for lint_files incremental mode in lint.py."""

from __future__ import annotations

import sys
from pathlib import Path

# Add scripts/doxygen to path to import lint module
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent / "scripts" / "doxygen"))

import lint

from test_framework import TestCase, assert_equal, assert_true

FIXTURES = Path(__file__).parent / "fixtures"


class TestLintFiles(TestCase):
    """Test cases for the lint_files function."""

    def test_valid_fixture_passes(self) -> None:
        """Test that a valid header produces no violations."""
        assert_equal(lint.lint_files([str(FIXTURES / "valid_file.h")]), [])

    def test_reports_violations_for_given_file(self) -> None:
        """Test that violations are reported for an explicitly given file."""
        assert_true(len(lint.lint_files([str(FIXTURES / "missing_brief.h")])) > 0)

    def test_skips_non_header_entries(self) -> None:
        """Test that non-header entries in the list are skipped."""
        assert_equal(lint.lint_files(["README.md", "src/main.cpp"]), [])

    def test_mix_returns_only_header_violations(self) -> None:
        """Test that mixing headers and non-headers reports only header violations."""
        only_header = lint.lint_files([str(FIXTURES / "missing_brief.h")])
        mixed = lint.lint_files([str(FIXTURES / "missing_brief.h"), "README.md"])
        assert_equal(len(mixed), len(only_header))
