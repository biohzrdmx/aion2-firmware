#include <Arduino.h>
#include "Timer.h"

Timer::Timer() {
  _finished = false;
}

void Timer::init(long duration) {
    _start = millis();
    _duration = duration;
    _finished = false;
}

long Timer::update() {
  long elapsed;
  long delta;
  long ret = 0;
  if (_duration) {
    elapsed = millis() - _start;
    delta = (elapsed * 100) / _duration;
    if (!_finished) {
      if (delta >= 100) {
        _finished = true;
      }
    }
    ret = elapsed;
  }
  return ret;
}

void Timer::restart() {
  _start = millis();
  _finished = false;
}

bool Timer::hasFinished() {
  return _finished;
}
