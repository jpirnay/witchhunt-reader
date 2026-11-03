#pragma once

// The theme metric tables are tuned for panels with a short side of 480 px or more. On Murphy's
// 240 px the header, tab bar and spacing alone took 35 % of the height, so a compact panel caps
// them. This is the one place that says which build is compact: BaseTheme.h's forPanel() applies
// the caps to every theme's table, and anything sized outside those tables asks capped() too.
//
// A build-time switch because the themes read their tables as constexpr in some 80 places, which
// a runtime scale would miss. Everywhere else capped() returns `tuned`, so those boards compile to
// exactly the numbers in their tables.
namespace PanelMetrics {

#if defined(FREEINK_DEVICE_MURPHY) && FREEINK_DEVICE_MURPHY
inline constexpr bool COMPACT = true;
#else
inline constexpr bool COMPACT = false;
#endif

constexpr int capped(const int tuned, const int limit) { return COMPACT && tuned > limit ? limit : tuned; }

}  // namespace PanelMetrics
