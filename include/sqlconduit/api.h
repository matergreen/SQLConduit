#ifndef SQLCONDUIT_API_H
#define SQLCONDUIT_API_H

// Public deprecation marker. APIs remain available for at least one minor
// release after first being marked with this macro.
#if defined(__has_cpp_attribute)
#  if __has_cpp_attribute(deprecated)
#    define SQLCONDUIT_DEPRECATED(message) [[deprecated(message)]]
#  endif
#endif

#if !defined(SQLCONDUIT_DEPRECATED)
#  if defined(_MSC_VER)
#    define SQLCONDUIT_DEPRECATED(message) __declspec(deprecated(message))
#  elif defined(__GNUC__) || defined(__clang__)
#    define SQLCONDUIT_DEPRECATED(message) __attribute__((deprecated(message)))
#  else
#    define SQLCONDUIT_DEPRECATED(message)
#  endif
#endif

#endif
