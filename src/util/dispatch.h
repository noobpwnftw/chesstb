#pragma once

#include <tuple>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

template <typename T, T... NextValuesVs>
struct Template_Dispatch {
    using value_type = T;

    Template_Dispatch() = delete;

    Template_Dispatch(T value) :
        value(value)
    {
    }

    T value;
};

struct Template_Dispatcher {
private:
    template <
        typename... DispatcherTs, 
        typename... DispatchedTs, 
        typename T, 
        T CurrValueV, 
        T... NextValuesVs, 
        typename F
    >
    decltype(auto) dispatch(
        const std::tuple<DispatcherTs...>& all_dispatchers, 
        const std::tuple<DispatchedTs...>& dispatched_values, 
        const Template_Dispatch<T, CurrValueV, NextValuesVs...>& curr_dispatcher, 
        F&& continuation
    )
    {
        if (curr_dispatcher.value == CurrValueV)
        {
            return dispatch(
                all_dispatchers,
                std::tuple_cat(dispatched_values, std::make_tuple(std::integral_constant<T, CurrValueV>{})),
                std::forward<F>(continuation)
            );
        }

        if constexpr (sizeof...(NextValuesVs) != 0)
        {
            return dispatch(
                all_dispatchers,
                dispatched_values,
                Template_Dispatch<T, NextValuesVs...>{curr_dispatcher.value},
                std::forward<F>(continuation)
            );
        }
        else
        {
            throw std::logic_error("Invalid template parameter.");
        }
    }

    template <
        typename... DispatcherTs, 
        typename... DispatchedTs, 
        typename F
    >
    decltype(auto) dispatch(
        const std::tuple<DispatcherTs...>& all_dispatchers, 
        const std::tuple<DispatchedTs...>& dispatched_values, 
        F&& continuation
    )
    {
        if constexpr (sizeof...(DispatcherTs) == sizeof...(DispatchedTs))
        {
            return std::apply(std::forward<F>(continuation), dispatched_values);
        }
        else
        {
            return dispatch(
                all_dispatchers, 
                dispatched_values, 
                std::get<sizeof...(DispatchedTs)>(all_dispatchers), 
                std::forward<F>(continuation)
            );
        }
    }

public:
    template <typename... DispatcherTs, typename F>
    decltype(auto) operator()(const std::tuple<DispatcherTs...>& all_dispatchers, F&& continuation)
    {
        return dispatch(all_dispatchers, std::tuple<>{}, std::forward<F>(continuation));
    }

    template <typename DispatcherT, typename F>
    decltype(auto) operator()(const DispatcherT& single_dispatcher, F&& continuation)
    {
        return dispatch(std::make_tuple(single_dispatcher), std::tuple<>{}, std::forward<F>(continuation));
    }

} static template_dispatch;

#define TEMPLATE_DISPATCH(all_dispatchers, func, ...) template_dispatch(all_dispatchers, [&](auto... tp_args__) { return func<decltype(tp_args__)::value...>(__VA_ARGS__); })
