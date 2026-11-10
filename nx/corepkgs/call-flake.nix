lockFileStr:

overrides:

fetchTreeFinal:

let
  inherit (builtins) mapAttrs;

  lockFile = builtins.fromJSON lockFileStr;

  resolveInput =
    inputSpec: if builtins.isList inputSpec then getInputByPath lockFile.root inputSpec else inputSpec;

  getInputByPath =
    nodeName: path:
    if path == [ ] then
      nodeName
    else
      getInputByPath
        (resolveInput lockFile.nodes.${nodeName}.inputs.${builtins.head path})
        (builtins.tail path);

  allNodes = mapAttrs (
    key: node:
    let
      hasOverride = overrides ? ${key};
      isRelative = node.locked.type or null == "path" && builtins.substring 0 1 node.locked.path != "/";

      parentNode = allNodes.${getInputByPath lockFile.root node.parent};

      sourceInfo =
        if hasOverride then
          overrides.${key}.sourceInfo
        else if isRelative then
          parentNode.sourceInfo
        else
          fetchTreeFinal (node.info or { } // removeAttrs node.locked [ "dir" ]);

      subdir = overrides.${key}.dir or node.locked.dir or "";

      outPath =
        if !hasOverride && isRelative then
          parentNode.outPath + (if node.locked.path == "" then "" else "/" + node.locked.path)
        else
          sourceInfo.outPath + (if subdir == "" then "" else "/" + subdir);

      flake = import (outPath + "/flake.nix");

      inputs = mapAttrs (inputName: inputSpec: allNodes.${resolveInput inputSpec}.result) (
        node.inputs or { }
      );

      outputs = flake.outputs (inputs // { self = result; });

      result =
        outputs
        // sourceInfo
        // {
          inherit outPath;

          inherit inputs;
          inherit outputs;
          inherit sourceInfo;
          _type = "flake";
        };

    in
    {
      result =
        if node.flake or true then
          assert builtins.isFunction flake.outputs;
          result
        else
          sourceInfo // { inherit sourceInfo outPath; };

      inherit outPath sourceInfo;
    }
  ) lockFile.nodes;

in
allNodes.${lockFile.root}.result
