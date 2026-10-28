/**
 * @file    primes.hpp
 * @brief   Prime-number predicate for the standalone compiler example.
 * @author  Charliechen114514
 * @date    2026-10-27
 * @version 1.0
 * @since   1.0
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace demo {

/**
 * @brief Determine whether an unsigned integer is prime.
 * @param[in] candidate Integer to examine.
 * @return True when candidate has exactly two positive divisors.
 */
bool IsPrime(unsigned int candidate);

}
