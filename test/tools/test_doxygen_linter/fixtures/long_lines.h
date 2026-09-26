/**
 * @file    long_lines.h
 * @brief   File with lines exceeding max length.
 *
 * @author  Test
 * @date    2024-01-01
 * @version 1.0.0
 * @since   1.0.0
 * @ingroup test
 */

#ifndef LONG_LINES_H
#define LONG_LINES_H

// This line is way too long and exceeds the maximum allowed length of 100 characters by quite a bit
// and it keeps going on purpose with padding words past the column limit and should be detected

/**
 * @brief  A function with a very long documentation line that exceeds the maximum allowed length of
 * 100 characters and should trigger a violation with extra padding words.
 *
 * @param[in]  input  This is a parameter description that is also quite long and exceeds the
 * maximum line length limit of 100 characters.
 * @return              A very long return value description that clearly exceeds the maximum line
 * length of 100 characters with extra words.
 * @throws              None
 * @note                This note exceeds the maximum allowed line length of 100 characters and
 * should be caught by the linter because of padding.
 * @warning             None
 * @since               1.0.0
 * @ingroup             test
 */
int long_function(int input);

#endif  // LONG_LINES_H
