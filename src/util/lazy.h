#pragma once

#include <utility>
#include <type_traits>

template <typename FuncT>
struct Lazy_Cached_Value : FuncT
{
	using Value_Type = decltype(std::declval<FuncT>()());

	Lazy_Cached_Value(FuncT&& f) :
		FuncT(std::move(f)),
		m_is_initialized(false)
	{
	}

	~Lazy_Cached_Value()
	{
		if (m_is_initialized)
		{
			reinterpret_cast<Value_Type&>(m_value).~Value_Type();
		}
	}

	const Value_Type& operator*() const
	{
		ensure_initialized();
		return reinterpret_cast<const Value_Type&>(m_value);
	}

	Value_Type& operator*()
	{
		ensure_initialized();
		return reinterpret_cast<Value_Type&>(m_value);
	}

	const Value_Type* operator->() const
	{
		ensure_initialized();
		return reinterpret_cast<const Value_Type*>(&m_value);
	}

	Value_Type* operator->()
	{
		ensure_initialized();
		return reinterpret_cast<Value_Type*>(&m_value);
	}

	const Value_Type& value() const
	{
		ensure_initialized();
		return reinterpret_cast<const Value_Type&>(m_value);
	}

	Value_Type& value()
	{
		ensure_initialized();
		return reinterpret_cast<Value_Type&>(m_value);
	}

	operator const Value_Type& () const
	{
		ensure_initialized();
		return reinterpret_cast<const Value_Type&>(m_value);
	}

	operator Value_Type& ()
	{
		ensure_initialized();
		return reinterpret_cast<Value_Type&>(m_value);
	}

private:
	mutable std::aligned_storage_t<sizeof(Value_Type), alignof(Value_Type)> m_value;
	mutable bool m_is_initialized;

	void ensure_initialized() const
	{
		if (!m_is_initialized)
		{
			new (reinterpret_cast<Value_Type*>(&m_value)) Value_Type(FuncT::operator()());
			m_is_initialized = true;
		}
	}
};