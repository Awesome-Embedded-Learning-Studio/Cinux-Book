#include "../framework/framework.hpp"
#include "cinux/checksum.hpp"
#include "test_assert.hpp"

namespace {

const unsigned char kRfcExample[8] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};

}  // namespace

TEST("checksum: RFC 1071 worked example folds to 0x220d") {
    cinux::base::InternetChecksum checksum;
    checksum.add(kRfcExample, sizeof(kRfcExample));
    ASSERT_TRUE(checksum.value() == 0x220d);
}

TEST("checksum: the example carrying its own field verifies to zero") {
    const unsigned char kVerified[10] = {
        0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7, 0x22, 0x0d,
    };
    cinux::base::InternetChecksum checksum;
    checksum.add(kVerified, sizeof(kVerified));
    ASSERT_TRUE(checksum.value() == 0x0000);
}

TEST("checksum: an odd span pads the lone byte with a zero low half") {
    const unsigned char           kOdd[3] = {0x00, 0x01, 0xf2};
    cinux::base::InternetChecksum checksum;
    checksum.add(kOdd, sizeof(kOdd));
    ASSERT_TRUE(checksum.value() == 0x0dfe);
}

TEST("checksum: a split across an odd boundary keeps the RFC answer") {
    const unsigned char           kTail[5] = {0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    cinux::base::InternetChecksum checksum;
    checksum.add(kRfcExample, 3);
    checksum.add(kTail, sizeof(kTail));
    ASSERT_TRUE(checksum.value() == 0x220d);
}

TEST("checksum: zero input complements to 0xffff") {
    const unsigned char           kZeros[4] = {0, 0, 0, 0};
    cinux::base::InternetChecksum checksum;
    checksum.add(kZeros, sizeof(kZeros));
    ASSERT_TRUE(checksum.value() == 0xffff);
}

TEST("checksum: asking twice without adding answers the same word") {
    cinux::base::InternetChecksum checksum;
    checksum.add(kRfcExample, sizeof(kRfcExample));
    ASSERT_TRUE(checksum.value() == 0x220d);
    ASSERT_TRUE(checksum.value() == 0x220d);
}

int main() {
    return cinux::test::RunAll();
}
