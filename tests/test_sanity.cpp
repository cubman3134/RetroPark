#include <doctest/doctest.h>
#include <retropark/retropark_abi.h>

TEST_CASE("sanity: build harness is wired") {
    CHECK(1 + 1 == 2);
}

extern "C" int retropark_abi_c_check(void);

TEST_CASE("abi: header compiles as C and constants are sane") {
    CHECK(retropark_abi_c_check() == static_cast<int>(RETROPARK_ABI_VERSION));
}
