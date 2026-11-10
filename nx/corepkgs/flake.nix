dir:
let
  inherit (builtins) all any attrNames concatStringsSep elem elemAt filter foldl' genList head isAttrs isBool isFunction isInt isList isPath isString length listToAttrs mapAttrs match removeAttrs split stringLength substring;
  changed = what: throw "flake 'path:${dir}' requires lock file changes but they're not allowed due to '--no-update-lock-file' (${what})";
  comps = s: filter (c: isString c && c != "" && c != ".") (split "/" s);
  # CanonPath(s, base): ".." above the root stays at the root (clamp) or is an escape
  canon = clamp: base: s: foldl' (acc: c: if c != ".." then acc ++ [ c ] else if acc != [ ] then genList (elemAt acc) (length acc - 1) else if clamp then [ ] else throw "access to a path outside the flake source '${dir}': ${s}") base (comps s);
  real = cs: dir + concatStringsSep "" (map (c: "/" + c) cs);
  showPath = concatStringsSep "/";
  flakeId = s: match "[a-zA-Z][a-zA-Z0-9_-]*" s != null;
  parsePath = s: map (e: if flakeId e then e else throw "invalid flake input attribute path element '${e}'") (comps s);
  # shortcut: a url is path:P, a relative or absolute path, or a flake id; any other scheme stays opaque and so never matches a lock entry
  parseRef = url: if substring 0 5 url == "path:" then { type = "path"; path = substring 5 (stringLength url) url; }
    else if substring 0 1 url == "." || substring 0 1 url == "/" then { type = "path"; path = url; }
    else if substring 0 6 url == "flake:" then { type = "indirect"; id = substring 6 (stringLength url) url; }
    else if flakeId url then { type = "indirect"; id = url; }
    else { inherit url; };
  isRelative = ref: ref.type or null == "path" && substring 0 1 ref.path != "/";
  # CanonPath::makeRelative from the components a to b
  makeRel = a: b: if a != [ ] && b != [ ] && head a == head b then makeRel (builtins.tail a) (builtins.tail b)
    else if a == [ ] && b == [ ] then "." else showPath (map (_: "..") a ++ b);
  trivial = n: x: if isString x || isBool x || (isInt x && x >= 0) then x else throw "flake input attribute '${n}' is not a string, Boolean, or integer";
  # parseFlakeInput (flake.cc)
  parseInput = fdir: lockRoot: a:
    let
      inner = removeAttrs (if isAttrs a then a else throw "a flake input is not a set") [ "url" "flake" "inputs" "follows" ];
      url = if !(a ? url) then null else if isString a.url then a.url
        else if isPath a.url then
          let p = toString a.url; in
          if p == dir then "path:" + makeRel fdir [ ]
          else if substring 0 (stringLength dir + 1) p == dir + "/" then "path:" + makeRel fdir (comps (substring (stringLength dir) (stringLength p) p))
          else throw "input attribute path '${p}' must be in the same source tree as '${dir}'"
        else throw "a flake input url is neither a string nor a path";
      attrs = mapAttrs trivial inner;
      ref = if inner ? type then attrs // (if url == null then { } else { inherit url; })
        else if inner != { } then throw "unexpected flake input attribute '${head (attrNames inner)}'"
        else if url == null then null else parseRef url;
      follows = if !(a ? follows) then null else if isString a.follows then lockRoot ++ parsePath a.follows else throw "follows is not a string";
    in
    if ref != null && follows != null then throw "flake input has both a flake reference and a follows attribute"
    else {
      inherit ref follows;
      isFlake = if !(a ? flake) then true else if isBool a.flake then a.flake else throw "flake is not a Boolean";
      overrides = if a ? inputs then parseInputs fdir lockRoot false a.inputs else { };
    };
  parseInputs = fdir: lockRoot: allowSelf: v:
    if !isAttrs v then throw "flake inputs are not a set"
    else if v ? self && !allowSelf then throw "'self' input attribute not allowed"
    else mapAttrs (_: parseInput fdir lockRoot) (removeAttrs v [ "self" ]);
  # readFlake (flake.cc): the inputs of the flake in directory fdir, with its outputs' formals as indirect inputs
  readFlake = fdir: lockRoot:
    let
      f = import (real fdir + "/flake.nix");
      bad = filter (n: !elem n [ "description" "inputs" "outputs" "nixConfig" ]) (attrNames f);
      declared = if f ? inputs then parseInputs fdir lockRoot true f.inputs else { };
      formals = filter (n: n != "self" && !(declared ? ${n})) (attrNames (builtins.functionArgs f.outputs));
      implicit = listToAttrs (map (n: { name = n; value = { ref = { type = "indirect"; id = n; }; follows = null; isFlake = true; overrides = { }; }; }) formals);
    in
    if !isAttrs f then throw "flake.nix is not a set"
    else if bad != [ ] then throw "flake has an unsupported attribute '${head bad}'"
    else if f ? description && !isString f.description then throw "flake description is not a string"
    else if !(f ? outputs) then throw "flake lacks attribute 'outputs'"
    else if !isFunction f.outputs then throw "flake outputs is not a function"
    else builtins.deepSeq declared (declared // implicit);

  # LockFile (lockfile.cc): versions 5 to 7, every node reachable from the root has locked and original
  lockFile = real [ ] + "/flake.lock";
  json = if builtins.pathExists lockFile then builtins.fromJSON (builtins.readFile lockFile) else { root = "root"; nodes.root = { }; version = 7; };
  version = json.version or 0;
  rootKey = if isString json.root then json.root else throw "lock file root is not a string";
  nodes = json.nodes or { };
  nodeOf = k: if k == rootKey then nodes.${k} or { } else nodes.${k} or (throw "lock file references missing node '${k}'");
  edgeKeys = k: filter isString (map (i: (nodeOf k).inputs.${i}) (attrNames ((nodeOf k).inputs or { })));
  checkNode = k:
    let n = nodeOf k; in
    if k != rootKey && !(n ? locked) then throw "attribute 'locked' missing in lock file"
    else if k != rootKey && !(n ? original) then throw "attribute 'original' missing in lock file"
    else if k != rootKey && n.locked.type or null == "path" && !isRelative n.locked && !(n.locked ? narHash) then throw "lock file contains unlocked input '${n.locked.path}'"
    else map (c: if c == rootKey then throw "lock file contains cycle to root node" else { key = c; }) (edgeKeys k);
  reachable = map (x: x.key) (builtins.genericClosure { startSet = [ { key = rootKey; } ]; operator = x: checkNode x.key; });
  # a node two edges reach is two nodes in the lock Nix writes (toJSON), so the lock would change
  edges = builtins.concatMap edgeKeys reachable;
  shared = filter (k: length (filter (e: e == k) edges) > 1) reachable;
  lockOk = if !(version >= 5 && version <= 7) then throw "lock file '${lockFile}' has unsupported version ${toString version}"
    else if shared != [ ] then changed "node '${head shared}' is shared" else true;

  # computeLocks (flake.cc) where every input must keep its lock entry: a new or a removed entry changes the lock
  computeLocks = flakeInputs: oldKey: prefix: followsPrefix: srcDir: trustLock: overrides0:
    let
      oldInputs = (nodeOf oldKey).inputs or { };
      addOverrides = acc: input: path: foldl' (acc: id:
        let o = input.overrides.${id}; p = path ++ [ id ]; k = showPath p; in
        addOverrides (if (o.ref != null || o.follows != null) && !(acc ? ${k}) then acc // { ${k} = { input = o; inherit srcDir; parent = prefix; }; } else acc) o p) acc (attrNames input.overrides);
      overrides = foldl' (acc: id: addOverrides acc flakeInputs.${id} (prefix ++ [ id ])) overrides0 (attrNames flakeInputs);
      extra = filter (id: !(flakeInputs ? ${id})) (attrNames oldInputs);
      one = id:
        let
          path = prefix ++ [ id ];
          ov = overrides.${showPath path} or null;
          input = if ov == null then flakeInputs.${id} else ov.input // { inherit (flakeInputs.${id}) isFlake; };
          ovSrc = if ov == null then srcDir else ov.srcDir;
          old = oldInputs.${id} or null;
          ref = if input.ref != null then input.ref else { type = "indirect"; inherit id; };
          rel = isRelative ref;
          parentPath = if rel then (if ov == null then prefix else ov.parent) else null;
          oldNode = if isString old then nodeOf old else null;
          oldChild = oldNode.inputs or { };
          mustRefetch = !trustLock && any (i: isList oldChild.${i} && !(overrides ? ${showPath (path ++ [ i ])})) (attrNames oldChild);
          cdir = canon true ovSrc ref.path;
          fake = mapAttrs (_: e: if isList e then { ref = null; follows = followsPrefix ++ e; isFlake = true; overrides = { }; }
            else { ref = (nodeOf e).original; follows = null; isFlake = (nodeOf e).flake or true; overrides = { }; }) oldChild;
        in
        if input.follows != null then old == input.follows || changed "input '${showPath path}' follows '${showPath input.follows}'"
        else if oldNode == null || oldNode.original != ref || oldNode.parent or null != parentPath then changed "input '${showPath path}'"
        else if !mustRefetch then computeLocks fake old path followsPrefix srcDir true overrides
        else if rel then computeLocks (readFlake cdir path) old path followsPrefix cdir false overrides
        else throw "snx does not fetch flake input '${showPath path}'";
    in
    if extra != [ ] then changed "input '${showPath (prefix ++ [ (head extra) ])}' is gone" else all one (attrNames flakeInputs);

  # LockFile::check: a follows leads to a node; doFind keeps one visited list for the whole search
  doFind = visited: path:
    if elem path visited then throw "follow cycle detected: [${concatStringsSep " -> " (map showPath (visited ++ [ path ]))}]"
    else foldl' (st: e:
      if st.pos == null then st
      else let edge = (nodeOf st.pos).inputs.${e} or null; in
      if edge == null then st // { pos = null; }
      else if isList edge then doFind st.visited edge
      else st // { pos = edge; }) { pos = rootKey; visited = visited ++ [ path ]; } path;
  followsOk = all (k: all (e: e == [ ] || (doFind [ ] e).pos != null || throw "input follows a non-existent input '${showPath e}'")
    (filter isList (map (i: (nodeOf k).inputs.${i}) (attrNames ((nodeOf k).inputs or { }))))) reachable;

  # callFlake: the lock as Nix writes it and each relative node's directory (an escape fails when the node is used)
  nodeDir = k: if k == rootKey then [ ] else canon false (nodeDir (getInputByPath rootKey (nodes.${k}.parent or [ ]))) nodes.${k}.locked.path;
  getInputByPath = k: path: if path == [ ] then k else let e = (nodeOf k).inputs.${head path}; in getInputByPath (if isList e then getInputByPath rootKey e else e) (builtins.tail path);
  relNodes = builtins.filter (k: k == rootKey || isRelative (nodes.${k}.locked or { })) (attrNames (nodes // { ${rootKey} = { }; }));
  overrides = listToAttrs (map (k: { name = k; value = { sourceInfo.outPath = real (nodeDir k); dir = ""; }; }) relNodes);
  lockStr = builtins.toJSON { root = rootKey; nodes = nodes // { ${rootKey} = nodeOf rootKey; }; version = 7; };
  flake = import /__corepkgs__/call-flake.nix lockStr overrides (_: throw "snx does not fetch inputs");
in
assert lockOk && computeLocks (readFlake [ ] [ ]) rootKey [ ] [ ] [ ] false { } && followsOk;
builtins.toJSON flake.outputs.result
