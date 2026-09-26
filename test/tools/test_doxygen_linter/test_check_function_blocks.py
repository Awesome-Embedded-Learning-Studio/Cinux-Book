"""Tests for check_function_blocks function in lint.py.

Tests verify that the linter correctly validates doxygen comments
according to the DOXYGEN_REQUEST.md specification.
"""

from __future__ import annotations

import sys
from pathlib import Path

# Add scripts/doxygen to path to import lint module
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent / "scripts" / "doxygen"))

import lint

from test_framework import TestCase, assert_len, assert_equal


class TestCheckFunctionBlocks(TestCase):
    """Test cases for the check_function_blocks function."""

    def test_fully_documented_function(self) -> None:
        """Test that fully documented function passes."""
        content = """/**
 * @brief  Adds two numbers.
 *
 * @param[in]  a  First number.
 * @param[in]  b  Second number.
 * @return        Sum of a and b.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
int add(int a, int b);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_function_without_doxygen(self) -> None:
        """Test that function without doxygen is detected."""
        content = "int add(int a, int b);"
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].message, "Missing Doxygen block")

    def test_missing_brief_tag(self) -> None:
        """Test that missing @brief is detected."""
        content = """/**
 * @param[in]  a  First number.
 * @param[in]  b  Second number.
 * @return        Sum.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
int add(int a, int b);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].message, "Missing required tag @brief")

    def test_non_void_function_without_return(self) -> None:
        """Test that non-void function without @return is detected."""
        content = """/**
 * @brief  Adds two numbers.
 * @param[in]  a  First number.
 * @param[in]  b  Second number.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
int add(int a, int b);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].message, "Non-void function missing @return")

    def test_void_function_with_return_type_description(self) -> None:
        """Test that void function with @return describing a type is flagged."""
        content = """/**
 * @brief  Does something.
 * @param[in]  flag  A flag.
 * @return            SomeType
 * @throws            None
 * @note              None
 * @warning           None
 * @since             1.0.0
 * @ingroup           test
 */
void do_something(bool flag);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].message, "Void function contains @return with type")

    def test_void_function_without_return(self) -> None:
        """Test that void function without @return passes."""
        content = """/**
 * @brief  Does something.
 * @param[in]  flag  A flag.
 * @throws            None
 * @note              None
 * @warning           None
 * @since             1.0.0
 * @ingroup           test
 */
void do_something(bool flag);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_param_without_direction(self) -> None:
        """Test that param without direction is detected."""
        content = """/**
 * @brief  Adds two numbers.
 * @param  a  First number.
 * @param  b  Second number.
 * @return     Sum.
 * @throws     None
 * @note       None
 * @warning    None
 * @since      1.0.0
 * @ingroup    test
 */
int add(int a, int b);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 2)  # Both params missing direction
        for v in violations:
            assert_equal(v.message, "@param missing direction")

    def test_private_function_is_skipped(self) -> None:
        """Test that private functions are skipped."""
        content = """class MyClass {
private:
    int privateFunc(int x);
public:
    /**
     * @brief  Public function.
     * @param[in]  x  Input.
     * @return        Result.
     * @throws        None
     * @note          None
     * @warning       None
     * @since         1.0.0
     * @ingroup       test
     */
    int publicFunc(int x);
};"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_constexpr_function(self) -> None:
        """Test constexpr function detection."""
        content = "constexpr int compute(int x) { return x * 2; }"
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 1)  # Missing doxygen block

    def test_function_with_default_param(self) -> None:
        """Test function with default parameter."""
        content = """/**
 * @brief  Function with default param.
 * @param[in]  x  Input value.
 * @return        Result.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
int withDefault(int x = 42);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_line_style_comment(self) -> None:
        """Test line-style doxygen comments."""
        content = """/// @brief  Line style function.
/// @param[in]  x  Input.
/// @return        Result.
/// @throws        None
/// @note          None
/// @warning       None
/// @since         1.0.0
/// @ingroup       test
int lineStyle(int x);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_operator_overload(self) -> None:
        """Test operator overloading."""
        content = """/**
 * @brief  Addition operator.
 * @param[in]  other  Right operand.
 * @return             Result.
 * @throws             None
 * @note               None
 * @warning            None
 * @since              1.0.0
 * @ingroup            test
 */
