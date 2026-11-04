// Link-time hooks that carsh's patched circle-stdlib expects from every image.
//
// scripts/patches/usbdevicefactory-aoa.patch makes Circle's USB device factory
// call carshCreateAndroidAutoDevice(), which the app defines in
// device/CircleAoaTransport.cpp. The chainloader never starts USB, but
// circle-newlib's io.o references CUSBHostController, which drags the factory
// (and with it this hook) into every circle-stdlib link — without a definition
// here the chainloader doesn't link at all. It never binds a device.
class CUSBFunction;

extern "C" CUSBFunction* carshCreateAndroidAutoDevice(CUSBFunction* /*pParent*/,
                                                      const char* /*pName*/) {
    return nullptr;
}
