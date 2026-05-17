#pragma once

#include "defines.h"
#include "utility.h"

#include <atomic>
#include <thread>
#include <vector>
#include <deque>
#include <type_traits>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <future>
#include <memory>

struct Thread_Pool
{
	explicit Thread_Pool(size_t num_threads)
	{
		for (size_t i = 0; i < num_threads; ++i)
			m_workers.emplace_back(i);
	}

	template <typename F>
	auto run_async_task_on_thread(F&& job, size_t i)
	{
		using RetT = decltype(job());
		auto promise = std::make_shared<std::promise<RetT>>();
		std::future<RetT> future = promise->get_future();
		m_workers[i].enqueue_task([job = std::forward<F>(job), promise = std::move(promise)]() mutable {
			if constexpr (std::is_same_v<RetT, void>)
			{
				job();
				promise->set_value();
			}
			else
			{
				promise->set_value(job());
			}
		});
		return future;
	}

	template <typename F>
	NODISCARD auto run_async_task_on_all_threads(F&& job)
	{
		return run_async_task_on_multiple_threads(m_workers.size(), std::forward<F>(job));
	}

	template <typename F>
	NODISCARD auto run_async_task_on_multiple_threads(size_t thread_use, F&& job)
	{
		using T = decltype(job(std::declval<size_t>()));

		ASSERT(thread_use <= m_workers.size());

		std::vector<std::future<T>> futures;
		futures.reserve(thread_use);

		for (size_t i = 0; i < thread_use; ++i)
			futures.emplace_back(run_async_task_on_thread([i, job]() { return job(i); }, i));

		return futures;
	}

	template <typename F>
	NODISCARD auto run_sync_task_on_all_threads(F&& job)
	{
		return run_sync_task_on_multiple_threads(m_workers.size(), std::forward<F>(job));
	}

	template <typename F>
	NODISCARD auto run_sync_task_on_multiple_threads(size_t thread_use, F&& job) ->
		std::enable_if_t<
			!std::is_same_v<
				decltype(job(std::declval<size_t>())),
				void
			>,
			Vector_Not_Bool<decltype(job(std::declval<size_t>()))>
		>
	{
		auto futures = run_async_task_on_multiple_threads(thread_use, std::forward<F>(job));

		// Avoid vector of bool, because it's unsafe for parallel writes.
		Vector_Not_Bool<decltype(job(std::declval<size_t>()))> ret;
		ret.reserve(thread_use);
		for (auto&& future : futures)
			ret.emplace_back(future.get());

		return ret;
	}

	template <typename F>
	auto run_sync_task_on_multiple_threads(size_t thread_use, F&& job) ->
		std::enable_if_t<
			std::is_same_v<
				decltype(job(std::declval<size_t>())),
				void
			>
		>
	{
		auto futures = run_async_task_on_multiple_threads(thread_use, std::forward<F>(job));

		for (auto&& future : futures)
			future.wait();
	}

	NODISCARD size_t num_workers() const
	{
		return m_workers.size();
	}

private:

	struct Worker_Thread
	{
		explicit Worker_Thread(size_t index) :
			m_thread_id(index)
		{
			m_quit.store(false);
			m_thread = std::thread([this]() { thread_entry(); });
		}

		Worker_Thread(const Worker_Thread&) = delete;
		Worker_Thread(Worker_Thread&&) noexcept = delete;

		Worker_Thread& operator=(const Worker_Thread&) = delete;
		Worker_Thread& operator=(Worker_Thread&&) noexcept = delete;

		~Worker_Thread()
		{
			std::unique_lock<std::mutex> lock(m_mutex);

			m_quit = true;
			m_task_or_quit.notify_one();

			m_no_tasks.wait(lock, [this]() { return m_tasks.empty(); });

			lock.unlock();

			m_thread.join();
		}

		template <typename F>
		void enqueue_task(F&& j)
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_tasks.emplace_back(std::forward<F>(j));
			m_task_or_quit.notify_one();
		}

		void thread_entry()
		{
			for(;;)
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_task_or_quit.wait(lock, [this]() { return m_quit || !m_tasks.empty(); });

				if (m_quit && m_tasks.empty())
					break;

				auto job = std::move(m_tasks.front());
				m_tasks.pop_front();

				if (m_tasks.empty())
					m_no_tasks.notify_one();

				lock.unlock();

				job();
			}
		}

	private:
		std::thread m_thread;
		size_t m_thread_id;

		mutable std::condition_variable m_task_or_quit;
		mutable std::condition_variable m_no_tasks;
		mutable std::mutex m_mutex;

		// These are rarely accessed so don't bother with putting them on a separate cacheline.
		std::deque<std::function<void()>> m_tasks;
		std::atomic<bool> m_quit;
	};

	std::deque<Worker_Thread> m_workers;
};
