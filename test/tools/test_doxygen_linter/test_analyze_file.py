"""Integration tests for analyze_file function."""

from __future__ import annotations

import sys
from pathlib import Path

# Add scripts/doxygen to path to import lint module
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent / "scripts" / "doxygen"))

import lint

from test_framework import TestCase, assert_len, assert_equal, assert_true, assert_not_in


class TestAnalyzeFile(TestCase):
    """Integration tests for analyze_file function."""

    def test_valid_fixture_file(self) -> None:
        """Test that valid_file.h passes all checks."""
        fixture_path = Path(__file__).parent / "fixtures" / "valid_file.h"
        violations = lint.analyze_file(fixture_path)
        assert_len(violations, 0)

    def test_no_file_header_fixture(self) -> None:
        """Test that no_file_header.h is detected."""
        fixture_path = Path(__file__).parent / "fixtures" / "no_file_header.h"
        violations = lint.analyze_file(fixture_path)
        assert_len(violations, 1)
        assert_equal(violations[0].message, "Missing file-level @file header")

    def test_long_lines_fixture(self) -> None:
        """Test that long_lines.h detects line length violations."""
        fixture_path = Path(__file__).parent / "fixtures" / "long_lines.h"
        violations = lint.analyze_file(fixture_path)
        assert_true(len(violations) > 3)

    def test_missing_brief_fixture(self) -> None:
        """Test that missing_brief.h detects missing @brief."""
        fixture_path = Path(__file__).parent / "fixtures" / "missing_brief.h"
        violations = lint.analyze_file(fixture_path)
        # no_brief_tag and no_doxygen should be detected
        assert_true(len(violations) >= 1)

    def test_missing_param_direction_fixture(self) -> None:
        """Test that missing_param_direction.h detects missing directions."""
        fixture_path = Path(__file__).parent / "fixtures" / "missing_param_direction.h"
        violations = lint.analyze_file(fixture_path)
        # add() function has 2 params without direction
        assert_true(len(violations) >= 2)

    def test_missing_return_fixture(self) -> None:
        """Test that missing_return.h detects return issues."""
        fixture_path = Path(__file__).parent / "fixtures" / "missing_return.h"
        violations = lint.analyze_file(fixture_path)
        # no_return: missing @return
        # void_with_return: void with @return
        assert_true(len(violations) >= 1)

    def test_undocumented_types_fixture(self) -> None:
        """Test that undocumented_types.h detects undocumented types."""
        fixture_path = Path(__file__).parent / "fixtures" / "undocumented_types.h"
        violations = lint.analyze_file(fixture_path)
        # Should find UndocumentedEnum, UndocumentedStruct, UndocumentedClass, etc.
        assert_true(len(violations) >= 3)

    def test_private_members_fixture(self) -> None:
        """Test that private_members.h skips private members."""
        fixture_path = Path(__file__).parent / "fixtures" / "private_members.h"
        violations = lint.analyze_file(fixture_path)
        # PublicStruct and ProtectedEnum should have violations
        # PrivateStruct and private functions should be skipped
        assert_true(len(violations) >= 1)
        # Check that private symbols are not in violations
        symbols = [v.symbol for v in violations if v.symbol]
        assert_not_in("PrivateStruct", symbols)
        assert_not_in("PrivateFunc", symbols)

    def test_language_issues_fixture(self) -> None:
        """Test that language_issues.h detects language problems."""
        fixture_path = Path(__file__).parent / "fixtures" / "language_issues.h"
        violations = lint.analyze_file(fixture_path)
        # Should have language violations for badLanguage()
        assert_true(len(violations) > 0)
        # Check for language violation messages
        messages = [v.message for v in violations]
        # Should contain first-person or future tense violation
        has_first_person = any("First-person" in m for m in messages)
        has_future_tense = any("will" in m for m in messages)
        assert_true(has_first_person or has_future_tense)

    def test_complex_functions_fixture(self) -> None:
        """Test that complex_functions.h handles complex patterns."""
        fixture_path = Path(__file__).parent / "fixtures" / "complex_functions.h"
        violations = lint.analyze_file(fixture_path)
        # Undocumented functions should be detected
        assert_true(len(violations) > 0)
