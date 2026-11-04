// Runs the host sender's own tests (tests/ota/carsh_ota_test.py: the
// flow-controlled push in scripts/carsh_ota.py against a simulated kernel) as
// part of carsh_tests, so the Python half of the PushProtocol contract is gated
// with the C++ half. Passes with a note when no python3 is installed.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "Test.h"

TEST(PushSenderScript, PythonFlowControlSuite) {
    if (std::system("python3 -c \"import sys; sys.exit(sys.version_info < (3, 6))\""
                    " > /dev/null 2>&1") != 0) {
        std::printf("         python3 >= 3.6 not found — carsh_ota.py tests not run\n");
        return;
    }
    const std::string cmd = std::string("python3 -B \"") + CARSH_TEST_DIR +
                            "/ota/carsh_ota_test.py\" > /dev/null 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc != 0)
        std::printf("         rerun for details: python3 %s/ota/carsh_ota_test.py\n",
                    CARSH_TEST_DIR);
    CHECK_EQ(rc, 0);
}
