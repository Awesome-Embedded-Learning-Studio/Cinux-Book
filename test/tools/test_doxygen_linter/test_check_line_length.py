"""Tests for check_line_length function in lint.py."""

from __future__ import annotations

import sys
from pathlib import Path

# Add scripts/doxygen to path to import lint module
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent / "scripts" / "doxygen"))

import lint

from test_framework import TestCase, assert_equal, assert_len


class TestCheckLineLength(TestCase):
    """Test cases for the check_line_length function."""

    def test_all_lines_within_limit(self) -> None:
        """Test that no violations are reported when all lines are within limit."""
        content = "Line 1\nLine 2\nLine 3"
        violations = lint.check_line_length(content, Path("test.h"))
        assert_len(violations, 0)

    def test_exactly_max_length(self) -> None:
        """Test that lines exactly at max length are allowed."""
        content = "x" * lint.MAX_LINE_LENGTH
        violations = lint.check_line_length(content, Path("test.h"))
        assert_len(violations, 0)

    def test_one_character_over_limit(self) -> None:
        """Test that lines one char over limit are flagged."""
        content = "x" * (lint.MAX_LINE_LENGTH + 1)
        violations = lint.check_line_length(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].symbol, None)

    def test_multiple_long_lines(self) -> None:
        """Test that multiple violations are reported."""
        content = f"{'x' * (lint.MAX_LINE_LENGTH + 1)}\nNormal line\n{'y' * (lint.MAX_LINE_LENGTH + 5)}"
        violations = lint.check_line_length(content, Path("test.h"))
        assert_len(violations, 2)

    def test_mixed_line_lengths(self) -> None:
        """Test mixed content with some long and some short lines."""
        content = (
            "Short line\n"
            + "x" * (lint.MAX_LINE_LENGTH + 10) + "\n"
            + "Another short line\n"
            + "y" * (lint.MAX_LINE_LENGTH + 1) + "\n"
            + "Last short line"
        )
        violations = lint.check_line_length(content, Path("test.h"))
        assert_len(violations, 2)

    def test_line_numbers_in_message(self) -> None:
        """Test that violation messages include correct line numbers."""
        content = "Short\n" + "x" * (lint.MAX_LINE_LENGTH + 1) + "\nShort"
        violations = lint.check_line_length(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].message, "Line 2 exceeds 100 characters")

    def test_empty_file(self) -> None:
        """Test that empty files produce no violations."""
        content = ""
        violations = lint.check_line_length(content, Path("test.h"))
        assert_len(violations, 0)

    def test_file_with_only_newlines(self) -> None:
        """Test file with only newline characters."""
        content = "\n\n\n"
        violations = lint.check_line_length(content, Path("test.h"))
        assert_len(violations, 0)

    def test_very_long_line(self) -> None:
        """Test extremely long line."""
        content = "x" * 1000
        violations = lint.check_line_length(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].message, "Line 1 exceeds 100 characters")
