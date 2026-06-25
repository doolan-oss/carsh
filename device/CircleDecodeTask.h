// CircleDecodeTask — the cooperative decode task (PLAN.md §1.3). It is the sole
// producer for the PcmRing: each slice tops up the ring via the PlayerController,
// then Yield()s so the UI/touch/BT tasks run. The I2S DMA pull callback consumes
// the ring independently. This is the "highest cooperative priority" citizen;
// yield granularity is tuned against real underrun behaviour at the bench (H4).
#ifndef _carsh_circle_decode_task_h
#define _carsh_circle_decode_task_h

#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>

#include "player/PlayerController.h"

namespace carsh::device {

class CircleDecodeTask : public CTask {
public:
    explicit CircleDecodeTask(carsh::player::PlayerController* player)
        : player_(player) {}

    void Run(void) override {
        for (;;) {
            player_->serviceDecode();          // keep the ring fed
            CScheduler::Get()->Yield();        // let UI/touch/BT run
        }
    }

private:
    carsh::player::PlayerController* player_;
};

}  // namespace carsh::device

#endif
