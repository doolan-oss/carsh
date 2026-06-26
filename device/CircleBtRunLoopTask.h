// CircleBtRunLoopTask — pumps the BTstack run loop as a cooperative CTask
// (PLAN.md §1.3 "BTstack run loop"). Each pass services HCI/L2CAP/transport RX,
// then Yield()s so audio/UI keep running. Only spawned when CircleBluetooth::init
// succeeds (i.e. BTstack is compiled in and HCI came up).
#ifndef _carsh_circle_bt_runloop_task_h
#define _carsh_circle_bt_runloop_task_h

#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>

#include "BtStackGlue.h"

namespace carsh::device {

class CircleBtRunLoopTask : public CTask {
public:
    void Run(void) override {
        for (;;) {
            btGlueProcess();              // one run-loop / transport pass
            CScheduler::Get()->Yield();
        }
    }
};

}  // namespace carsh::device

#endif
