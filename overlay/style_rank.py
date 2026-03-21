"""
Style Rank reader - walks the SpEffect linked list and determines current rank.
"""

from dataclasses import dataclass, field
from memory_reader import MemoryReader

# Style rank SpEffect IDs
STYLE_EFFECT_MIN = 999320410
STYLE_EFFECT_MAX = 999320429
EFFECT_MAX_DURATION = 5.0

RANK_NAMES = ["", "D", "C", "B", "A", "S", "SS", "SSS", "V"]
RANK_COLORS = [
    (128, 128, 128),  # None - gray
    (180, 180, 180),  # D - white/gray
    (100, 180, 255),  # C - light blue
    (80, 220, 80),    # B - green
    (255, 220, 50),   # A - yellow/gold
    (255, 140, 0),    # S - orange
    (255, 80, 80),    # SS - red
    (200, 50, 255),   # SSS - purple
    (255, 255, 255),  # V - white (max)
]


@dataclass
class StyleRankState:
    rank: int = 0             # 0=None, 1=D, 2=C, 3=B, 4=A, 5=S, 6=SS, 7=SSS, 8=V
    effect_count: int = 0     # Number of active style effects
    min_duration: float = 0.0 # Shortest remaining duration
    max_duration: float = 0.0 # Longest remaining duration
    avg_duration: float = 0.0 # Average remaining duration across all effects
    total_duration: float = 0.0 # Sum of all remaining durations
    max_possible: float = EFFECT_MAX_DURATION
    durations: list = field(default_factory=list) # All individual durations sorted

    @property
    def rank_name(self) -> str:
        if 0 <= self.rank < len(RANK_NAMES):
            return RANK_NAMES[self.rank]
        return "?"

    @property
    def rank_color(self) -> tuple:
        if 0 <= self.rank < len(RANK_COLORS):
            return RANK_COLORS[self.rank]
        return (128, 128, 128)

    @property
    def gauge_fill(self) -> float:
        """0.0 to 1.0 - overall gauge fullness.

        Uses total remaining duration as a proportion of max possible total.
        Max total = 8 effects * 5.0 seconds = 40.0 seconds.
        This gives a smooth continuous gauge that represents overall style momentum.
        """
        max_total = 8 * self.max_possible  # 40 seconds at full V rank
        if self.total_duration <= 0:
            return 0.0
        return min(1.0, self.total_duration / max_total)

    @property
    def seconds_until_drop(self) -> float:
        """How many seconds until losing the current rank tier."""
        return self.min_duration


import time as _time

_debounce_rank = 0        # the rank we're currently displaying
_debounce_candidate = 0   # the new rank we've seen
_debounce_since = 0.0     # when we first saw the candidate
DEBOUNCE_TIME = 0.25      # seconds a new rank must hold before we switch


def read_style_rank(reader: MemoryReader) -> StyleRankState:
    """Walk the SpEffect linked list and count active style effects.

    Uses a 0.25s debounce — rank must hold steady before display updates.
    """
    state = StyleRankState()

    head = reader.get_sp_effect_list_head()
    if head == 0:
        return state

    node = head
    count = 0
    style_count = 0
    durations = []

    while node != 0 and count < 500:
        effect_id = reader.read_i32(node + 0x08)

        if STYLE_EFFECT_MIN <= effect_id <= STYLE_EFFECT_MAX:
            style_count += 1
            dur = reader.read_f32(node + 0x48)
            if dur >= 0:
                durations.append(dur)

        count += 1
        next_node = reader.read_ptr(node + 0x38)
        if next_node == 0 or next_node == head or next_node == node:
            break
        node = next_node

    global _debounce_rank, _debounce_candidate, _debounce_since

    raw_rank = min(style_count, 8)
    now = _time.time()

    # Debounce: only change displayed rank after it holds for DEBOUNCE_TIME
    if raw_rank != _debounce_candidate:
        print(f"[{now:.2f}] RAW rank changed: {_debounce_candidate} -> {raw_rank} (displayed: {_debounce_rank}, resetting debounce)")
        _debounce_candidate = raw_rank
        _debounce_since = now
    elif raw_rank != _debounce_rank and now - _debounce_since >= DEBOUNCE_TIME:
        print(f"[{now:.2f}] DISPLAY rank change: {_debounce_rank} -> {_debounce_candidate} (held for {now - _debounce_since:.3f}s)")
        _debounce_rank = _debounce_candidate

    state.effect_count = style_count
    state.rank = _debounce_rank
    state.durations = sorted(durations)
    state.total_duration = sum(durations)
    state.min_duration = min(durations) if durations else 0.0
    state.max_duration = max(durations) if durations else 0.0
    state.avg_duration = (sum(durations) / len(durations)) if durations else 0.0

    return state
