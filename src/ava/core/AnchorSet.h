#pragma once

#include "ava/core/result.h"
#include "ava/debug/print_members_on.h"

#include <filesystem>
#include <memory>
#include <vector>
#include "debug.h"

namespace ava::core {

// Return the launch directory as a logical path, preserving symlinks.
//
// std::filesystem::current_path() calls getcwd(3), which returns the physical
// (symlink-resolved) path. When the user navigated to the directory through a
// symlink (e.g. cd ~/projects/github where projects/github -> /usr/src/projects),
// the shell records the logical path in the $PWD environment variable. This
// function prefers $PWD when it is set, absolute, and stats to the same device
// and inode as getcwd(); otherwise it falls back to getcwd().
//
// This should be called once at program startup. The returned path becomes
// the workspace root (anchor index 0) and should be retrieved thereafter via
// AnchorSet::launch_workspace_root() rather than calling this function again.
[[nodiscard]] Result<std::filesystem::path> launch_workspace_root();

// A set of pre-opened anchor directory descriptors used for symlink-contained
// filesystem access. Each anchor is a directory opened once at startup from a
// trusted configuration path (the workspace root, spill directory, session
// storage, or user-configured additional writable directories). Untrusted
// candidate paths supplied by the model or tools are then resolved against
// the set: the anchor whose root is the longest lexical prefix of the
// candidate is selected, and open_beneath is called with that anchor's
// descriptor and the candidate path relative to the anchor root.
//
// Symlink containment is per-anchor. A symlink inside anchor A that points
// to a path beneath A is followed; a symlink that would escape A (even if it
// points into anchor B) is rejected by open_beneath with EXDEV or ELOOP.
// This ensures that a compromised or buggy tool cannot use a symlink in one
// writable directory to write into a different writable directory without
// going through that directory's own anchor.
//
// Path matching is lexical only — no filesystem canonicalization is performed.
// This avoids TOCTOU races where a symlink is changed between the permission
// check and the actual file operation.
class AnchorSet
{
 public:
  AnchorSet(AnchorSet const&) = delete;
  AnchorSet& operator=(AnchorSet const&) = delete;
  AnchorSet(AnchorSet&& other) noexcept;
  AnchorSet& operator=(AnchorSet&& other) noexcept;
  ~AnchorSet();

  // Open a set of anchor directories. Each path is opened directly with
  // openat (no containment) because the configured paths are trusted startup
  // input; the resulting descriptors serve as anchors for open_beneath calls
  // on untrusted relative paths. Paths that do not exist yet are silently
  // skipped (they will be opened on demand or created by the caller).
  //
  // Returns a shared AnchorSet on success, or an error if no anchor could be
  // opened at all.
  [[nodiscard]] static Result<std::shared_ptr<AnchorSet>> open(std::vector<std::filesystem::path> const& roots);

  struct Anchor
  {
    // The anchor's open directory descriptor. Callers pass this to open_beneath
    // along with relative. The descriptor is owned by the AnchorSet and remains
    // valid for the lifetime of the AnchorSet.
    int fd = -1;
    // The absolute, lexically-normalized root path of the selected anchor.
    std::filesystem::path root;

    AVA_DEBUG_PRINT_MEMBERS_ON
  };

  // The result of resolving a candidate path against the anchor set.
  struct AnchorRef
  {
   private:
    // The best anchor for the given path.
    Anchor const& anchor_;

    // The candidate path relative to the anchor root. This is the path to pass
    // to open_beneath. May be empty if the candidate is the anchor root itself.
    std::filesystem::path relative_;    // Lexically normalized; this does not contain ".." and or "." components anywhere ("." is replaced with an empty path).

    // Calculate a lexically-normalized relative part of root / relative.
    std::filesystem::path normalize_relative(std::filesystem::path const& relative) const
    {
      // Collapse every "." and ".." first; only the normalized form is trustworthy.
      auto const normalized_absolute = (anchor_.root / relative).lexically_normal();
      auto normalized = normalized_absolute.lexically_relative(anchor_.root);
      if (normalized == ".")
        normalized.clear();
      // After lexically_normal, "." is gone and ".." can remain only as a leading
      // run — present iff the normalized path escaped the anchor root.
      if (!normalized.empty() && *normalized.begin() == "..")
        throw std::runtime_error("AnchorRef relative escapes its anchor root");
      return normalized;
    }

   public:
    AnchorRef(Anchor const& best, std::filesystem::path const& relative) : anchor_(best), relative_(normalize_relative(relative))
    {
      DoutEntering(dc::avacore, "AnchorRef::AnchorRef(" << best << ", " << relative << ")");
    }

    Anchor const& anchor() const { return anchor_; }
    std::filesystem::path const& relative() const { return relative_; }
    std::filesystem::path absolute() const { return anchor_.root / relative_; }

    AVA_DEBUG_PRINT_MEMBERS_ON
  };

  // Resolve a candidate path to the anchor that contains it.
  //
  // The candidate is lexically normalized (made absolute and normalized with
  // lexically_normal — no filesystem canonicalization). The anchor whose root
  // is the longest lexical prefix of the normalized candidate is selected.
  // If no anchor root is a prefix, the candidate is outside all writable
  // directories and an error is returned.
  //
  // Returns the anchor descriptor, root path, and relative path on success.
  // Returns PermissionDenied if the candidate is outside all anchors.
  [[nodiscard]] Result<AnchorRef> find_anchor(std::filesystem::path const& candidate) const;

  // Check whether the lexically-normalized candidate path starts with any
  // anchor root. This is a pure string comparison — it does not touch the
  // filesystem and therefore does not detect symlinks that may escape the
  // anchor at resolution time.
  [[nodiscard]] bool contains_lexical(std::filesystem::path const& candidate) const;

  // Return the workspace root (anchor at index 0). This is the launch
  // directory determined at startup and always occupies the first position
  // in the anchor set. Callers that need the workspace root should use this
  // instead of std::filesystem::current_path() to preserve symlinked path
  // components.
  [[nodiscard]] std::filesystem::path const& launch_workspace_root() const;

 private:
  AnchorSet() = default;

  std::vector<Anchor> anchors_;

 public:
  // For test purposes.
  size_t number_of_anchors() const { return anchors_.size(); }
  std::vector<Anchor> const& anchors() const { return anchors_; }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::core
