#pragma once

#include "FeedbackKey.h"
#include "Data.h"
#include <vector>

namespace ava::app::frontend {

struct KeyInputFeedbackSnapshot
{
  std::vector<FeedbackKey> keys_;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// List of keys typed in the composer area while the draft is still empty (aka, the first key) for which feedback must be provided.
class FirstComposerKey : public Data
{
 public:
  using Data::Data;

  KeyInputFeedbackSnapshot snapshot() const;

  // The user pressed one of the keys configured as first key in the composer area.
  // If this function returns true then the key must be handled by the frontend as normal input; otherwise calling the `on_key` was enough.
  bool on_key(FeedbackKey key);

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(Data)
};

// List of keys for which feedback must always be provided.
class KeyInputFeedback : public Data
{
 public:
  using Data::Data;

  KeyInputFeedbackSnapshot snapshot() const;

  // The user pressed one of the keys configured as feedback key.
  // If this function returns true then the key must be handled by the frontend as normal input; otherwise calling the `on_key` was enough.
  bool on_key(FeedbackKey key);

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(Data)
};

} // namespace ava::app::frontend
