open GenTypeCommon;

let genericsString = (~typeVars) =>
  typeVars === [] ? "" : "<" ++ String.concat(",", typeVars) ++ ">";

let rec renderString = (~language, typ) =>
  switch (typ) {
  | Ident(identPath, typeArguments) =>
    identPath
    ++ genericsString(
         ~typeVars=typeArguments |> List.map(renderString(~language)),
       )
  | TypeVar(s) => s
  | Option(typ)
  | Nullable(typ) =>
    switch (language) {
    | Flow
    | Untyped => "?" ++ (typ |> renderString(~language))
    | Typescript =>
      "(null | undefined | " ++ (typ |> renderString(~language)) ++ ")"
    }
  | Array(typ) =>
    let typIsSimple =
      switch (typ) {
      | Ident(_)
      | TypeVar(_) => true
      | _ => false
      };

    if (language == Typescript && typIsSimple) {
      (typ |> renderString(~language)) ++ "[]";
    } else {
      "Array<" ++ (typ |> renderString(~language)) ++ ">";
    };
  | GroupOfLabeledArgs(fields)
  | Object(fields)
  | Record(fields) => fields |> renderFieldType(~language)
  | Function({typeVars, argTypes, retType}) =>
    renderFunType(~language, ~typeVars, argTypes, retType)
  }
and renderField = (~language, (lbl, optness, typ)) => {
  let optMarker = optness === NonMandatory ? "?" : "";
  lbl ++ optMarker ++ ":" ++ (typ |> renderString(~language));
}
and renderFieldType = (~language, fields) =>
  (language == Flow ? "{|" : "{")
  ++ String.concat(", ", List.map(renderField(~language), fields))
  ++ (language == Flow ? "|}" : "}")
and renderFunType = (~language, ~typeVars, argTypes, retType) =>
  genericsString(~typeVars)
  ++ "("
  ++ String.concat(
       ", ",
       List.mapi(
         (i, t) => {
           let parameterName =
             language == Flow ? "" : "_" ++ string_of_int(i + 1) ++ ":";
           parameterName ++ (t |> renderString(~language));
         },
         argTypes,
       ),
     )
  ++ ") => "
  ++ (retType |> renderString(~language));

let typToString = (~language) => renderString(~language);

let ofType = (~language, ~typ, s) =>
  language == Untyped ? s : s ++ ": " ++ (typ |> typToString(~language));

let flowExpectedError = "// $FlowExpectedError: Reason checked type sufficiently\n";
let commentBeforeRequire = (~language) =>
  switch (language) {
  | Typescript => "// tslint:disable-next-line:no-var-requires\n"
  | Flow => flowExpectedError
  | _ => ""
  };

let emitExportConst = (~name, ~typ, ~config, line) =>
  switch (config.module_, config.language) {
  | (_, Typescript)
  | (ES6, _) =>
    "export const "
    ++ (name |> ofType(~language=config.language, ~typ))
    ++ " = "
    ++ line
  | (CommonJS, _) =>
    "const "
    ++ (name |> ofType(~language=config.language, ~typ))
    ++ " = "
    ++ line
    ++ ";\nexports."
    ++ name
    ++ " = "
    ++ name
  };

let emitExportConstMany = (~name, ~typ, ~config, lines) =>
  lines |> String.concat("\n") |> emitExportConst(~name, ~typ, ~config);

let emitExportFunction = (~name, ~config, line) =>
  switch (config.module_, config.language) {
  | (_, Typescript)
  | (ES6, _) => "export function " ++ name ++ line
  | (CommonJS, _) =>
    "function " ++ name ++ line ++ ";\nexports." ++ name ++ " = " ++ name
  };

let emitExportDefault = (~config, name) =>
  switch (config.module_, config.language) {
  | (_, Typescript)
  | (ES6, _) => "export default " ++ name ++ ";"
  | (CommonJS, _) => "exports.default = " ++ name ++ ";"
  };

