/**
 * @file    missing_param_direction.h
 * @brief   Test missing parameter direction tags.
 *
 * @author  Test
 * @date    2024-01-01
 * @version 1.0.0
 * @since   1.0.0
 * @ingroup test
 */

#ifndef MISSING_PARAM_DIRECTION_H
#define MISSING_PARAM_DIRECTION_H

/**
 * @brief  Function with params missing direction.
 *
 * @param  x  Input value - no direction.
 * @param  y  Another value - no direction.
 * @return     The sum.
 * @throws     None
 * @note       None
 * @warning    None
 * @since      1.0.0
 * @ingroup    test
 */
int add(int x, int y);

/**
 * @brief  Function with proper directions.
 *
 * @param[in]  a  First value.
 * @param[in]  b  Second value.
 * @return        The sum.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         1.0.0
 * @ingroup       test
 */
int add_correct(int a, int b);

#endif  // MISSING_PARAM_DIRECTION_H
