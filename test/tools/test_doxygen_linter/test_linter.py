#!/usr/bin/env python3
"""
Doxygen Linter Test Runner

Main entry point for running all Doxygen linter tests.

Usage:
    python test_linter.py              # Run all tests
    python test_linter.py -v           # Run with verbose output
    python test_linter.py <suite>      # Run specific test suite

Example:
    python test_linter.py TestShouldSkip
"""

from __future__ import annotations

import sys
from pathlib import Path

# Add current directory to path for imports
sys.path.insert(0, str(Path(__file__).parent))

from test_framework import TestSuite, run_all_tests, run_test_suite

# Import all test cases
from test_should_skip import TestShouldSkip
from test_check_line_length import TestCheckLineLength
from test_check_file_header import TestCheckFileHeader
from test_check_enum_struct_docs import TestCheckEnumStructDocs
from test_check_function_blocks import TestCheckFunctionBlocks
from test_check_language_rules import TestCheckLanguageRules
from test_private_detection import TestIsPrivateType, TestIsPrivateFunction
from test_member_variables import TestIsMemberVariable
from test_analyze_file import TestAnalyzeFile
from test_code_block_detection import TestIsInCodeBlock, TestCodeBlockInFunctionDetection, TestCodeBlockInTypeDetection
from test_constexpr_constructor import TestConstexprConstructor
from test_lint_files import TestLintFiles


def create_all_suites() -> list[TestSuite]:
    """Create all test suites."""
    suites = []

    # Suite for file filtering tests
    suite = TestSuite("File Filtering Tests")
    suite.add_test(TestShouldSkip())
    suites.append(suite)

    # Suite for incremental lint_files tests
    suite = TestSuite("Incremental Lint Tests")
    suite.add_test(TestLintFiles())
    suites.append(suite)

    # Suite for line length checks
    suite = TestSuite("Line Length Check Tests")
    suite.add_test(TestCheckLineLength())
    suites.append(suite)

    # Suite for file header checks
    suite = TestSuite("File Header Check Tests")
    suite.add_test(TestCheckFileHeader())
    suites.append(suite)

    # Suite for enum/struct/class documentation checks
    suite = TestSuite("Type Documentation Tests")
    suite.add_test(TestCheckEnumStructDocs())
    suites.append(suite)

    # Suite for function block checks
    suite = TestSuite("Function Block Tests")
    suite.add_test(TestCheckFunctionBlocks())
    suites.append(suite)

    # Suite for language rule checks
    suite = TestSuite("Language Rule Tests")
    suite.add_test(TestCheckLanguageRules())
    suites.append(suite)

    # Suite for private detection tests
    suite = TestSuite("Private Detection Tests")
    suite.add_test(TestIsPrivateType())
    suite.add_test(TestIsPrivateFunction())
    suites.append(suite)

    # Suite for member variable detection tests
    suite = TestSuite("Member Variable Tests")
    suite.add_test(TestIsMemberVariable())
    suites.append(suite)

    # Suite for integration tests
    suite = TestSuite("Integration Tests")
    suite.add_test(TestAnalyzeFile())
    suites.append(suite)

    # Suite for @code block detection tests
    suite = TestSuite("@code Block Detection Tests")
    suite.add_test(TestIsInCodeBlock())
    suite.add_test(TestCodeBlockInFunctionDetection())
    suite.add_test(TestCodeBlockInTypeDetection())
    suites.append(suite)

    # Suite for constexpr constructor tests
    suite = TestSuite("Constexpr Constructor Tests")
    suite.add_test(TestConstexprConstructor())
    suites.append(suite)

    return suites


def main() -> int:
    """Main entry point."""
    args = sys.argv[1:]
    verbose = "-v" in args or "--verbose" in args

    # Check if a specific test class is requested
    specific = None
    for arg in args:
        if not arg.startswith("-"):
            specific = arg
            break

    if specific:
        # Run specific test suite
        suite_map = {
            "TestShouldSkip": TestShouldSkip,
            "TestCheckLineLength": TestCheckLineLength,
            "TestCheckFileHeader": TestCheckFileHeader,
            "TestCheckEnumStructDocs": TestCheckEnumStructDocs,
            "TestCheckFunctionBlocks": TestCheckFunctionBlocks,
            "TestCheckLanguageRules": TestCheckLanguageRules,
            "TestIsPrivateType": TestIsPrivateType,
            "TestIsPrivateFunction": TestIsPrivateFunction,
            "TestIsMemberVariable": TestIsMemberVariable,
            "TestAnalyzeFile": TestAnalyzeFile,
        }

        # Check for @code block detection tests (multiple classes in one suite)
        code_block_tests = {
            "TestIsInCodeBlock": TestIsInCodeBlock,
            "TestCodeBlockInFunctionDetection": TestCodeBlockInFunctionDetection,
            "TestCodeBlockInTypeDetection": TestCodeBlockInTypeDetection,
        }

        if specific in suite_map:
            suite = TestSuite(f"{specific} Suite")
            suite.add_test(suite_map[specific]())
        elif specific in code_block_tests:
            suite = TestSuite(f"{specific} Suite")
            suite.add_test(code_block_tests[specific]())
            result = run_test_suite(suite, verbose)
            return 0 if result.failed == 0 else 1
        else:
            print(f"Unknown test suite: {specific}")
            print(f"Available: {', '.join(suite_map.keys())}")
            return 1

    # Run all suites
    suites = create_all_suites()
    results = run_all_tests(suites, verbose)

    # Return non-zero if any tests failed
    total_failed = sum(r.failed for r in results)
    return 1 if total_failed > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