let emitExportType = (~language, ~opaque, ~typeName, ~typeVars, ~comment, typ) => {
  let typeParamsString = genericsString(~typeVars);
  let commentString =
    switch (comment) {
    | None => ""
    | Some(s) => " /* " ++ s ++ " */"
    };

  switch (language) {
  | Flow =>
    "export"
    ++ (opaque ? " opaque " : " ")
    ++ "type "
    ++ typeName
    ++ typeParamsString
    ++ " = "
    ++ (typ |> typToString(~language))
    ++ ";"
    ++ commentString

  | Typescript =>
    if (opaque) {
      /* Represent an opaque type as an absract class with a field called 'opaque'.
         Any type parameters must occur in the type of opaque, so that different
         instantiations are considered different types. */
      let typeOfOpaqueField =
        typeVars == [] ? "any" : typeVars |> String.concat(" | ");
      "// tslint:disable-next-line:max-classes-per-file \n"
      ++ "export abstract class "
      ++ typeName
      ++ typeParamsString
      ++ " { protected opaque: "
      ++ typeOfOpaqueField
      ++ " }; /* simulate opaque types */"
      ++ commentString;
    } else {
      "// tslint:disable-next-line:interface-over-type-literal\n"
      ++ "export type "
      ++ typeName
      ++ typeParamsString
      ++ " = "
      ++ (typ |> typToString(~language))
      ++ ";"
      ++ commentString;
    }
  | Untyped => ""
  };
};

let emitExportVariantType = (~language, ~name, ~typeParams, ~leafTypes) =>
  switch (language) {
  | Flow
  | Typescript =>
    "export type "
    ++ name
    ++ genericsString(
         ~typeVars=typeParams |> List.map(typToString(~language)),
       )
    ++ " =\n  | "
    ++ String.concat("\n  | ", List.map(typToString(~language), leafTypes))
    ++ ";"
  | Untyped => ""
  };

let emitRequire = (~language, moduleName, importPath) =>
  commentBeforeRequire(~language)
  ++ "const "
  ++ ModuleName.toString(moduleName)
  ++ " = require(\""
  ++ (importPath |> ImportPath.toString)
  ++ "\");";

let requireReact = (~language) =>
  switch (language) {
  | Flow => emitRequire(~language, ModuleName.react, ImportPath.react)
  | Typescript => "import * as React from \"react\";"
  | Untyped => ""
  };

let reactComponentType = (~language, ~propsTypeName) =>
  Ident(
    language == Flow ? "React$ComponentType" : "React.ComponentClass",
    [Ident(propsTypeName, [])],
  );

let fileHeader = (~language) =>
  switch (language) {
  | Flow => "/** \n * @flow strict\n * @generated \n * @nolint\n */\n"
  | Typescript => "/* Typescript file generated by genType. */\n"
  | Untyped => "/* Untyped file generated by genType. */\n"
  };

let componentExportName = (~language, ~moduleName) =>
  language == Flow ? "component" : ModuleName.toString(moduleName);

let outputFileSuffix = (~language) =>
  switch (language) {
  | Flow
  | Untyped => ".re.js"
  | Typescript => ".tsx"
  };

let generatedModuleExtension = (~language) => language == Flow ? ".re" : "";

let emitImportTypeAs = (~language, ~typeName, ~asTypeName, ~importPath) =>
  switch (language) {
  | Flow
  | Typescript =>
    (language == Flow ? "" : "\n")
    ++ "import "
    ++ (language == Flow ? "type " : "")
    ++ "{"
    ++ typeName
    ++ (
      switch (asTypeName) {
      | Some(asT) => " as " ++ asT
      | None => ""
      }
    )
    ++ "} from '"
    ++ (importPath |> ImportPath.toString)
    ++ "';"
  | Untyped => ""
  };

let blockTagValue = (~language, i) =>
  string_of_int(i) ++ (language == Typescript ? " as any" : "");

let shimExtension = (~language) =>
  switch (language) {
  | Flow => ".shim.js"
  | Typescript => ".shim.ts"
  | Untyped => ".shim.not.used"
  };