/**
 * @file    missing_brief.h
 * @brief   File to test missing @brief tag.
 *
 * @author  Test
 * @date    2024-01-01
 * @version 1.0.0
 * @since   1.0.0
 * @ingroup test
 */

#ifndef MISSING_BRIEF_H
#define MISSING_BRIEF_H

/**
 * @param[in]  x  Input value.
 * @return        The result.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
int no_brief_tag(int x);

/**
 * Function with no doxygen at all.
 */
int no_doxygen(int y);

/**
 * @brief  This one is valid.
 * @param[in]  z  Input.
 * @return        Result.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
int has_brief_tag(int z);

#endif  // MISSING_BRIEF_H
