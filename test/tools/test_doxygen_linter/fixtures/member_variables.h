/**
 * @file    member_variables.h
 * @brief   Test member variable detection.
 *
 * Tests that member variables are not treated as functions.
 *
 * @author  Test
 * @date    2024-01-01
 * @version 1.0.0
 * @since   1.0.0
 * @ingroup test
 */

#ifndef MEMBER_VARIABLES_H
#define MEMBER_VARIABLES_H
#include <functional>
#include <vector>

/**
 * @brief  A class with member variables.
 * @ingroup test
 */
class HasMembers {
public:
    // Member variables - should NOT be treated as functions
    bool                  has_value_;
    int                   count_;
    float*                ptr_;
    std::function<void()> callback_;

    /**
     * @brief  Actual function.
     *
     * @param[in]  x  Input.
     * @return        Result.
     * @throws        None
     * @note          None
     * @warning       None
     * @since         1.0.0
     * @ingroup       test
     */
    int realFunction(int x);

    // Member variable with default value
    int initialized_ = 42;

    // Member variable that looks like a function declaration
    // (has parens in the type, not as parameters)
    std::vector<int (*)()> func_ptr_;
};

// Member variable at namespace scope
inline int global_variable = 0;

#endif  // MEMBER_VARIABLES_H
