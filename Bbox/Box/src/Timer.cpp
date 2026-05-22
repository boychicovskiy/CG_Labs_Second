#include "Timer.hpp"

Timer::Timer() {
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	m_secondsPerCount = 1.0 / static_cast<double>(freq.QuadPart);

	Reset();
}

double Timer::TotalTime() const {
	int64_t effectiveTime = m_stopped ? m_stopTime : m_currTime;

	return static_cast<double>((effectiveTime - m_pausedTime) - m_baseTime) * m_secondsPerCount;
}

double Timer::DeltaTime() const {
	return m_deltaTime;
}

void Timer::Reset() {
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);

	m_baseTime = t.QuadPart;
	m_prevTime = t.QuadPart;
	m_stopTime = 0;
	m_pausedTime = 0;
	m_stopped = false;

	m_currTime = t.QuadPart;
	m_deltaTime = 0.0;
}

void Timer::Start() {
	if (!m_stopped)
		return;

	LARGE_INTEGER start;
	QueryPerformanceCounter(&start);

	m_pausedTime += (start.QuadPart - m_stopTime);

	m_prevTime = start.QuadPart;
	m_stopTime = 0;
	m_stopped = false;
}

void Timer::Stop() {
	if (m_stopped)
		return;

	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);

	m_stopTime = t.QuadPart;
	m_stopped = true;
}

void Timer::Tick() {
	if (m_stopped) {
		m_deltaTime = 0.0;
		return;
	}

	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	m_currTime = t.QuadPart;

	m_deltaTime = static_cast<double>(m_currTime - m_prevTime) * m_secondsPerCount;
	m_prevTime = m_currTime;

	if (m_deltaTime < 0.0)
		m_deltaTime = 0.0;
}