#include "Timer.h"

Timer::Timer() :
    isRun(false),
    currentTime(0.0),
    pausedTime(0.0)
{
}

double Timer::getTime() const {
    if (isRun) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - startTime).count();
        return std::max(currentTime + elapsed, 0.0);
    }
    return std::max(currentTime, 0.0);
}

void Timer::start() {
    if (!isRun) {
        startTime = std::chrono::steady_clock::now();
        isRun = true;
    }
}

void Timer::pause() {
    if (isRun) {
        auto now = std::chrono::steady_clock::now();
        currentTime += std::chrono::duration_cast<std::chrono::duration<double>>(now - startTime).count();
        isRun = false;
    }
}

void Timer::reset() {
    isRun = false;
    currentTime = 0.0;
    pausedTime = 0.0;
}

bool Timer::toggle() {
    isRun ? pause() : start();
    return isRun;
}

void Timer::adjustTime(double seconds) {
    if (isRun) {
        auto now = std::chrono::steady_clock::now();
        currentTime += std::chrono::duration_cast<std::chrono::duration<double>>(now - startTime).count();
        currentTime = std::max(0.0, currentTime + seconds);
        startTime = now;
    }
    else {
        currentTime = std::max(0.0, currentTime + seconds);
    }
}

void Timer::setTime(double seconds) {
    currentTime = std::max(0.0, seconds);
    pausedTime = 0.0;

    if (isRun) {
        startTime = std::chrono::steady_clock::now();
    }
}