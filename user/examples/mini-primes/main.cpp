#include <charconv>
#include <iostream>
#include <string_view>
#include <system_error>

#include "primes.hpp"

int main(int argc, char* argv[]) {
    if (argc > 2) {
        std::cerr << "Usage: mini-primes [limit: 2..10000]\n";
        return 2;
    }
    unsigned int limit = 100;
    if (argc == 2) {
        const std::string_view kInput{argv[1]};
        const auto kParsed = std::from_chars(kInput.data(), kInput.data() + kInput.size(), limit);
        if (kParsed.ec != std::errc{} || kParsed.ptr != kInput.data() + kInput.size() ||
            limit < 2 || limit > 10000) {
            std::cerr << "Limit must be an integer from 2 to 10000.\n";
            return 2;
        }
    }
    std::cout << "Primes up to " << limit << ":\n";
    unsigned int count = 0;
    for (unsigned int candidate = 2; candidate <= limit; ++candidate) {
        if (demo::IsPrime(candidate)) {
            std::cout << (count == 0 ? "" : " ") << candidate;
            ++count;
        }
    }
    std::cout << "\nFound " << count << " primes.\n";
}
