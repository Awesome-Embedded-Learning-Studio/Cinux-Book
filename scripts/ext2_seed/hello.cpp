#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    const std::vector<std::string> kWords{"Hello", "from", "G++!"};
    for (unsigned long index = 0; index < kWords.size(); ++index) {
        if (index != 0) {
            std::cout << ' ';
        }
        std::cout << kWords[index];
    }
    std::cout << '\n';
    try {
        throw std::runtime_error("caught");
    } catch (const std::exception& error) {
        std::cout << "exception: " << error.what() << '\n';
    }
    return 0;
}
