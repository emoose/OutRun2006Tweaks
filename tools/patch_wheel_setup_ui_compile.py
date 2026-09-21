from pathlib import Path

path = Path("src/overlay/wheel_setup_ui.cpp")
text = path.read_text(encoding="utf-8")

text = text.replace(
'''#include <algorithm>\n#include <array>\n#include <cmath>\n#include <cstdint>\n#include <cstring>\n#include <string>\n#include <vector>\n''',
'''#include <algorithm>\n#include <array>\n#include <cctype>\n#include <cmath>\n#include <cstdint>\n#include <cstdio>\n#include <cstring>\n#include <string>\n#include <utility>\n#include <vector>\n''',
1)

text = text.replace('axis_name(axisSetting)', 'axis_name(int(axisSetting))')

# wheel_setup_ui.cpp is outside namespace Settings here; qualify the template
# type explicitly so MSVC does not parse Setting<int> as an unknown template.
text = text.replace('Setting<int>& axisSetting', 'Settings::Setting<int>& axisSetting')
text = text.replace('Setting<int>& setting', 'Settings::Setting<int>& setting')

old = '''        void assign_digital(int code)\n        {\n            switch (target_)\n'''
new = '''        void assign_digital(int code)\n        {\n            const bool gameplayButton =\n                target_ == BindTarget::GearUp || target_ == BindTarget::GearDown ||\n                target_ == BindTarget::Start || target_ == BindTarget::View;\n            if (gameplayButton && code >= 128)\n            {\n                target_ = BindTarget::None;\n                status_ = "Gameplay actions currently require a physical button; POV is supported for menu directions.";\n                return;\n            }\n\n            switch (target_)\n'''
if old not in text:
    raise SystemExit("assign_digital insertion point not found")
text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
print("Patched universal wheel setup UI compile details")
