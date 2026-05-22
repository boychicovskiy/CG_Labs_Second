#ifndef TIMER_HPP
#define TIMER_HPP

#include <cstdint>
#include <Windows.h>

class Timer {
public:
	Timer();

	double TotalTime() const;
	double DeltaTime() const;

	void Reset();
	void Start();
	void Stop();
	void Tick();

private:
	double m_secondsPerCount = 0.0;
	double m_deltaTime = 0.0;

	int64_t m_baseTime = 0;
	int64_t m_pausedTime = 0;
	int64_t m_stopTime = 0;
	int64_t m_prevTime = 0;
	int64_t m_currTime = 0;

	bool m_stopped = false;
};

#endif // TIMER_HPP
