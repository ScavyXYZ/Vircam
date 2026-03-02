#include <chrono>
#include <algorithm>

class Timer {
public:
    Timer();

    // Main functions

    double getTime() const;          // Current time in seconds
    void start();                    // Start the timer
    void pause();                    // Pause the timer
    void reset();                    // Reset the timer
    bool toggle();                   // Toggle start/pause

    // Additional functions
    
    // Adjust time by adding seconds
    void adjustTime(double seconds);
    
    // Set specific time
    void setTime(double seconds);    

private:
    bool isRun;
    std::chrono::time_point<std::chrono::steady_clock> startTime;
    double currentTime;
    double pausedTime;
};