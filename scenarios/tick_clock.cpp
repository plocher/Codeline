#include "codeline/core/TickClock.h"
#include "scenario.h"

using codeline::ElapsedMillis;
using codeline::TickClock;

SCENARIO("a timer counts only when the clock is advanced") {
  TickClock clock;
  ElapsedMillis timer(clock);
  CHECK(timer == 0u);
  clock.advanceTo(1500);
  CHECK(timer == 1500u);
  timer = 0;
  CHECK(timer == 0u);
  clock.advanceTo(1750);
  CHECK(timer == 250u);
}

SCENARIO("a timer survives uint32 clock rollover") {
  TickClock clock;
  clock.advanceTo(0xFFFFFF00u);
  ElapsedMillis timer(clock);
  clock.advanceTo(0x00000100u);  // 0x200 ms later, across the wrap
  CHECK(timer == 0x200u);
}

SCENARIO("subtracting a period keeps a flasher drift-free") {
  TickClock clock;
  ElapsedMillis flasher(clock);
  clock.advanceTo(520);  // tick arrived 20 ms late for a 500 ms half period
  CHECK(flasher >= 500u);
  flasher -= 500;
  CHECK(flasher == 20u);  // the lateness is carried, not lost
}

int main() { return codeline::scenario::run(); }
