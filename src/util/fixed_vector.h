#pragma once

#include "defines.h"

#include <iterator>
#include <type_traits>
#include <initializer_list>
#include <utility>

template <typename T, size_t CAPACITY>
struct Fixed_Vector
{
private:
    using StorageType = typename std::aligned_storage<sizeof(T), alignof(T)>::type;

public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = T*;
    using const_iterator = const T*;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    Fixed_Vector() :
        m_size(0)
    {
    }

    Fixed_Vector(std::initializer_list<T> ilist) :
        Fixed_Vector()
    {
        for (const auto& v : ilist)
            emplace_back(v);
    }

    explicit Fixed_Vector(size_type s) :
        m_size(s)
    {
        ASSERT(s <= CAPACITY);

        for (size_type i = 0; i < s; ++i)
            new (&(m_data[i])) T;
    }

    Fixed_Vector(size_type s, const T& v) :
        m_size(s)
    {
        ASSERT(s <= CAPACITY);

        for (size_type i = 0; i < s; ++i)
            new (&(m_data[i])) T(v);
    }

    Fixed_Vector(const Fixed_Vector& other) :
        m_size(other.m_size)
    {
        for (size_type i = 0; i < m_size; ++i)
            new (&(m_data[i])) T(other[i]);
    }

    Fixed_Vector(Fixed_Vector&& other) noexcept :
        m_size(other.m_size)
    {
        for (size_type i = 0; i < m_size; ++i)
            new (&(m_data[i])) T(std::move(other[i]));
    }

    ~Fixed_Vector()
    {
        clear();
    }

    Fixed_Vector& operator=(const Fixed_Vector& other)
    {
        size_type i = 0;
        for (; i < m_size && i < other.m_size; ++i)
            (*this)[i] = other[i];

        for (; i < m_size; ++i)
            (*this)[i].~T();

        for (; i < other.m_size; ++i)
            new (&(m_data[i])) T(other[i]);

        m_size = other.m_size;

        return *this;
    }

    Fixed_Vector& operator=(Fixed_Vector&& other) noexcept
    {
        size_type i = 0;
        for (; i < m_size && i < other.m_size; ++i)
            (*this)[i] = std::move(other[i]);

        for (; i < m_size; ++i)
            (*this)[i].~T();

        for (; i < other.m_size; ++i)
            new (&(m_data[i])) T(std::move(other[i]));

        m_size = other.m_size;

        return *this;
    }

    void resize(size_type newSize)
    {
        ASSERT(newSize <= CAPACITY);

        size_type i = newSize < m_size ? newSize : m_size;

        for (; i < m_size; ++i)
            (*this)[i].~T();

        for (; i < newSize; ++i)
            new (&(m_data[i])) T;

        m_size = newSize;
    }

    NODISCARD pointer data()
    {
        return reinterpret_cast<T*>(&(m_data[0]));
    }

    NODISCARD const_pointer data() const
    {
        return reinterpret_cast<const T*>(&(m_data[0]));
    }

    NODISCARD reference operator[](size_type i)
    {
        ASSERT(i < m_size);
        return data()[i];
    }

    NODISCARD const_reference operator[](size_type i) const
    {
        ASSERT(i < m_size);
        return data()[i];
    }

    NODISCARD reference at(size_type i)
    {
        if (i >= m_size)
            throw std::out_of_range("");
        return (data()[i]);
    }

    NODISCARD const_reference at(size_type i) const
    {
        if (i >= m_size)
            throw std::out_of_range("");
        return (data()[i]);
    }

    NODISCARD reference front()
    {
        ASSERT(m_size > 0);
        return (*this)[0];
    }

    NODISCARD const_reference front() const
    {
        ASSERT(m_size > 0);
        return (*this)[0];
    }

    NODISCARD reference back()
    {
        ASSERT(m_size > 0);
        return (*this)[m_size - 1];
    }

    NODISCARD const_reference back() const
    {
        ASSERT(m_size > 0);
        return (*this)[m_size - 1];
    }

    NODISCARD iterator begin()
    {
        return data();
    }

    NODISCARD iterator end()
    {
        return data() + m_size;
    }

    NODISCARD const_iterator begin() const
    {
        return data();
    }

    NODISCARD const_iterator end() const
    {
        return data() + m_size;
    }

    NODISCARD const_iterator cbegin() const
    {
        return data();
    }

    NODISCARD const_iterator cend() const
    {
        return data() + m_size;
    }

    NODISCARD reverse_iterator rbegin()
    {
        return reverse_iterator(end());
    }

    NODISCARD reverse_iterator rend()
    {
        return reverse_iterator(begin());
    }

    NODISCARD const_reverse_iterator rbegin() const
    {
        return const_reverse_iterator(end());
    }

    NODISCARD const_reverse_iterator rend() const
    {
        return const_reverse_iterator(begin());
    }

    NODISCARD const_reverse_iterator crbegin() const
    {
        return const_reverse_iterator(cend());
    }

    NODISCARD const_reverse_iterator crend() const
    {
        return const_reverse_iterator(cbegin());
    }

    NODISCARD bool empty() const
    {
        return m_size == 0;
    }

    NODISCARD size_type size() const
    {
        return m_size;
    }

    NODISCARD size_type capacity() const
    {
        return CAPACITY;
    }

    void reserve(size_type n)
    {
        // do nothing
        // it's for interface compatibility
    }

    void clear()
    {
        for (size_type i = 0; i < m_size; ++i)
            (*this)[i].~T();
        m_size = 0;
    }

    template<typename... ArgsTs>
    reference emplace_back(ArgsTs&& ... args)
    {
        ASSERT(m_size < CAPACITY);

        new (&(m_data[m_size])) T(std::forward<ArgsTs>(args)...);
        ++m_size;
        return back();
    }

    reference push_back(const T& value)
    {
        ASSERT(m_size < CAPACITY);

        new (&(m_data[m_size])) T(value);
        ++m_size;
        return back();
    }

    reference push_back(T&& value)
    {
        ASSERT(m_size < CAPACITY);

        new (&(m_data[m_size])) T(std::move(value));
        ++m_size;
        return back();
    }

    void pop_back()
    {
        ASSERT(m_size > 0);

        --m_size;
        (*this)[m_size].~T();
    }

private:
    size_type m_size;
    StorageType m_data[CAPACITY];
};
