// Copyright (c) Microsoft Open Technologies, Inc. All rights reserved. See License.txt in the project root for license information.

#pragma once

#if !defined(RXCPP_RX_SUBSCRIPTION_HPP)
#define RXCPP_RX_SUBSCRIPTION_HPP

#include "rx-includes.hpp"

namespace rxcpp {

namespace detail {

template<class F>
struct is_unsubscribe_function
{
    struct not_void {};
    template<class CF>
    static auto check(int) -> decltype((*(CF*)nullptr)());
    template<class CF>
    static not_void check(...);

    static const bool value = std::is_same<decltype(check<typename std::decay<F>::type>(0)), void>::value;
};

}

struct tag_subscription {};
struct subscription_base {typedef tag_subscription subscription_tag;};
template<class T>
class is_subscription
{
    template<class C>
    static typename C::subscription_tag* check(int);
    template<class C>
    static void check(...);
public:
    static const bool value = std::is_convertible<decltype(check<typename std::decay<T>::type>(0)), tag_subscription*>::value;
};

template<class Unsubscribe>
class static_subscription
{
    typedef typename std::decay<Unsubscribe>::type unsubscribe_call_type;
    unsubscribe_call_type unsubscribe_call;
    static_subscription()
    {
    }
public:
    static_subscription(const static_subscription& o)
        : unsubscribe_call(o.unsubscribe_call)
    {
    }
    static_subscription(static_subscription&& o)
        : unsubscribe_call(std::move(o.unsubscribe_call))
    {
    }
    static_subscription(unsubscribe_call_type s)
        : unsubscribe_call(std::move(s))
    {
    }
    void unsubscribe() const {
        unsubscribe_call();
    }
};

class subscription : public subscription_base
{
    class base_subscription_state : public std::enable_shared_from_this<base_subscription_state>
    {
        base_subscription_state();
    public:

        explicit base_subscription_state(bool initial)
            : issubscribed(initial)
        {
        }
        virtual void unsubscribe() {
            issubscribed = false;
        }
        std::atomic<bool> issubscribed;
    };
public:
    typedef std::weak_ptr<base_subscription_state> weak_state_type;

private:
    template<class I>
    struct subscription_state : public base_subscription_state
    {
        typedef typename std::decay<I>::type inner_t;
        subscription_state(inner_t i)
            : base_subscription_state(true)
            , inner(std::move(i))
        {
        }
        virtual void unsubscribe() {
            if (issubscribed.exchange(false)) {
                inner.unsubscribe();
            }
        }
        inner_t inner;
    };
    std::shared_ptr<base_subscription_state> state;

    friend bool operator<(const subscription&, const subscription&);
    friend bool operator==(const subscription&, const subscription&);

    subscription(weak_state_type w)
        : state(w.lock())
    {
        if (!state) {
            abort();
        }
    }
public:

    subscription()
        : state(std::make_shared<base_subscription_state>(false))
    {
        if (!state) {
            abort();
        }
    }
    template<class U>
    explicit subscription(U u, typename std::enable_if<!is_subscription<U>::value, void**>::type = nullptr)
        : state(std::make_shared<subscription_state<U>>(std::move(u)))
    {
        if (!state) {
            abort();
        }
    }
    template<class U>
    explicit subscription(U u, typename std::enable_if<!std::is_same<subscription, U>::value && is_subscription<U>::value, void**>::type = nullptr)
        // intentionally slice
        : state(std::move((*static_cast<subscription*>(&u)).state))
    {
        if (!state) {
            abort();
        }
    }
    subscription(const subscription& o)
        : state(o.state)
    {
        if (!state) {
            abort();
        }
    }
    subscription(subscription&& o)
        : state(std::move(o.state))
    {
        if (!state) {
            abort();
        }
    }
    subscription& operator=(subscription o) {
        state = std::move(o.state);
        return *this;
    }
    bool is_subscribed() const {
        if (!state) {
            abort();
        }
        return state->issubscribed;
    }
    void unsubscribe() const {
        if (!state) {
            abort();
        }
        auto keepAlive = state;
        state->unsubscribe();
    }

    weak_state_type get_weak() {
        return state;
    }
    static subscription lock(weak_state_type w) {
        return subscription(w);
    }
};

inline bool operator<(const subscription& lhs, const subscription& rhs) {
    return lhs.state < rhs.state;
}
inline bool operator==(const subscription& lhs, const subscription& rhs) {
    return lhs.state == rhs.state;
}
inline bool operator!=(const subscription& lhs, const subscription& rhs) {
    return !(lhs == rhs);
}


inline auto make_subscription()
    ->      subscription {
    return  subscription();
}
template<class I>
auto make_subscription(I&& i)
    -> typename std::enable_if<!is_subscription<I>::value && !detail::is_unsubscribe_function<I>::value,
            subscription>::type {
    return  subscription(std::forward<I>(i));
}
template<class Unsubscribe>
auto make_subscription(Unsubscribe&& u)
    -> typename std::enable_if<detail::is_unsubscribe_function<Unsubscribe>::value,
            subscription>::type {
    return  subscription(static_subscription<Unsubscribe>(std::forward<Unsubscribe>(u)));
}

namespace detail {

struct tag_composite_subscription_empty {};

class composite_subscription_inner
{
private:
    typedef subscription::weak_state_type weak_subscription;
    struct composite_subscription_state : public std::enable_shared_from_this<composite_subscription_state>
    {
        std::set<subscription> subscriptions;
        std::mutex lock;
        std::atomic<bool> issubscribed;

