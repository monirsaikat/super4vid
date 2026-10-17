// Default entry point for test executables. It lives in its own object file inside the ct_test static library,
// so the linker only pulls it in when the test exe does not define main() itself.
#include <ct/test/ct_test.h>

int main(int argc, char** argv) {
    return ct::test::RunAll(argc, argv);
}
