"""Tests for should_skip function in lint.py."""

from __future__ import annotations

import sys
from pathlib import Path

# Add scripts/doxygen to path to import lint module
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent / "scripts" / "doxygen"))

import lint

from test_framework import TestCase, assert_equal, assert_true, assert_false


class TestShouldSkip(TestCase):
    """Test cases for the should_skip function."""

    def test_skips_non_header_files(self) -> None:
        """Test that non-header files are skipped."""
        assert_true(lint.should_skip(Path("test.cpp")))
        assert_true(lint.should_skip(Path("test.c")))
        assert_true(lint.should_skip(Path("test.txt")))
        assert_true(lint.should_skip(Path("test.md")))

    def test_scans_header_files(self) -> None:
        """Test that header files are scanned."""
        assert_false(lint.should_skip(Path("test.h")))
        assert_false(lint.should_skip(Path("test.hpp")))

    def test_skips_excluded_directories(self) -> None:
        """Test that files in excluded directories are skipped."""
        assert_true(lint.should_skip(Path("out/test.h")))
        assert_true(lint.should_skip(Path(".vscode/test.h")))
        assert_true(lint.should_skip(Path("cmake/test.h")))
        assert_true(lint.should_skip(Path("example/test.h")))
        assert_true(lint.should_skip(Path("scripts/test.h")))
        assert_true(lint.should_skip(Path(".git/test.h")))
        assert_true(lint.should_skip(Path("test/test.h")))
        assert_true(lint.should_skip(Path("third_party/test.h")))

    def test_scans_allowed_directories(self) -> None:
        """Test that files in allowed directories are scanned."""
        assert_false(lint.should_skip(Path("src/test.h")))
        assert_false(lint.should_skip(Path("include/test.h")))
        assert_false(lint.should_skip(Path("ui/test.h")))
        assert_false(lint.should_skip(Path("core/test.h")))

    def test_skips_excluded_extensions(self) -> None:
        """Test that explicitly excluded extensions are skipped."""
        # Currently EXCLUDED_EXTENSIONS is empty, but test the logic
        # Modify EXCLUDED_EXTENSIONS for testing
        original = lint.EXCLUDED_EXTENSIONS
        # Use .gen as extension (path.suffix returns the last extension)
        lint.EXCLUDED_EXTENSIONS = (".gen",)
        try:
            assert_true(lint.should_skip(Path("test.gen")))
        finally:
            lint.EXCLUDED_EXTENSIONS = original

    def test_complex_paths(self) -> None:
        """Test complex nested paths."""
        # Should skip - test directory in path
        assert_true(lint.should_skip(Path("src/subdir/test/test.h")))

        # Should scan
        assert_false(lint.should_skip(Path("src/subdir/include/test.h")))

    def test_absolute_paths(self) -> None:
        """Test absolute path handling."""
        # These should still work with absolute paths
        assert_true(lint.should_skip(Path("/absolute/path/out/test.h")))
        assert_false(lint.should_skip(Path("/absolute/path/src/test.h")))
