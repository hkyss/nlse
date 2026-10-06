# code.json

`code.json` in a mod folder patches the game's scripts at startup. Keys are script names or full names like `gml_Object_o_main_menu_Step_0`:

    {"gui_menu_font": [{"find": 40, "set": 26}],
     "gui_main_menu": [{"skip": "gui_button_accept_hint"}]}

- `{"find": 40, "set": 26}`: replace a number or `true`/`false`
- `{"skip": "f"}`: drop calls to `f`
- `{"call": "f", "to": "g"}`: call `g` instead of `f`
- `{"member": "a", "to": "b"}`: read `b` instead of `a`
- `{"member": "a", "set": 5}`: `a` reads as 5
- `{"member": "a", "set": {"noise": [0, 0.03], "seconds": 8}}`: `a` drifts randomly in that range, turning about every 8 s
- `{"new": "gw_Label", "show": false}`: hide what `new gw_Label` makes
- `{"new": "gw_WrapPanel", "show": "hover"}`: show it only on hover over it or the part above; `"pin": true` keeps it open on click
- `{"new": "gw_StackPanel", "name": "stock"}`: name it; `{"new": "gw_Canvas", "into": "stock", "at": 2}`: move a part into it at position 2
- `"align": ["right", "top"]`, `"margin": [left, top, right, bottom]`, `"size": [w, h]`: place a part in its parent, in view units, `null` is 0
- `{"picture": "s_white_pixel", "into": "court", "at": 1, "size": [2, 48], "color": 6461622, "alpha": 0.8}`: add a sprite to a named part, color in BGR

`nth` picks one of several matches, from 1. `count` is the expected number of matches: if it differs, the change is refused, which catches game updates. A mod's changes apply all or none, and the later mod wins a conflict.
