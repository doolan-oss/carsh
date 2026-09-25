// Circle entry point for the carsh A/B chainloader. Unlike the app image's
// main.cpp (which calls reboot() for a real PM reset so the firmware reloads
// this chainloader), this one RETURNS to Circle's runtime: sysinit checks
// IsChainBootEnabled() on EXIT_REBOOT and warm-jumps to the loaded slot image.
#include "kernel.h"

#include <circle/startup.h>

int main(void) {
    CKernel Kernel;
    if (!Kernel.Initialize()) {
        halt();
        return EXIT_HALT;
    }

    CStdlibApp::TShutdownMode ShutdownMode = Kernel.Run();
    Kernel.Cleanup();

    return ShutdownMode == CStdlibApp::ShutdownReboot ? EXIT_REBOOT : EXIT_HALT;
}
