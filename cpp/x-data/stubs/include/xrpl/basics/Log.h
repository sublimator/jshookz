// Stub Log.h — replaces the real one to kill the dependency chain:
// Log.h → UnorderedContainers.h → hardened_hash.h → xxhasher.h → xxhash.h
//
// The real Log.h pulls in half the beast library. All we need is:
// - JLOG macro (no-op)
// - debugLog() declaration

#ifndef RIPPLE_BASICS_LOG_H_INCLUDED
#define RIPPLE_BASICS_LOG_H_INCLUDED

#include <xrpl/beast/utility/Journal.h>
#include <string>

namespace ripple {

beast::Journal
debugLog();

// No-op JLOG — the real macro wraps a Journal::Stream.
// We make it compile away to nothing.
struct NullLogStream
{
    template <typename T>
    NullLogStream& operator<<(T const&) { return *this; }
};

#define JLOG(x) if (true) {} else ::ripple::NullLogStream()

}  // namespace ripple

#endif
