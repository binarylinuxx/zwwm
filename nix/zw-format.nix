{ lib }:

let
  valueType = (lib.types.nullOr (lib.types.oneOf [
    lib.types.bool
    lib.types.int
    lib.types.str
    lib.types.path
    (lib.types.listOf valueType)
    (lib.types.attrsOf valueType)
  ])) // { description = "a zw configuration value"; };

  identifier = name: builtins.match "[A-Za-z_][A-Za-z0-9_.-]*" name != null;
  field = name: if identifier name then name else builtins.toJSON name;
  renderValue = value:
    if value == null then "null"
    else if builtins.isBool value then (if value then "true" else "false")
    else if builtins.isInt value then toString value
    else if builtins.isString value || builtins.isPath value then builtins.toJSON (toString value)
    else if builtins.isList value then "[${lib.concatMapStringsSep ", " renderValue value}]"
    else if builtins.isAttrs value then "{ ${lib.concatStringsSep " " (lib.mapAttrsToList (name: item: "${field name} = ${renderValue item}") value)} }"
    else throw "zw configuration values must be strings, integers, booleans, paths, lists, objects, or null";

  renderAssignment = name: value:
    if !identifier name then throw "invalid zw top-level setting name: ${name}"
    else if name == "bind" && builtins.isList value then
      let
        bindings = if value == [ ] then [ ] else if builtins.isList (builtins.head value) then value else [ value ];
      in lib.concatMapStrings (binding:
        if !builtins.isList binding || builtins.length binding != 4 then throw "each zwwm binding requires four values"
        else "bind = ${lib.concatMapStringsSep ", " renderValue binding}\n"
      ) bindings
    else "${name} = ${renderValue value}\n";
in {
  type = lib.types.attrsOf valueType;
  render = settings: lib.concatStrings (lib.mapAttrsToList renderAssignment settings);
}
