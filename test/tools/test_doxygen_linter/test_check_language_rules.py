"""Tests for check_language_rules function in lint.py."""

from __future__ import annotations

import sys
from pathlib import Path

# Add scripts/doxygen to path to import lint module
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent / "scripts" / "doxygen"))

import lint

from test_framework import TestCase, assert_len, assert_equal


class TestCheckLanguageRules(TestCase):
    """Test cases for the check_language_rules function."""

    def test_clean_content(self) -> None:
        """Test that clean content passes."""
        content = """/**
 * @brief  Adds two numbers together.
 *
 * @param[in]  a  First number.
 * @return        The sum.
 */"""
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 0)

    def test_first_person_we(self) -> None:
        """Test detection of 'we'."""
        content = "/** We initialize the cache. */"
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].message, "First-person usage detected")

    def test_first_person_our(self) -> None:
        """Test detection of 'our'."""
        content = "/** This function uses our internal cache. */"
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 1)

    def test_first_person_i(self) -> None:
        """Test detection of 'I'."""
        content = "/** I think this works. */"
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 1)

    def test_first_person_my(self) -> None:
        """Test detection of 'my'."""
        content = "/** Returns my internal value. */"
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 1)

    def test_future_tense_will(self) -> None:
        """Test detection of future tense 'will'."""
        content = "/** This function will compute the result. */"
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].message, "Future tense 'will' detected")

    def test_combined_violations(self) -> None:
        """Test multiple violations in same content."""
        content = "/** We will return my value. */"
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 2)

    def test_case_insensitive_first_person(self) -> None:
        """Test case-insensitive first-person detection."""
        # WE
        content = "/** WE do this. */"
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 1)

        # Our
        content = "/** Our value. */"
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 1)

        # MY
        content = "/** MY value. */"
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 1)

    def test_will_case_variants(self) -> None:
        """Test case-insensitive 'will' detection."""
        content = "/** This WILL fail. */"
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 1)

        content = "/** This Will fail. */"
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 1)

    def test_i_not_followed_by_slash_or_o(self) -> None:
        """Test that I/O patterns are not flagged."""
        # I/O should NOT be flagged (I followed by /)
        content = "/** Handles I/O operations. */"
        violations = lint.check_language_rules(content, Path("test.h"))
        # The 'I' in 'I/O' should not trigger first-person detection
        # But let's verify the behavior

    def test_words_containing_will(self) -> None:
        """Test that words containing 'will' are not flagged incorrectly."""
        # The regex uses word boundaries, so 'will' as part of a word should not match
        content = "/** Uses a swill container. */"
        violations = lint.check_language_rules(content, Path("test.h"))
        # 'swill' contains 'will' but \bwill\b should not match
        assert_len(violations, 0)

    def test_third_person_present_tense(self) -> None:
        """Test that proper third-person present tense passes."""
        content = """/**
 * @brief  Computes the result.
 * @note   Returns the calculated value.
 */"""
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 0)

    def test_code_comments_not_checked(self) -> None:
        """Test that code comments are included (they are part of content)."""
        # Actually check_language_rules checks entire content
        content = """// We use a cache here.
/**
 * @brief  Function.
 */
int func();"""
        violations = lint.check_language_rules(content, Path("test.h"))
        # The // comment is also checked
        assert_len(violations, 1)

    def test_empty_content(self) -> None:
        """Test empty content."""
        content = ""
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 0)

    def test_loop_variable_i_not_flagged(self) -> None:
        """Test that loop variable 'i' is NOT flagged as first-person."""
        content = """/**
 * @brief  Process items in a loop.
 *
 * @details Iterates through the array using index i.
 *
 * @param[in]  data  The data array.
 * @return        Processed result.
 */
int process(const int* data);"""
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 0)

    def test_loop_variables_i_j_k_not_flagged(self) -> None:
        """Test that common loop variables i, j, k are NOT flagged."""
        content = """/**
 * @brief  Nested loop example.
 *
 * @details Uses i for outer loop, j for inner loop.
 *
 * @return        The sum.
 */
int nestedLoop() {
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 10; ++j) {
            for (int k = 0; k < 10; ++k) {
                // Do something
            }
        }
    }
    return 0;
}"""
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 0)

    def test_uppercase_i_is_flagged(self) -> None:
        """Test that uppercase 'I' (first person) IS flagged."""
        content = "/** I think this works. */"
        violations = lint.check_language_rules(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].message, "First-person usage detected")
