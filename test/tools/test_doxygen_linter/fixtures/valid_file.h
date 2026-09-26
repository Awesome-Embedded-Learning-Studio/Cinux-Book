/**
 * @file    valid_file.h
 * @brief   This is a valid file with proper Doxygen comments.
 *
 * Demonstrates all required Doxygen patterns.
 *
 * @author  Test Author
 * @date    2024-01-01
 * @version 1.0.0
 * @since   1.0.0
 * @ingroup test
 */

#ifndef VALID_FILE_H
#define VALID_FILE_H

/**
 * @brief  A simple enum for testing.
 *
 * @ingroup test
 */
enum class Status {
    Ok,     ///< Status is okay.
    Error,  ///< Status indicates error.
};

/**
 * @brief  A test structure.
 *
 * @ingroup test
 */
struct TestData {
    int value;  ///< The value.
};

/**
 * @brief  Computes the sum of two integers.
 *
 * @param[in]  a    First operand.
 * @param[in]  b    Second operand.
 * @return          The sum of a and b.
 * @throws          None
 * @note            This is a simple addition.
 * @warning         None
 * @since           1.0.0
 * @ingroup         test
 */
int add(int a, int b);

/**
 * @brief  Does nothing and returns void.
 *
 * @param[in]  flag  A flag parameter.
 * @note            No return value.
 * @warning         None
 * @since           1.0.0
 * @ingroup         test
 */
void do_something(bool flag);

#endif  // VALID_FILE_H
