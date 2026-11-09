#pragma once

#include <ios>
#include <streambuf>

namespace ava::tests {

// Make stream use the stream buffer replacement for the lifetime of this object.
//
// Use it to feed std::cin from, or capture std::cout and std::cerr into, a string stream while the code
// under test uses the standard streams directly. The destructor restores the original stream buffer.
// Changing the stream buffer also clears the state flags of stream, both here and upon restoration.
// The standard streams are process-global: do not use this while another thread uses stream.
class StreamRedirect
{
 private:
  std::ios& stream_;
  std::streambuf* original_;

 public:
  StreamRedirect(std::ios& stream, std::streambuf* replacement) : stream_(stream), original_(stream.rdbuf(replacement)) { }

  ~StreamRedirect() { stream_.rdbuf(original_); }

  StreamRedirect(StreamRedirect const&) = delete;
  StreamRedirect& operator=(StreamRedirect const&) = delete;
};

}  // namespace ava::tests