        ~composite_subscription_state()
        {
            std::unique_lock<decltype(lock)> guard(lock);
            subscriptions.clear();
        }

        composite_subscription_state()
            : issubscribed(true)
        {
        }
        composite_subscription_state(tag_composite_subscription_empty)
            : issubscribed(false)
        {
        }

        inline weak_subscription add(subscription s) {
            if (!issubscribed) {
                s.unsubscribe();
            } else if (s.is_subscribed()) {
                std::unique_lock<decltype(lock)> guard(lock);
                subscriptions.insert(s);
            }
            return s.get_weak();
        }

        inline void remove(weak_subscription w) {
            if (issubscribed && !w.expired()) {
                std::unique_lock<decltype(lock)> guard(lock);
                subscriptions.erase(subscription::lock(w));
            }
        }

        inline void clear() {
            if (issubscribed) {
                std::unique_lock<decltype(lock)> guard(lock);

                std::set<subscription> v(std::move(subscriptions));
                guard.unlock();
                std::for_each(v.begin(), v.end(),
                              [](const subscription& s) {
                                s.unsubscribe(); });
            }
        }

        inline void unsubscribe() {
            if (issubscribed.exchange(false)) {
                std::unique_lock<decltype(lock)> guard(lock);

                std::set<subscription> v(std::move(subscriptions));
                guard.unlock();
                std::for_each(v.begin(), v.end(),
                              [](const subscription& s) {
                                s.unsubscribe(); });
            }
        }
    };

public:
    typedef std::shared_ptr<composite_subscription_state> shared_state_type;

protected:
    mutable shared_state_type state;

public:
    composite_subscription_inner()
        : state(std::make_shared<composite_subscription_state>())
    {
    }
    composite_subscription_inner(tag_composite_subscription_empty et)
        : state(std::make_shared<composite_subscription_state>(et))
    {
    }

    composite_subscription_inner(const composite_subscription_inner& o)
        : state(o.state)
    {
        if (!state) {
            abort();
        }
    }
    composite_subscription_inner(composite_subscription_inner&& o)
        : state(std::move(o.state))
    {
        if (!state) {
            abort();
        }
    }

    composite_subscription_inner& operator=(composite_subscription_inner o)
    {
        state = std::move(o.state);
        if (!state) {
            abort();
        }
        return *this;
    }

    inline weak_subscription add(subscription s) const {
        if (!state) {
            abort();
        }
        return state->add(std::move(s));
    }
    inline void remove(weak_subscription w) const {
        if (!state) {
            abort();
        }
        state->remove(std::move(w));
    }
    inline void clear() const {
        if (!state) {
            abort();
        }
        state->clear();
    }
    inline void unsubscribe() {
        if (!state) {
            abort();
        }
        state->unsubscribe();
    }
};

}

class composite_subscription
    : protected detail::composite_subscription_inner
    , public subscription
{
    typedef detail::composite_subscription_inner inner_type;
public:
    typedef subscription::weak_state_type weak_subscription;

    static composite_subscription shared_empty;

    composite_subscription(detail::tag_composite_subscription_empty et)
        : inner_type(et)
        , subscription() // use empty base
    {
    }

public:

    composite_subscription()
        : inner_type()
        , subscription(*static_cast<const inner_type* const>(this))
    {
    }

    composite_subscription(const composite_subscription& o)
        : inner_type(o)
        , subscription(static_cast<const subscription&>(o))
    {
    }
    composite_subscription(composite_subscription&& o)
        : inner_type(std::move(o))
        , subscription(std::move(static_cast<subscription&>(o)))
    {
    }

    composite_subscription& operator=(composite_subscription o)
    {
        inner_type::operator=(std::move(o));
        subscription::operator=(std::move(*static_cast<subscription*>(&o)));
        return *this;
    }

    static inline composite_subscription empty() {
        return shared_empty;
    }

    using subscription::is_subscribed;
    using subscription::unsubscribe;

    using inner_type::remove;
    using inner_type::clear;

    inline weak_subscription add(subscription s) const {
        if (s == static_cast<const subscription&>(*this)) {
            // do not nest the same subscription
            abort();
            //return s.get_weak();
        }
        return inner_type::add(std::move(s));
    }

    template<class F>
    auto add(F f) const
    -> typename std::enable_if<detail::is_unsubscribe_function<F>::value, weak_subscription>::type {
        return add(make_subscription(std::move(f)));
    }
};

inline bool operator<(const composite_subscription& lhs, const composite_subscription& rhs) {
    return static_cast<const subscription&>(lhs) < static_cast<const subscription&>(rhs);
}
inline bool operator==(const composite_subscription& lhs, const composite_subscription& rhs) {
    return static_cast<const subscription&>(lhs) == static_cast<const subscription&>(rhs);
}
inline bool operator!=(const composite_subscription& lhs, const composite_subscription& rhs) {
    return !(lhs == rhs);
}

//static
RXCPP_SELECT_ANY composite_subscription composite_subscription::shared_empty = composite_subscription(detail::tag_composite_subscription_empty());

}

#endif
