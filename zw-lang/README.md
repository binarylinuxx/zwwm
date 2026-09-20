# zw-lang

`zw-lang` is the standalone parser and validation library for zwwm
configuration. Consumers link `zwwm::lang`; compositor-specific validation is
provided through callbacks rather than compiled into the language module.

## Syntax

```zw
-- Variables and references
SUPER = "Super"
TERMINAL = "ghostty"

bind = $SUPER, Return, exec, $TERMINAL
items = ["one", "two"]
output = {
  name = "DP-1"
  mode = "2560x1440@144"
}
```

The language supports strings, booleans, integers, `null`, variable references,
arrays, comma-separated lists, objects, comments, and trailing array commas.

## API

```cpp
const auto parsed = zwwm::lang::parse_config_file(path);
if (!parsed.ok()) {
  // Inspect parsed.diagnostics.
}

const auto checked = zwwm::lang::validate_config(parsed.config, rules);
const auto* value = zwwm::lang::find_assignment(parsed.config, "output");
const auto* object = value == nullptr ? nullptr : zwwm::lang::as_object(*value);
```

Use `find_assignment`, `find_field`, and the `as_*` accessors instead of
depending on the internal variant representation. `ValidationRule` and
`ValidationContext` add component-owned semantic checks.
