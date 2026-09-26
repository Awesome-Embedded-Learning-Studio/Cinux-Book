/**
 * @file    missing_return.h
 * @brief   Test missing @return tag.
 *
 * @author  Test
 * @date    2024-01-01
 * @version 1.0.0
 * @since   1.0.0
 * @ingroup test
 */

#ifndef MISSING_RETURN_H
#define MISSING_RETURN_H

/**
 * @brief  Non-void function missing @return.
 *
 * @param[in]  x  Input value.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
int no_return(int x);

/**
 * @brief  Void function with @return - should warn.
 *
 * @param[in]  flag  A flag.
 * @return            void - bad!
 * @throws            None
 * @note              None
 * @warning           None
 * @since             1.0.0
 * @ingroup           test
 */
void void_with_return(bool flag);

/**
 * @brief  Proper void function without @return.
 *
 * @param[in]  flag  A flag.
 * @throws            None
 * @note              No return needed.
 * @warning           None
 * @since             1.0.0
 * @ingroup           test
 */
void void_no_return(bool flag);

/**
 * @brief  Proper non-void function with @return.
 *
 * @param[in]  x  Input.
 * @return        The result.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
int has_return(int x);

#endif  // MISSING_RETURN_H
