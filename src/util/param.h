#pragma once

#include "defines.h"

#include <type_traits>
#include <utility>

template <typename T>
struct Has_Subscript_Operator_Helper
{
private:
	using Yes = char;
	using No = Yes[2];

	template<typename C> static constexpr auto test(void*)
		-> decltype(std::declval<C>()[std::declval<size_t>()], Yes{});

	template<typename> static constexpr No& test(...);

public:
	static constexpr bool value = sizeof(test<T>(0)) == sizeof(Yes);
};

template <typename T>
constexpr bool Has_Subscript_Operator = Has_Subscript_Operator_Helper<T>::value;

template <typename T, template <typename> typename ChildT>
struct Param_Base
{
	template <typename U = T>
	NODISCARD std::enable_if_t<Has_Subscript_Operator<U>, decltype(std::declval<U>()[0])&> operator[](size_t i)
	{
		return (*m_ptr)[i];
	}

	template <typename U = T>
	NODISCARD std::enable_if_t<Has_Subscript_Operator<const U>, const decltype(std::declval<U>()[0])&> operator[](size_t i) const
	{
		return (*m_ptr)[i];
	}

	NODISCARD T& operator*() { return *m_ptr; }
	NODISCARD const T& operator*() const { return *m_ptr; }

	NODISCARD T* operator->() { return m_ptr; }
	NODISCARD const T* operator->() const { return m_ptr; }

protected:
	T* m_ptr;

	explicit Param_Base(T* v) :
		m_ptr(v)
	{
	}
};

template <typename T>
struct Out_Param;

template <typename T>
struct Optional_Out_Param final : public Param_Base<T, Optional_Out_Param>
{
private:
	using Base_Type = Param_Base<T, ::Optional_Out_Param>;

public:
	Optional_Out_Param() :
		Base_Type(nullptr)
	{
	}

	explicit Optional_Out_Param(T& v) :
		Base_Type(&v)
	{
	}

	NODISCARD Out_Param<T> as_mandatory() const;

	NODISCARD operator bool() const { return Base_Type::m_ptr != nullptr; }
};

template <typename T>
struct Out_Param final : public Param_Base<T, Out_Param>
{
private:
	using Base_Type = Param_Base<T, ::Out_Param>;

public:
	explicit Out_Param(T& v) :
		Base_Type(&v)
	{
	}

	NODISCARD operator Optional_Out_Param<T>() const { return Optional_Out_Param<T>(*Base_Type::m_ptr); }
};

template <typename T>
Out_Param<T> Optional_Out_Param<T>::as_mandatory() const
{
	return Out_Param<T>(*Base_Type::m_ptr);
}

template <typename T>
Out_Param<T> out_param(T& v)
{
	return Out_Param<T>(v);
}

template <typename T>
Optional_Out_Param<T> out_param()
{
	return Optional_Out_Param<T>();
}

template <typename T>
struct In_Out_Param;

template <typename T>
struct Optional_In_Out_Param final : public Param_Base<T, Optional_In_Out_Param>
{
private:
	using Base_Type = Param_Base<T, ::Optional_In_Out_Param>;

public:
	Optional_In_Out_Param() :
		Base_Type(nullptr)
	{
	}

	explicit Optional_In_Out_Param(T& v) :
		Base_Type(&v)
	{
	}

	NODISCARD In_Out_Param<T> as_mandatory() const;
	NODISCARD Optional_Out_Param<T> as_out_param() const { return Optional_Out_Param<T>(*Base_Type::m_ptr); }

	NODISCARD operator bool() const { return Base_Type::m_ptr != nullptr; }
};

template <typename T>
struct In_Out_Param final : public Param_Base<T, In_Out_Param>
{
private:
	using Base_Type = Param_Base<T, ::In_Out_Param>;

public:
	explicit In_Out_Param(T& v) :
		Base_Type(&v)
	{
	}

	NODISCARD Out_Param<T> as_out_param() const { return Out_Param<T>(*Base_Type::m_ptr); }

	NODISCARD operator Optional_In_Out_Param<T>() const { return Optional_In_Out_Param<T>(*Base_Type::m_ptr); }
};

template <typename T>
NODISCARD In_Out_Param<T> Optional_In_Out_Param<T>::as_mandatory() const
{
	return In_Out_Param<T>(*Base_Type::m_ptr);
}


template <typename T>
NODISCARD In_Out_Param<T> inout_param(T& v)
{
	return In_Out_Param<T>(v);
}

template <typename T>
NODISCARD Optional_In_Out_Param<T> inout_param()
{
	return Optional_In_Out_Param<T>();
}
