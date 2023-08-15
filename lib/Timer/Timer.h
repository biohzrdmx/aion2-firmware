#ifndef TIMER_h
#define TIMER_h

class Timer {
  public:
    Timer();
    long update();
    void restart();
    void init(long duration);
    bool hasFinished();
  private:
    long _start;
    long _duration;
    bool _finished;
};

#endif
