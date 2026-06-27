#include "CircleBluetooth.h"

#include "BtStackGlue.h"

// The glue provides stub definitions when CARSH_HAVE_BTSTACK is undefined, so
// these calls link unconditionally.
namespace carsh::device {

bool CircleBluetooth::init() { return btGlueInit(this); }

void CircleBluetooth::sendAvrcpCommand(carsh::bt::AvrcpCommand cmd) {
    btGlueSendAvrcp(int(cmd));
}

void CircleBluetooth::startScan() {
    scanning_ = true;
    btGlueStartScan();
}

void CircleBluetooth::stopScan() {
    scanning_ = false;
    btGlueStopScan();
}

void CircleBluetooth::connect(const carsh::bt::BdAddr& addr) {
    if (scanning_) stopScan();  // inquiry + paging can't run together (contract)
    btGlueConnect(addr.data());
}

void CircleBluetooth::disconnect() { btGlueDisconnect(); }

}  // namespace carsh::device
