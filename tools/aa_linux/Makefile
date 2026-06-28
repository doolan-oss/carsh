# aa_linux — build carsh's AA core + a libusb transport into a PC diagnostic binary.
#
# Needs host mbedTLS 2.28 (the vendored source built for x86). Build it once with:
#   cmake -S ../../third_party/circle-stdlib/libs/mbedtls -B /tmp/mbedtls-host \
#         -DENABLE_TESTING=Off -DENABLE_PROGRAMS=Off -DCMAKE_POLICY_VERSION_MINIMUM=3.5
#   cmake --build /tmp/mbedtls-host -j
# then `make` here. Run with `sudo ./aa_linux` (USB interface claim needs privileges).

CARSHTOP    := $(realpath ../..)
MBEDTLS_HOST ?= /tmp/mbedtls-host
MBEDTLS_INC := $(CARSHTOP)/third_party/circle-stdlib/libs/mbedtls/include

CXX      := g++
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wno-unused-parameter
INCLUDES := -I$(CARSHTOP)/src -I$(CARSHTOP)/device -I. -I$(MBEDTLS_INC) \
            $(shell pkg-config --cflags libusb-1.0)
LIBS     := -L$(MBEDTLS_HOST)/library -lmbedtls -lmbedx509 -lmbedcrypto \
            $(shell pkg-config --libs libusb-1.0)

SRCS := main.cpp LibUsbAaTransport.cpp \
        $(CARSHTOP)/src/aa/AaConnection.cpp \
        $(CARSHTOP)/src/aa/AaMessenger.cpp \
        $(CARSHTOP)/device/AaCryptoMbedTls.cpp

aa_linux: $(SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRCS) $(LIBS) -o aa_linux

clean:
	rm -f aa_linux
.PHONY: clean
