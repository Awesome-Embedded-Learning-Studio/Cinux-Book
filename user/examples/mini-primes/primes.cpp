#include "primes.hpp"

namespace demo {

bool IsPrime(unsigned int candidate) {
    if (candidate < 2) {
        return false;
    }
    for (unsigned int divisor = 2; divisor <= candidate / divisor; ++divisor) {
        if (candidate % divisor == 0) {
            return false;
        }
    }
    return true;
}

}
