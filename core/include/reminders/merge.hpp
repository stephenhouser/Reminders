// Merging a Syncthing conflict copy back into its list. See docs/FORMAT.md,
// "Conflicts".
#pragma once

#include "reminders/model.hpp"

namespace rem {

// Merges `conflict` into `main`. With a `base` (the last version both sides
// shared) this is a three-way merge: a field changed on one side only takes
// that side; changed on both, `main` wins. Without a base, `main` wins every
// field and nothing is deleted. Reminders are matched by id, or by title
// when they have no id.
Document merge(const Document& main, const Document& conflict,
			   const Document* base);

}  // namespace rem
