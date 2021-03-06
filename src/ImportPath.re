open GenTypeCommon;

type t = string;

let react = "react";
let reasonReactPath = (~config) => config.reasonReactPath;
let bsBlockPath = (~config) => config.bsBlockPath;

let fromModule = (~config, ~dir, ~importExtension, moduleName) => {
  let withNoPath = (moduleName |> ModuleName.toString) ++ importExtension;
  switch (config.importPath) {
  | Relative => Filename.concat(dir, withNoPath)
  | Node => withNoPath
  };
};

let fromStringUnsafe = s => s;

let toCmt = (~outputFileRelative, s) =>
  Filename.(concat(outputFileRelative |> dirname, s ++ ".cmt"));
let toString = s => s;