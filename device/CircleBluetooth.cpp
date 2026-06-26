#include "CircleBluetooth.h"

#include "BtStackGlue.h"

// The glue provides stub definitions when CARSH_HAVE_BTSTACK is undefined, so
// these calls link unconditionally.
namespace carsh::device {

bool CircleBluetooth::init() { return btGlueInit(this); }

void CircleBluetooth::sendAvrcpCommand(carsh::bt::AvrcpCommand cmd) {
    btGlueSendAvrcp(int(cmd));
}

}  // namespace carsh::device