MyClass operator+(const MyClass& other);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_constructor(self) -> None:
        """Test constructor."""
        content = """/**
 * @brief  Constructor.
 * @param[in]  x  Initial value.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
MyClass(int x);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_destructor(self) -> None:
        """Test destructor."""
        content = """/**
 * @brief  Destructor.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
~MyClass();"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_template_function(self) -> None:
        """Test template function."""
        content = "template<typename T> T generic(T x);"
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 1)  # Missing doxygen block

    def test_const_method(self) -> None:
        """Test const method."""
        content = "int getValue() const;"
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 1)  # Missing doxygen block

    def test_static_function(self) -> None:
        """Test static function."""
        content = "static int helper(int x);"
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 1)  # Missing doxygen block

    def test_virtual_function(self) -> None:
        """Test virtual function."""
        content = "virtual int pureVirtual() = 0;"
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 1)  # Missing doxygen block

    def test_override_method(self) -> None:
        """Test override method."""
        content = "int getValue() override;"
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 1)  # Missing doxygen block

    def test_noexcept_function(self) -> None:
        """Test noexcept function."""
        content = "int safeFunc() noexcept;"
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 1)  # Missing doxygen block

    def test_explicit_constructor(self) -> None:
        """Test explicit constructor."""
        content = "explicit MyClass(int x);"
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 1)  # Missing doxygen block

    def test_multiple_functions(self) -> None:
        """Test multiple functions in same file."""
        content = """/**
 * @brief  First function.
 * @param[in]  x  Input.
 * @return        Result.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
int first(int x);

int second(int y);  // Undocumented

/**
 * @brief  Third function.
 * @param[in]  z  Input.
 * @return        Result.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
int third(int z);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        # Only second() should have violations
        assert_len(violations, 1)
        assert_equal(violations[0].symbol, "second")

    def test_return_statement_not_matched_as_function(self) -> None:
        """Test that return statements are not matched as function declarations."""
        content = """/**
 * @brief  Dynamically converts Base to Derived.
 *
 * @param[in]  other  WeakPtr to the source object.
 *
 * @return        WeakPtr<Derived> pointing to the same object.
 *
 * @throws        None
 * @note          The returned WeakPtr shares the same weak reference.
 * @warning       Always check the returned WeakPtr's validity before use.
 * @since         0.1
 * @ingroup       none
 */
template <typename Source>
static WeakPtr DynamicCast(const WeakPtr<Source>& other) noexcept {
    if (!other.flag_ || !other.flag_->IsAlive()) {
        return WeakPtr();  // Should not be flagged as missing Doxygen
    }
    T* casted = dynamic_cast<T*>(other.ptr_);
    if (casted) {
        return WeakPtr(casted, other.flag_);  // Should not be flagged
    }
    return WeakPtr();  // Should not be flagged
}"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        # DynamicCast is fully documented, return statements should not cause extra violations
        assert_len(violations, 0)

    def test_undocumented_function_with_return_statements(self) -> None:
        """Test that return statements don't create false violations."""
        content = """
// Undocumented function
static WeakPtr CreateWeakPtr() {
    return WeakPtr();  // Should not be flagged as a separate missing Doxygen
}
"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        # Only CreateWeakPtr should have a violation, not the return statement
        assert_len(violations, 1)
        assert_equal(violations[0].symbol, "CreateWeakPtr")

    def test_member_initializer_list_not_matched_as_function(self) -> None:
        """Test that member initializer lists are not matched as function declarations."""
        content = """/**
 * @brief  Constructor with spring easing preset.
 *
 * @param[in] easing Spring preset for physics parameters.
 * @param[in] parent QObject parent.
 *
 * @throws     None
 * @note       None
 * @warning    None
 * @since      0.1
 * @ingroup    ui_components
 */
ICFSpringAnimation(const base::Easing::SpringPreset& easing, QObject* parent = nullptr)
    : ICFAbstractAnimation(parent) {  # Should not be flagged as a separate function
    easing_ = easing;
}"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        # Constructor is documented, initializer list should not cause extra violations
        assert_len(violations, 0)

    def test_constructor_with_return_statement(self) -> None:
        """Test constructor that contains return statements (for nested helper functions)."""
        content = """/**
 * @brief  Default constructor: creates an empty weak reference.
 * @throws None
 * @note   None
 * @warning None
 * @since  0.1
 * @ingroup none
 */
constexpr WeakPtr() noexcept = default;

/**
 * @brief  Constructs an empty weak reference from nullptr.
 * @param[in] nullptr_t The nullptr literal.
 * @throws None
 * @note   None
 * @warning None
 * @since  0.1
 * @ingroup none
 */
constexpr WeakPtr(std::nullptr_t) noexcept {}"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        # Both constructors are documented
        assert_len(violations, 0)
