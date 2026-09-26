/**
 * @file    language_issues.h
 * @brief   Test language rule violations.
 *
 * We will test first-person and future tense.
 * I think this file has issues.
 *
 * @author  Test
 * @date    2024-01-01
 * @version 1.0.0
 * @since   1.0.0
 * @ingroup test
 */

#ifndef LANGUAGE_ISSUES_H
#define LANGUAGE_ISSUES_H

/**
 * @brief  We initialize the cache.
 *
 * This function will compute the result.
 *
 * @param[in]  input  Our input value.
 * @return             My return value.
 * @throws             None
 * @note               I add a note here.
 * @warning            None
 * @since              1.0.0
 * @ingroup            test
 */
int badLanguage(int input);

/**
 * @brief  Proper third-person present tense.
 *
 * @param[in]  input  The input value.
 * @return             The computed result.
 * @throws             None
 * @note               This is correct.
 * @warning            None
 * @since              1.0.0
 * @ingroup            test
 */
int goodLanguage(int input);

#endif  // LANGUAGE_ISSUES_H
