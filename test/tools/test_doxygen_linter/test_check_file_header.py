"""Tests for check_file_header function in lint.py."""

from __future__ import annotations

import sys
from pathlib import Path

# Add scripts/doxygen to path to import lint module
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent / "scripts" / "doxygen"))

import lint

from test_framework import TestCase, assert_len, assert_equal


class TestCheckFileHeader(TestCase):
    """Test cases for the check_file_header function."""

    def test_valid_file_header(self) -> None:
        """Test that valid file header passes."""
        content = """/**
 * @file    test.h
 * @brief   Test file.
 */
"""
        violations = lint.check_file_header(content, Path("test.h"))
        assert_len(violations, 0)

    def test_missing_file_header(self) -> None:
        """Test that missing file header is detected."""
        content = "// No file header here\nint x;"
        violations = lint.check_file_header(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].message, "Missing file-level @file header")

    def test_file_header_without_at_file(self) -> None:
        """Test that header without @file tag is detected."""
        content = """/**
 * @brief   Test file.
 */
"""
        violations = lint.check_file_header(content, Path("test.h"))
        assert_len(violations, 1)

    def test_multiline_file_header(self) -> None:
        """Test that multiline file header is recognized."""
        content = """/**
 * @file    test.h
 * @brief   This is a longer description
 *         that spans multiple lines.
 */
"""
        violations = lint.check_file_header(content, Path("test.h"))
        assert_len(violations, 0)

    def test_file_header_with_additional_tags(self) -> None:
        """Test file header with many tags."""
        content = """/**
 * @file    test.h
 * @brief   Test file.
 * @author  Test Author
 * @date    2024-01-01
 * @version 1.0
 * @since   1.0
 * @ingroup test
 */
"""
        violations = lint.check_file_header(content, Path("test.h"))
        assert_len(violations, 0)

    def test_file_header_with_code_before(self) -> None:
        """Test that file header is detected even with code/comments before."""
        content = """// License header
/**
 * @file    test.h
 * @brief   Test file.
 */
"""
        violations = lint.check_file_header(content, Path("test.h"))
        assert_len(violations, 0)

    def test_empty_file(self) -> None:
        """Test that empty file is flagged."""
        content = ""
        violations = lint.check_file_header(content, Path("test.h"))
        assert_len(violations, 1)

    def test_inline_comment_with_file(self) -> None:
        """Test that inline @file doesn't count."""
        content = "// @file test.h\nint x;"
        violations = lint.check_file_header(content, Path("test.h"))
        assert_len(violations, 1)

    def test_file_header_different_spacing(self) -> None:
        """Test file header with various spacing."""
        # With extra spaces
        content = "/**\n * @file    test.h\n * @brief   Test.\n*/"
        violations = lint.check_file_header(content, Path("test.h"))
        assert_len(violations, 0)

        # Minimal spacing
        content = "/**@file test.h*/"
        violations = lint.check_file_header(content, Path("test.h"))
        assert_len(violations, 0)
