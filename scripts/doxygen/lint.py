#!/usr/bin/env python3
"""
Strict Doxygen Linter for Modern C++ Projects

- Scans directory recursively
- Applies Doxygen compliance checks
- Writes failures to FAILED_DOXYGEN.md in project root
- Returns non-zero exit code if violations found
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Tuple, Iterable

# ============================================================
# ===================== CONFIGURATION ========================
# ============================================================


def find_project_root() -> Path:
    """Find the project root by searching for .git directory or markers.

    Starts from the script location and searches upward.

    Returns:
        Path to the project root directory.
    """
    # Start from the script's directory
    script_path = Path(__file__).resolve().parent

    # Known project root markers (in order of priority)
    markers = [".git", "CMakeLists.txt", ".project-root"]

    # Search upward from script directory
    current = script_path
    while current != current.parent:  # Stop at filesystem root
        # Check if any marker exists
        if any((current / marker).exists() for marker in markers):
            return current

        # Also check if we're in a known project structure
        # (scripts/doxygen/lint.py should be in project root)
        if current.name == "scripts":
            parent = current.parent
            # Check if this looks like a project root
            if any((parent / marker).exists() for marker in markers):
                return parent

        current = current.parent

    # Fallback: use current working directory
    return Path.cwd()


PROJECT_ROOT: Path = find_project_root()

# Directories to ignore
EXCLUDED_DIRS: Tuple[str, ...] = (
    "out",
    ".vscode",
    "cmake",
    "example",
    "scripts",
    ".git",
    "test",
    "third_party",
    ".notes",
    "node_modules",
    "site",
    "document",
    "assets",
    "build"
)

# File extensions to scan
SCAN_EXTENSIONS: Tuple[str, ...] = (
    ".h",
    ".hpp",
)

# File extensions to exclude explicitly
EXCLUDED_EXTENSIONS: Tuple[str, ...] = ()

MAX_LINE_LENGTH: int = 100

FAILED_OUTPUT_FILE: Path = PROJECT_ROOT / "FAILED_DOXYGEN.md"

# ============================================================
# ====================== REGEX RULES =========================
# ============================================================

# Note: 'I' must be case-sensitive to avoid matching loop variable 'i'
# we/our/my are case-insensitive to catch variants like WE, Our, MY, etc.
FUTURE_TENSE_PATTERN = re.compile(r"\bwill\b", re.IGNORECASE)

# Compile a case-sensitive pattern for 'I' to avoid matching loop variable 'i'
# This pattern matches 'I' (uppercase only) as a whole word, except when followed by /, o, or O
FIRST_PERSON_I_PATTERN = re.compile(r"\bI\b(?![/oO])")

# Pattern for we/our/my (case-insensitive)
FIRST_PERSON_WE_PATTERN = re.compile(r"\b(?:we|our|my)\b", re.IGNORECASE)

FUNCTION_PATTERN = re.compile(
    r'^\s*(?:template\s*<[^>]*>\s*)?(?:virtual\s+)?(?:inline\s+)?(?:explicit\s+)?(?:static\s+)?(?:constexpr\s+)?(?:consteval\s+)?(?:constinit\s+)?(?:friend\s+)?([\w:<>&\s]+?)(?:\s+const)?\s+(?!if|for|while|switch|return|static_assert|catch|union|struct|class)(\w+)\s*\(([^)]*)\)(?:\s*(?:const|volatile|noexcept(?:\([^)]*\))?|override|final|consteval|constinit)\s*)*(?:\s*=\s*0)?\s*[;{]',
    re.MULTILINE,
)

# Keywords that are not functions
NON_FUNCTION_KEYWORDS = {
    'if', 'for', 'while', 'switch', 'return', 'static_assert',
    'catch', 'sizeof', 'decltype', 'typeid', 'alignof', 'alignas',
    'typename', 'namespace', 'using', 'typedef', 'struct', 'class',
    'enum', 'union', 'template', 'extern', 'thread_local', 'mutable',
    'volatile', 'const', 'constexpr', 'consteval', 'constinit',
    'concept', 'requires', 'co_await', 'co_yield', 'co_return',
    'public', 'private', 'protected', 'friend', 'virtual', 'inline',
    'explicit', 'static', 'register', 'auto', 'signed', 'unsigned',
    'true', 'false', 'nullptr', 'and', 'or', 'not', 'xor', 'bitand',
    'bitor', 'compl', 'not_eq', 'and_eq', 'or_eq', 'xor_eq',
    # Add common type traits that might be confused with function names
    'enable_if', 'type_identity', 'remove_reference', 'add_const',
    'remove_const', 'remove_cv', 'remove_cvref', 'is_same', 'is_void',
}

DOXYGEN_BLOCK_PATTERN = re.compile(r"/\*\*.*?\*/", re.DOTALL)
# Match enum/struct/class with optional export macros and other attributes
# This pattern matches the keyword and skips to the actual type name
ENUM_STRUCT_PATTERN = re.compile(
    r'\b(enum|struct|class|union)\s+'
    r'(?:CF_UI_EXPORT|Q_DECL_EXPORT|Q_DECL_IMPORT|DLL_EXPORT|DLL_IMPORT|EXPORT|IMPORT|'
    r'(?:__declspec\s*\(\s*dllexport\s*\)|__declspec\s*\(\s*dllimport\s*\)))?\s*'
    r'(?:class\s+)?'  # Optional 'class' keyword after enum/struct
    r'(\w+)'  # The actual type name
    r'\s*[{:]'  # Must be followed by { (for definition) or : (for inheritance)
)

# Core required tags - must be present
CORE_REQUIRED_TAGS = (
    "@brief",
)

# Conditional tags - only check if they make sense for the function
OPTIONAL_TAGS = (
    "@throws",
    "@note",
    "@warning",
    "@since",
    "@ingroup",
)

# ============================================================
# ======================= DATA TYPES =========================
# ============================================================


@dataclass
class Violation:
    file: Path
    message: str
    symbol: str | None = None

    def to_markdown(self) -> str:
        if self.symbol:
            return f"- **{self.file}** :: `{self.symbol}` → {self.message}"
        return f"- **{self.file}** → {self.message}"


# ============================================================
# ======================= CORE LOGIC =========================
# ============================================================


def should_skip(path: Path) -> bool:
    """Determine if a file should be skipped."""
    if path.suffix not in SCAN_EXTENSIONS:
        return True
    if path.suffix in EXCLUDED_EXTENSIONS:
        return True
    for part in path.parts:
        if part in EXCLUDED_DIRS:
            return True
    return False


def read_file(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="ignore")


def is_in_code_block(pos: int, content: str) -> bool:
    """Check if a position is within a @code ... @endcode block.

    Args:
        pos: The position to check in the content.
        content: The full file content.

    Returns:
        True if the position is inside a @code block, False otherwise.
    """
    # Track both @code and @htmlonly variations
    code_start_pattern = re.compile(r'@\s*(?:code|htmlonly|verbatim|dot|msc|plantuml)\b', re.IGNORECASE)
    code_end_pattern = re.compile(r'@\s*(?:endcode|endhtmlonly|endverbatim|enddot|endmsc|endplantuml)\b', re.IGNORECASE)

    # Find all code block boundaries before the position
    snippet = content[:pos]

    in_code = False
    # We need to check line by line to properly handle nesting
    lines = snippet.split('\n')
    for line in lines:
        if code_start_pattern.search(line):
            in_code = True
        elif code_end_pattern.search(line):
            in_code = False

    return in_code


def is_in_doxygen_comment(pos: int, content: str) -> bool:
    """Check if a position is within a doxygen comment block (/** ... */).

    Args:
        pos: The position to check in the content.
        content: The full file content.

    Returns:
        True if the position is inside a doxygen comment block, False otherwise.
    """
    # Look for doxygen comment blocks that might contain this position
    # We need to check if there's an unclosed doxygen comment before pos

    # Find the last unclosed doxygen comment start before pos
    snippet = content[:pos]

    # Find all /** ... */ blocks and check if any contain pos
    doxygen_pattern = re.compile(r'/\*\*.*?\*/', re.DOTALL)

    for match in doxygen_pattern.finditer(content):
        start = match.start()
        end = match.end()  # Position after */

        # Check if pos is within this comment block
        if start <= pos < end:
            return True

    # Also check for unclosed doxygen comments (edge case)
    # Find /** that doesn't have a matching */ before pos
    last_doxygen_start = snippet.rfind('/**')
    if last_doxygen_start != -1:
        # Check if there's a matching */ after the start
        comment_end = snippet.find('*/', last_doxygen_start + 3)
        if comment_end == -1:
            # Unclosed comment - everything from /** to pos is inside the comment
            return True

    return False


def check_line_length(content: str, file: Path) -> List[Violation]:
    violations: List[Violation] = []
    for i, line in enumerate(content.splitlines(), start=1):
        if len(line) > MAX_LINE_LENGTH:
            violations.append(
                Violation(file, f"Line {i} exceeds {MAX_LINE_LENGTH} characters")
            )
    return violations


def check_file_header(content: str, file: Path) -> List[Violation]:
    if not re.search(r"/\*\*.*?@file.*?\*/", content, re.DOTALL):
        return [Violation(file, "Missing file-level @file header")]
    return []


def is_private_type(content: str, type_pos: int) -> bool:
    """Check if an enum/struct/class is in a private section."""
    before_type = content[:type_pos]

    # Search backwards for access specifiers
    private_matches = list(re.finditer(r'\bprivate\s*:', before_type))
    protected_matches = list(re.finditer(r'\bprotected\s*:', before_type))
    public_matches = list(re.finditer(r'\bpublic\s*:', before_type))

    if not private_matches and not protected_matches and not public_matches:
        # No explicit access specifiers - determine based on enclosing type
        # Look for the type's own class/struct keyword and any enclosing type
        search_start = max(0, type_pos - 500)
        search_end = min(len(content), type_pos + 200)  # Look beyond for opening brace

        # Find the LAST class/struct keyword before type_pos
        # Exclude 'class' keyword that's part of 'enum class'
        last_class_pos = -1
        last_struct_pos = -1

        # Look for 'class' keyword, but not if preceded by 'enum' (for 'enum class')
        for match in re.finditer(r'\bclass\b', content[search_start:type_pos]):
            class_pos = search_start + match.start()
            # Check if this 'class' is part of 'enum class'
            # Look for 'enum' keyword immediately before 'class'
            before_class = content[max(0, class_pos - 10):class_pos]
            if not re.search(r'\benum\s*$', before_class):
                # This is a standalone 'class' keyword, not 'enum class'
                last_class_pos = class_pos

        for match in re.finditer(r'\bstruct\b', content[search_start:type_pos]):
            last_struct_pos = search_start + match.start()

        # Check if we found an enclosing type (not the type itself)
        # A type is enclosing if there's a { between its keyword and the current type_pos
        has_enclosing_type = False
        enclosing_type_is_class = False

        if last_class_pos >= 0 or last_struct_pos >= 0:
            # Found a keyword after search_start - could be an enclosing type
            last_keyword_pos = max(last_class_pos, last_struct_pos)
            last_keyword_type = 'class' if last_class_pos > last_struct_pos else 'struct'

            # Check if there's an opening brace between the keyword and type_pos
            between = content[last_keyword_pos:type_pos]
            has_brace = re.search(r'\{', between)

            if has_brace:
                # Found an opening brace - this is an enclosing type
                has_enclosing_type = True
                enclosing_type_is_class = (last_keyword_type == 'class')

        if not has_enclosing_type:
            # No enclosing type found - at namespace level
            # Determine the type keyword by searching closer to type_pos
            search_range_start = max(0, type_pos - 50)
            last_enum_pos = -1
            last_struct_pos2 = -1
            last_class_pos2 = -1

            # Find the LAST occurrence of each keyword before type_pos
            for match in re.finditer(r'\benum\b', content[search_range_start:type_pos]):
                last_enum_pos = search_range_start + match.start()

            for match in re.finditer(r'\bstruct\b', content[search_range_start:type_pos]):
                last_struct_pos2 = search_range_start + match.start()

            # Find 'class' but not if preceded by 'enum'
            for match in re.finditer(r'\bclass\b', content[search_range_start:type_pos]):
                class_pos = search_range_start + match.start()
                before_class = content[max(0, class_pos - 10):class_pos]
                if not re.search(r'\benum\s*$', before_class):
                    last_class_pos2 = class_pos

            # Determine which keyword is most recent (enum beats class for "enum class")
            if last_enum_pos > max(last_struct_pos2, last_class_pos2):
                # It's an enum (including "enum class")
                return False  # Namespace-level enum - check it
            elif last_class_pos2 > last_struct_pos2:
                # It's a namespace-level class - should check it (public API)
                return False
            else:
                return False  # Namespace-level struct - check it

        # Determine which keyword is more recent
        last_keyword_pos = max(last_class_pos, last_struct_pos)
        last_keyword_type = 'class' if last_class_pos > last_struct_pos else 'struct'

        # Check if there's an opening brace between the keyword and type_pos
        between = content[last_keyword_pos:type_pos]
        has_brace_before = re.search(r'\{', between)

        if has_brace_before:
            # Brace found before type_pos - we're inside an enclosing type
            if last_class_pos > last_struct_pos:
                return True  # Inside a class
            return False  # Inside a struct
        else:
            # No brace before type_pos - the keyword we found is for the type itself
            # At namespace level, apply default access rules
            if last_class_pos > last_struct_pos:
                return True  # It's a class at namespace level - skip it
            return False  # It's a struct at namespace level - check it

    # Get the most recent access specifier
    last_private = private_matches[-1].end() if private_matches else -1
    last_protected = protected_matches[-1].end() if protected_matches else -1
    last_public = public_matches[-1].end() if public_matches else -1

    last_access = max(last_private, last_protected, last_public)

    return last_access == last_private


def check_enum_struct_docs(content: str, file: Path) -> List[Violation]:
    violations: List[Violation] = []
    for match in ENUM_STRUCT_PATTERN.finditer(content):
        name = match.group(2)
        # Use the start position of group 2 (the type name), not the whole match
        type_pos = match.start(2) if len(match.groups()) >= 2 else match.start()
        # Also track where the entire match starts (for checking if keywords are part of this declaration)
        decl_start = match.start()

        # Skip if type is inside a @code block (example code)
        if is_in_code_block(type_pos, content):
            continue

        # Skip if type is inside a doxygen comment block (documentation example)
        if is_in_doxygen_comment(type_pos, content):
            continue

        # Skip private types (like private unions inside classes)
        if is_private_type(content, type_pos):
            continue

        # Increase search range to 3000 characters to find comments
        start = max(0, type_pos - 3000)
        snippet = content[start:type_pos]

        # Find all doxygen blocks in the snippet
        blocks = list(DOXYGEN_BLOCK_PATTERN.finditer(snippet))
        # Filter out file-level documentation (contains @file)
        type_blocks = [b for b in blocks if '@file' not in b.group()]

        # Check if there's a valid comment for this specific type
        has_valid_comment = False
        if type_blocks:
            # Get the last doxygen block before the type
            last_block = type_blocks[-1]
            block_end_in_snippet = last_block.end()
            block_end_in_content = start + block_end_in_snippet
            between = content[block_end_in_content:type_pos]

            # Check if there's another type keyword between the comment and this type
            # But exclude keywords that are part of this type's definition (e.g., "enum class")
            has_other_type_between = False
            for kw_match in re.finditer(r'\b(enum|struct|class|union)\b', between):
                kw_pos = block_end_in_content + kw_match.start()
                # If this keyword is at or after the declaration start, it's part of this type's declaration
                # (e.g., "class CF_UI_EXPORT MaterialColorScheme")
                if kw_pos >= decl_start:
                    # This is part of the current type's declaration, not a different type
                    continue
                # Check what's between this keyword and the type name
                kw_to_type = content[kw_pos + len(kw_match.group()):type_pos + len(name)]
                # If the type name appears shortly after without { or : it's part of the same definition
                if name in kw_to_type[:30]:
                    # Type name found, check if there's a { or : before it
                    name_idx = kw_to_type.index(name)
                    before_name = kw_to_type[:name_idx]
                    if not re.search(r'[{:]', before_name):
                        # No { or :, so it's part of this type's definition (e.g., "enum class TestEnum")
                        continue
                # Otherwise, it's a different type
                has_other_type_between = True
                break

            if not has_other_type_between:
                has_valid_comment = True
            else:
                # There's another type keyword between comment and this type
                # Check if the keyword is immediately followed by an access specifier
                # (meaning we're in a nested class situation with access specifiers)
                if re.search(r'\b(public|private|protected)\s*:', between):
                    # Check if the access specifier comes after the other type keyword
                    # and before the current type
                    # In this case, the comment might still be valid if it's after the access specifier
                    # Look for the last access specifier position
                    access_matches = list(re.finditer(r'\b(public|private|protected)\s*:', between))
                    if access_matches:
                        # There's an access specifier between the outer type and this type
                        # Check if the comment is after this access specifier
                        # by looking at what's between the comment end and the access specifier
                        last_access = access_matches[-1]
                        access_pos_in_between = last_access.start()
                        access_pos_in_content = block_end_in_content + access_pos_in_between
                        between_comment_and_access = content[block_end_in_content:access_pos_in_content]
                        # If there's no type keyword between comment and access specifier,
                        # the comment might be for this type (but this is complex)
                        # For simplicity, we'll require the comment to be AFTER the access specifier
                        pass

        # Also check for line-style comments (///)
        if not has_valid_comment and "///" in snippet:
            # Look for /// comments immediately before this type
            # Check if there's a type keyword between the last /// and this type
            # But exclude keywords that are part of this type's definition
            lines_before = content[:type_pos].split('\n')
            slash_comment_lines = []
            # Collect consecutive /// comments going backwards from the type
            for line in reversed(lines_before[-20:]):
                stripped = line.strip()
                if stripped.startswith('///'):
                    slash_comment_lines.append(line)
                elif stripped.startswith('//'):
                    # Different comment style, skip it
                    continue
                elif stripped == '':
                    # Empty line, continue looking
                    continue
                elif re.match(r'\s*(enum|struct|class|union)\b', stripped):
                    # Line starts with a type keyword - it's likely the type definition itself
                    # Skip it and continue looking for comments above
                    continue
                else:
                    # Non-empty, non-comment, non-type-keyword line - stop
                    break

            if slash_comment_lines:
                # We have some /// comments, now check if they're for this type
                # Get the position of the first (earliest) /// comment line
                # Since we collected in reverse order, the last element is the earliest
                first_slash_line = slash_comment_lines[-1]
                # Find this line in content
                slash_pos = content.rfind(first_slash_line, 0, type_pos)
                if slash_pos >= 0:
                    between = content[slash_pos + len(first_slash_line):type_pos]
                    # Check if there's another type keyword (excluding this type's own keyword)
                    has_other_type = False
                    for kw_match in re.finditer(r'\b(enum|struct|class|union)\b', between):
                        kw_pos = slash_pos + len(first_slash_line) + kw_match.start()
                        kw_to_type = content[kw_pos + len(kw_match.group()):type_pos + len(name)]
                        # Check if this keyword is part of this type's definition
                        if name in kw_to_type[:30]:
                            name_idx = kw_to_type.index(name)
                            before_name = kw_to_type[:name_idx]
                            if not re.search(r'[{:]', before_name):
                                # Part of this type's definition
                                continue
                        has_other_type = True
                        break
                    if not has_other_type:
                        has_valid_comment = True

        if not has_valid_comment:
            violations.append(Violation(file, "Missing Doxygen comment", name))
    return violations


def is_private_function(content: str, func_pos: int) -> bool:
    """Check if a function is in a private section."""
    # Find the last access specifier before this function
    before_func = content[:func_pos]

    # Search backwards for access specifiers
    private_matches = list(re.finditer(r'\bprivate\s*:', before_func))
    protected_matches = list(re.finditer(r'\bprotected\s*:', before_func))
    public_matches = list(re.finditer(r'\bpublic\s*:', before_func))

    if not private_matches and not protected_matches and not public_matches:
        # No explicit access specifiers - determine based on enclosing type
        search_start = max(0, func_pos - 500)

        # Find the LAST class/struct keyword before func_pos
        last_class_pos = -1
        last_struct_pos = -1

        for match in re.finditer(r'\bclass\b', content[search_start:func_pos]):
            last_class_pos = search_start + match.start()

        for match in re.finditer(r'\bstruct\b', content[search_start:func_pos]):
            last_struct_pos = search_start + match.start()

        # Determine which keyword is more recent
        last_keyword_pos = max(last_class_pos, last_struct_pos)
        last_keyword_type = 'class' if last_class_pos > last_struct_pos else 'struct'

        if last_keyword_pos < 0:
            # No class/struct keyword found - at namespace level, check it
            return False

        # Check if there's an opening brace between the keyword and func_pos
        between = content[last_keyword_pos:func_pos]
        has_brace = re.search(r'\{', between)

        if has_brace:
            # Opening brace found before func_pos - we're inside an enclosing type
            if last_class_pos > last_struct_pos:
                # Inside a class with no access specifier - defaults to private
                return True
            # Inside a struct with no access specifier - defaults to public
            return False
        else:
            # No opening brace - at namespace level
            return False

    # Get the most recent access specifier
    last_private = private_matches[-1].end() if private_matches else -1
    last_protected = protected_matches[-1].end() if protected_matches else -1
    last_public = public_matches[-1].end() if public_matches else -1

    last_access = max(last_private, last_protected, last_public)

    return last_access == last_private


def is_member_variable(content: str, func_pos: int, return_type: str = "", name: str = "") -> bool:
    """Check if a matched 'function' is actually a member variable declaration."""
    # Get a snippet after the match position
    snippet = content[func_pos:min(func_pos + 200, len(content))]

    # Check if it ends with semicolon (no function body)
    if not re.search(r';\s*(?://|/\*|\n|$)', snippet):
        # Has a function body { ... } - definitely a function
        return False

    # Ends with semicolon - could be member variable or function declaration
    full_signature = content[func_pos:func_pos + len(snippet)]

    # Function pointer patterns: (*name), (&name), (ClassName::*name)
    # These are definitely member variables
    if re.search(r'\(\s*\*[\s&]*\w+\s*\)', full_signature):
        return True  # Function pointer member variable
    if re.search(r'\(\s*\w+\s*::\s*\*\s*\w+\s*\)', full_signature):
        return True  # Member function pointer

    # Extract the identifier name from the snippet (first word-like thing)
    identifier_match = re.search(r'^(\w+)', snippet)
    identifier = identifier_match.group(1) if identifier_match else (name if name else "")

    # Helper function to check if params look like function parameters
    def has_function_params(sig: str) -> bool:
        """Check if signature has function-like parameters (with type annotations)."""
        params_match = re.search(r'\(([^)]*)\)', sig)
        if not params_match:
            return False
        params = params_match.group(1).strip()
        if not params:
            return False  # Empty params could be either
        # Check for type keywords in parameters
        type_keywords = ['int', 'void', 'char', 'float', 'double', 'bool', 'const',
                         'unsigned', 'signed', 'size_t', 'std::', 'auto', 'typename',
                         'class', 'struct', 'enum']
        return any(kw in params for kw in type_keywords)

    # If return_type is provided, use it for better detection
    if return_type:
        # Check if return type has angle brackets (template type like std::function)
        if '<' in return_type and '>' in return_type:
            return True  # Template type member variable

        # Check for common member variable naming conventions
        if identifier and (identifier.endswith('_') or identifier.startswith('k')):
            # Potential member variable by naming convention
            # But check if it has proper function parameters first
            if not has_function_params(full_signature):
                return True  # No function params + member var naming = member variable
            # Has function params - likely a function despite the naming

    # Fallback for when return_type is not provided (direct test cases)
    if re.search(r';\s*(?://|/\*|\n|$)', snippet):
        # Ends with semicolon

        # First check: Does it have function-like parameters?
        if has_function_params(full_signature):
            # Has proper function parameters - it's a function declaration
            return False

        # No function params - check other indicators
        # Check if there's an opening parenthesis (function-like)
        if not re.search(r'\(', snippet):
            # No parentheses at all - definitely a member variable
            return True

        # Has parentheses but no function params - could be:
        # - Function pointer (already handled above)
        # - Member variable with function-like syntax (e.g., std::function<void()>)
        # - Function declaration with empty params

        # Check for function declaration markers that indicate it's NOT a member variable
        # Look for trailing return type (-> type)
        if re.search(r'\s*->\s*[\w\:\<\>]+\s*;', full_signature):
            return False  # Trailing return type syntax - function declaration

        # Look for function specifiers after the params
        # These indicate a function declaration, not a member variable
        if re.search(r'\)\s*(?:const|override|final|noexcept|volatile|consteval|constinit)\s*;', full_signature):
            return False  # Has function specifiers - function declaration

        # Check for lambda initialization - variable with lambda as initializer
        # Pattern: Type name([](...) { ... });
        if re.search(r'\[\][\(\{]', full_signature):
            return True  # Lambda initialization - member variable (or local variable)

        # Check for member variable naming conventions
        if identifier and (identifier.endswith('_') or identifier.startswith('k') or
                           identifier.startswith('m_') or identifier.startswith('s_')):
            return True

        # Look backwards to see if there's a type keyword before the identifier
        # But ONLY if there are NO proper function params
        before = content[:func_pos]
        before_match = re.search(r'(\w+)\s*$', before[:50])  # Look at last 50 chars
        if before_match:
            last_word = before_match.group(1)
            type_keywords = ['int', 'float', 'double', 'char', 'bool', 'void', 'size_t',
                            'unsigned', 'signed', 'const', 'static']
            # Check for more context - look for angle brackets or :: in the type
            # If type is complex (template, scoped), it's likely a member variable
            # If type is simple and has empty params, it's likely a function declaration
            before_context = before[:100] if len(before) > 100 else before
            has_complex_type = '<' in before_context or '::' in before_context or 'std::' in before_context

            if last_word in type_keywords:
                # Simple type keyword before identifier
                if has_complex_type:
                    # Complex type like std::function, std::map, etc.
                    return True  # Member variable with complex type
                else:
                    # Simple type like int, void, etc.
                    # With empty params, this is likely a function declaration, NOT a member variable
                    # Example: int getValue(); is a function, int value_; is a member variable
                    # But if there's no '(' after the identifier, it's a member variable
                    if not re.search(r'\(', snippet):
                        return True  # No parentheses - member variable
                    # Has parentheses with simple type - function declaration
                    return False

    # Default: assume it's a function declaration
    return False


def check_function_blocks(content: str, file: Path) -> List[Violation]:
    violations: List[Violation] = []

    # Helper function to check if params look like function parameters (not function call arguments)
    def looks_like_function_call_params(params_str: str) -> bool:
        """Check if parameters look like a function call rather than declaration.

        Function calls have expressions like: obj.member, this->member
        Function declarations have types like: Type& name, const Type name
        """
        if not params_str:
            return False  # Empty params - could be either

        params_str = params_str.strip()

        # Check for member access patterns that indicate function calls
        if re.search(r'\w+\.\w+', params_str):
            return True  # Has obj.member pattern - likely a function call
        if re.search(r'\w+->\w+', params_str):
            return True  # Has obj->member pattern - likely a function call

        # Check for common type keywords - if present, likely a declaration
        type_keywords = ['int', 'void', 'char', 'float', 'double', 'bool', 'const',
                         'unsigned', 'signed', 'size_t', 'std::', 'auto', 'typename',
                         'class', 'struct', 'enum', 'typename']
        if any(kw in params_str for kw in type_keywords):
            return False  # Has type keywords - likely a function declaration

        # Check for reference/pointer modifiers - likely a declaration
        if '&' in params_str or '*' in params_str:
            return False  # Has reference or pointer - likely a declaration

        # If params are simple identifiers (no types, no member access), could be either
        # But typically function calls don't end with ; at the top level in a way that
        # our pattern would match without other context
        return False

    # Use finditer to get both match data and position
    for match in FUNCTION_PATTERN.finditer(content):
        return_type = match.group(1)
        name = match.group(2)
        params = match.group(3)
        func_pos = match.start()

        # Skip if return type is empty or only whitespace (likely a function call, not declaration)
        if not return_type or not return_type.strip():
            continue

        # Skip if return type is ":" - this is a member initializer list, not a function
        # Example: ": ICFAbstractAnimation(parent) {"
        if return_type.strip() == ':':
            continue

        # Skip if return type is "return" - this is a return statement, not a function
        # Example: "return WeakPtr();"
        if return_type.strip() == 'return':
            continue

        # Skip if function is inside a @code block (example code)
        if is_in_code_block(func_pos, content):
            continue

        # Skip if function is inside a doxygen comment block (documentation example)
        if is_in_doxygen_comment(func_pos, content):
            continue

        # Skip matches that look like function calls rather than declarations
        # This catches cases like: swap(obj1.member, obj2.member);
        if looks_like_function_call_params(params):
            # But still check if it's a valid function declaration with qualifiers
            # Get the full matched text to check for qualifiers
            full_match = match.group()
            # If it has noexcept, override, final, etc., it's a declaration
            if not re.search(r'\b(?:noexcept|override|final|consteval|constinit|virtual|inline|static|constexpr|friend|explicit)\b', full_match[:100]):
                continue

        # Skip union declarations - they are not functions
        # Look backwards to see if this is a union member declaration
        before_func = content[:func_pos]
        # Check if there's a union keyword before this position (within ~500 chars)
        union_search_start = max(0, func_pos - 500)
        union_context = content[union_search_start:func_pos]

        # Check if we're inside a union body
        # Look for 'union ... {' pattern before this position
        in_union = False
        for union_match in re.finditer(r'\bunion\s+(?:\w+\s*)?\{', content[union_search_start:func_pos]):
            # Found a union with opening brace
            # Now check if there's a closing brace before func_pos
            union_start = union_search_start + union_match.start()
            after_union = content[union_match.end():func_pos - union_search_start]
            # Count braces to see if we're still inside the union
            brace_count = 1
            for char in after_union:
                if char == '{':
                    brace_count += 1
                elif char == '}':
                    brace_count -= 1
                    if brace_count == 0:
                        break
            if brace_count > 0:
                # Still inside the union (no matching closing brace found)
                in_union = True
                break

        if in_union:
            # This is inside a union body
            # Skip only if it doesn't look like a member function (has params but no body declaration)
            # Union members without () are not functions
            if not params:
                continue

        # Skip keywords that are not functions
        if name in NON_FUNCTION_KEYWORDS:
            continue

        # Skip member variables (they look like: bool has_val_;)
        if is_member_variable(content, func_pos, return_type, name):
            continue

        # Skip private functions
        if is_private_function(content, func_pos):
            continue

        # Use func_pos consistently (from the regex match)
        # Increase search range to 2000 characters to find comments
        snippet = content[max(0, func_pos - 2000):func_pos]

        # Check for block-style comments (/** ... */)
        block_match = None
        blocks = [b for b in DOXYGEN_BLOCK_PATTERN.finditer(snippet) if '@file' not in b.group()]
        if blocks:
            # Check if the last block comment belongs to this function
            # by checking if there's another function between the comment and this function
            last_block = blocks[-1]
            block_end_in_snippet = last_block.end()
            block_end_in_content = max(0, func_pos - 2000) + block_end_in_snippet
            between = content[block_end_in_content:func_pos]

            # Check if there's another function definition between comment and this function
            has_other_func_between = False
            # Look for function-like patterns: return_type name(params)
            # Simplified check: look for semicolon followed by new function signature
            for other_match in FUNCTION_PATTERN.finditer(between):
                # Found another function pattern between comment and current function
                # But it could be a false positive (e.g., function pointer in return type)
                # Check if it ends with ; or {
                other_func_text = other_match.group()
                if other_func_text.rstrip().endswith(';') or other_func_text.rstrip().endswith('{'):
                    has_other_func_between = True
                    break

            if not has_other_func_between:
                block_match = last_block

        # Check for line-style comments (///)
        has_line_comment = False
        if "///" in snippet:
            # Collect consecutive /// comments before the function
            lines_before = content[:func_pos].split('\n')
            slash_comment_lines = []
            for line in reversed(lines_before[-30:]):
                stripped = line.strip()
                if stripped.startswith('///'):
                    slash_comment_lines.append(line)
                elif stripped.startswith('//'):
                    continue
                elif stripped == '':
                    continue
                elif re.match(r'\s*(\w+\s+)+\w+\s*\(', stripped):
                    # Line looks like a function declaration - stop
                    break
                else:
                    break

            if slash_comment_lines:
                has_line_comment = True

        if not block_match and not has_line_comment:
            violations.append(
                Violation(file, "Missing Doxygen block", name)
            )
            continue

        # Determine which comment block to use for validation
        if block_match:
            block = block_match.group()
        else:
            # Combine line-style comments for validation
            lines_before = content[:func_pos].split('\n')
            slash_comment_lines = []
            for line in reversed(lines_before[-30:]):
                stripped = line.strip()
                if stripped.startswith('///'):
                    slash_comment_lines.append(line)
                elif stripped.startswith('//'):
                    continue
                elif stripped == '':
                    continue
                elif re.match(r'\s*(\w+\s+)+\w+\s*\(', stripped):
                    break
                else:
                    break
            # Reverse to get original order and join
            block = '\n'.join(reversed(slash_comment_lines))

        # Only check core required tags
        for tag in CORE_REQUIRED_TAGS:
            if tag not in block:
                violations.append(
                    Violation(file, f"Missing required tag {tag}", name)
                )

        # Normalize return type by removing extra whitespace and newlines
        normalized_return_type = re.sub(r'\s+', ' ', return_type).strip()

        # Handle cases where return_type is empty (e.g., constructors/destructors)
        # In such cases, treat as void
        if not normalized_return_type or name == '~' + name[1:] if '~' in name else False:
            normalized_return_type = "void"

        # Handle constructors/destructors - class name same as function name
        # Check if the "return type" is actually just part of the class declaration
        if normalized_return_type and normalized_return_type.split()[-1] == name:
            # This is likely a constructor (ClassName ClassName(...))
            normalized_return_type = "void"

        # Handle constructors where return_type is just a keyword (constexpr, explicit, etc.)
        # This happens when a constructor has only keywords before the class name
        # e.g., constexpr WeakPtr() -> return_type="constexpr", name="WeakPtr"
        cvr_keywords = {'constexpr', 'consteval', 'constinit', 'inline', 'explicit', 'static'}
        if normalized_return_type in cvr_keywords:
            normalized_return_type = "void"

        # Skip @return check for void functions that have explanatory @return
        # (e.g., "Returns void if in success state" - this is documentation, not a type error)
        # Only warn if @return describes an actual type (not just "void" or descriptive text)
        if normalized_return_type == "void" and "@return" in block:
            # Check if @return contains more than just "void" or descriptive text
            # This is okay: "@return     void if in success state."
            # This is NOT okay: "@return     SomeType"
            return_lines = [line for line in block.splitlines() if "@return" in line]
            for line in return_lines:
                # Remove @return and common words to see if there's an actual type mentioned
                cleaned = re.sub(r'@return\s*', '', line)
                cleaned = re.sub(r'\b(void|null|none|nothing)\b', '', cleaned, flags=re.IGNORECASE)
                cleaned = re.sub(r'[^\w]', '', cleaned)
                if cleaned and len(cleaned) > 3:
                    # There's something that looks like a type name
                    violations.append(
                        Violation(file, "Void function contains @return with type", name)
                    )
                    break

        if normalized_return_type != "void" and "@return" not in block:
            violations.append(
                Violation(file, "Non-void function missing @return", name)
            )

        for line in block.splitlines():
            if "@param" in line and not re.search(r"\[(in|out|in,out)\]", line):
                violations.append(
                    Violation(file, "@param missing direction", name)
                )

    return violations


def check_language_rules(content: str, file: Path) -> List[Violation]:
    violations: List[Violation] = []

    # Check for first-person pronouns
    # For 'I', use case-sensitive pattern to avoid matching loop variable 'i'
    if FIRST_PERSON_I_PATTERN.search(content):
        violations.append(Violation(file, "First-person usage detected"))
    # For we/our/my, use case-insensitive pattern
    elif FIRST_PERSON_WE_PATTERN.search(content):
        violations.append(Violation(file, "First-person usage detected"))

    if FUTURE_TENSE_PATTERN.search(content):
        violations.append(Violation(file, "Future tense 'will' detected"))

    return violations


def analyze_file(path: Path) -> List[Violation]:
    content = read_file(path)

    violations: List[Violation] = []
    violations += check_line_length(content, path)
    violations += check_file_header(content, path)
    violations += check_enum_struct_docs(content, path)
    violations += check_function_blocks(content, path)
    violations += check_language_rules(content, path)

    return violations


def collect_files(root: Path) -> Iterable[Path]:
    for path in root.rglob("*"):
        if path.is_file() and not should_skip(path):
            yield path


def lint_files(files: List[str]) -> List[Violation]:
    violations: List[Violation] = []
    for name in files:
        file_path = Path(name)
        if file_path.suffix not in SCAN_EXTENSIONS:
            continue
        violations.extend(analyze_file(file_path))
    return violations


def write_failed_markdown(violations: List[Violation]) -> None:
    if not violations:
        if FAILED_OUTPUT_FILE.exists():
            FAILED_OUTPUT_FILE.unlink()
        return

    lines: List[str] = [
        "# FAILED DOXYGEN CHECKS",
        "# **Please Refer To .claude/tools/DOXYGEN_REQUEST.md to see how to implement doxygens!!!**",
        f"Total violations: {len(violations)}",
        "",
    ]

    for violation in violations:
        lines.append(violation.to_markdown())

    FAILED_OUTPUT_FILE.write_text("\n".join(lines), encoding="utf-8")


# ============================================================
# ========================= MAIN =============================
# ============================================================


def main() -> int:
    args = sys.argv[1:]

    if args and args[0] == "--files":
        all_violations = lint_files(args[1:])
        for violation in all_violations:
            print(violation.to_markdown())
        if all_violations:
            print(f"FAILED: {len(all_violations)} violations")
            return 1
        print("All Doxygen checks passed")
        return 0

    root = Path(args[0]).resolve() if args else PROJECT_ROOT

    all_violations: List[Violation] = []

    for file_path in collect_files(root):
        all_violations.extend(analyze_file(file_path))

    write_failed_markdown(all_violations)

    if all_violations:
        print(f"FAILED: {len(all_violations)} violations")
        print(f"See: {FAILED_OUTPUT_FILE}")
        return 1

    print("All Doxygen checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())