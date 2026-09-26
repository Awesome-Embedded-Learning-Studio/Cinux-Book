/**
 * @file    complex_functions.h
 * @brief   Test complex function patterns.
 *
 * Tests templates, constexpr, virtual, etc.
 *
 * @author  Test
 * @date    2024-01-01
 * @version 1.0.0
 * @since   1.0.0
 * @ingroup test
 */

#ifndef COMPLEX_FUNCTIONS_H
#define COMPLEX_FUNCTIONS_H

// Template function - undocumented
template <typename T>
T undoc_template(T x);

/**
 * @brief  Documented template function.
 *
 * @tparam T  The type parameter.
 * @param[in]  x  Input value.
 * @return        The result.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
template <typename T>
T doc_template(T x);

// Constexpr function - undocumented
constexpr int undoc_constexpr(int x) {
    return x * 2;
}

/**
 * @brief  Documented constexpr.
 *
 * @param[in]  x  Input.
 * @return        Double the input.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
constexpr int doc_constexpr(int x) {
    return x * 2;
}

// Virtual function - undocumented
virtual int undoc_virtual() = 0;

/**
 * @brief  Documented virtual function.
 *
 * @return        The result.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
virtual int doc_virtual() = 0;

// Static function - undocumented
static int undoc_static(int x);

/**
 * @brief  Documented static function.
 *
 * @param[in]  x  Input.
 * @return        Result.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
static int doc_static(int x);

// Explicit constructor - undocumented
explicit MyClass(int x);

/**
 * @brief  Documented explicit constructor.
 *
 * @param[in]  x  Initial value.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
explicit MyClass(int x);

// noexcept function
/**
 * @brief  Documented noexcept function.
 *
 * @param[in]  x  Input.
 * @return        Result.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
int doc_noexcept(int x) noexcept;

#endif  // COMPLEX_FUNCTIONS_H
