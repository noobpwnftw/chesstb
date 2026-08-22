#pragma once

#include "defines.h"
#include "utility.h"

#include <chrono>
#include <string>
#include <mutex>
#include <atomic>

struct Concurrent_Progress_Bar
{
	Concurrent_Progress_Bar(size_t end, size_t print_period, std::string task_name = "") :
		m_start_time(std::chrono::steady_clock::now()),
		m_current(0),
		m_end(end),
		m_last_print(0),
		m_print_period(print_period),
		m_last_print_length(0),
		m_prefix(std::move(task_name))
	{
		if (!m_prefix.empty())
			m_prefix += ": ";

		m_last_print_length = printf("\r%s0%%; 0/%zd",
			m_prefix.c_str(),
			m_end
		);
	}

	Concurrent_Progress_Bar& operator+=(size_t n)
	{
		const size_t curr = m_current.fetch_add(n) + n;

		// Since last_print can only increase we can do an early check
		// before the critical section.
		if (curr < m_last_print.load() + m_print_period)
			return *this;

		std::unique_lock<std::mutex> lock(m_mutex);

		if (curr < m_last_print.load() + m_print_period)
			return *this;

		const auto now = std::chrono::steady_clock::now();
		const double time_use_s = elapsed_seconds(m_start_time, now);
		const double nps = static_cast<double>(curr) / time_use_s;
		const auto expected_end_time = now + std::chrono::seconds(static_cast<size_t>((m_end - curr) / nps) + 1);
		const double pct = static_cast<double>(m_current) / m_end * 100.0;

		if (m_last_print_length)
			printf("\r%*c", m_last_print_length, ' ');

		m_last_print_length = printf("\r%s%03.2f%%; %zd/%zd; %.3git/s; <%s",
			m_prefix.c_str(),
			pct,
			curr, m_end,
			nps,
			format_elapsed_time(now, expected_end_time).c_str()
		);

		fflush(stdout);

		m_last_print.store(curr);

		return *this;
	}

	void set_finished()
	{
		std::unique_lock<std::mutex> lock(m_mutex);

		const auto now = std::chrono::steady_clock::now();
		const double time_use_s = elapsed_seconds(m_start_time, now);
		const double nps = static_cast<double>(m_end) / time_use_s;

		if (m_last_print_length)
			printf("\r%*c", m_last_print_length, ' ');

		m_last_print_length = printf("\r%s100.00%%; %zd/%zd; %.3git/s; %s\n",
			m_prefix.c_str(),
			m_end, m_end,
			nps,
			format_elapsed_time(m_start_time, now).c_str()
		);

		m_current.store(m_end);
		m_last_print.store(m_end);
	}

private:
	mutable std::mutex m_mutex;
	std::chrono::steady_clock::time_point m_start_time;
	std::atomic<size_t> m_current;
	size_t m_end;
	std::atomic<size_t> m_last_print;
	size_t m_print_period;
	int m_last_print_length;
	std::string m_prefix;
};