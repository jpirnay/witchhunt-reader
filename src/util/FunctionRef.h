#pragma once

#include <memory>
#include <type_traits>
#include <utility>

// A non-owning reference to a callable: an object pointer and a call thunk, never a heap block.
//
// For callbacks that run before the call receiving them returns and are never stored. A
// std::function in that place copies the callable into itself, and on the ESP32-C3 its inline
// buffer holds 8 bytes -- a lambda with three captures already goes to the heap. On a path that
// runs every loop pass (ButtonNavigator, on every list screen) that was a malloc/free pair per
// tick, pressed or not. Pointing at the caller's callable costs nothing, and it is safe exactly
// because the callable outlives the call: a lambda written as an argument lives until the end of
// the full-expression.
//
// So never store one, and never build one from a temporary in a declaration: it would dangle.
//
// One non-template function body serves every callable; each callable type adds only its thunk,
// a few instructions. Taking the callable as a template parameter instead would compile a copy of
// the whole receiving function per lambda.
template <typename Signature>
class FunctionRef;

template <typename R, typename... Args>
class FunctionRef<R(Args...)> {
 public:
  template <typename F>
    requires(!std::is_same_v<std::remove_cvref_t<F>, FunctionRef> && std::is_invocable_r_v<R, F&, Args...>)
  // cppcheck-suppress noExplicitConstructor ; a lambda argument must convert implicitly
  FunctionRef(F&& callable) noexcept
      : object(const_cast<void*>(static_cast<const void*>(std::addressof(callable)))),
        thunk([](void* target, Args... args) -> R {
          return (*static_cast<std::remove_reference_t<F>*>(target))(std::forward<Args>(args)...);
        }) {}

  R operator()(Args... args) const { return thunk(object, std::forward<Args>(args)...); }

 private:
  void* object;
  R (*thunk)(void*, Args...);
};
