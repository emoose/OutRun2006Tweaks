from pathlib import Path

path = Path('src/input_manager.hpp')
s = path.read_text(encoding='utf-8')

old = 'setPrimaryGamepad(std::min(removedIndex, int(controllers.size()) - 1));'
new = '''const int replacementIndex =
                    removedIndex < int(controllers.size())
                        ? removedIndex
                        : int(controllers.size()) - 1;
                setPrimaryGamepad(replacementIndex);'''

count = s.count(old)
if count != 1:
    raise SystemExit(f'round5 compile fix: expected exactly one std::min hotplug site, got {count}')

s = s.replace(old, new, 1)
path.write_text(s, encoding='utf-8')

# Guard against Windows min/max macro regressions in this header.  Windows.h can
# be included before input_manager.hpp in several translation units, so an
# unprotected std::min( token is not safe even when <algorithm> is included.
if 'std::min(' in s or 'std::max(' in s:
    raise SystemExit('round5 compile fix: unsafe std::min/std::max call remains in input_manager.hpp')

if 'setPrimaryGamepad(replacementIndex);' not in s:
    raise SystemExit('round5 compile fix: replacement controller selection missing')

print('Applied round-5 MSVC min/max macro compile fix')
