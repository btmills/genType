open GenTypeCommon;

type typeMap = StringMap.t((list(string), typ));

type env = {
  requires: ModuleNameMap.t(ImportPath.t),
  externalReactClass: list(CodeItem.externalReactClass),
  /* For each .cmt we import types from, keep the map of exported types. */
  cmtExportTypeMapCache: StringMap.t(typeMap),
  /* Map of types imported from other files. */
  typesFromOtherFiles: typeMap,
};

let requireModule = (~requires, ~importPath, moduleName) =>
  requires
  |> ModuleNameMap.add(
       moduleName,
       moduleName |> ModuleResolver.resolveSourceModule(~importPath),
     );

let createExportTypeMap = (~language, codeItems): typeMap => {
  let updateExportTypeMap = (exportTypeMap: typeMap, codeItem): typeMap => {
    let addExportType = ({typeName, typeVars, typ, _}: CodeItem.exportType) => {
      if (Debug.codeItems) {
        logItem(
          "Export Type: %s%s = %s\n",
          typeName,
          typeVars == [] ?
            "" : "(" ++ (typeVars |> String.concat(",")) ++ ")",
          typ |> EmitTyp.typToString(~language),
        );
      };
      exportTypeMap |> StringMap.add(typeName, (typeVars, typ));
    };
    switch (codeItem) {
    | CodeItem.ExportType(exportType) => exportType |> addExportType
    | ValueBinding(_)
    | ComponentBinding(_)
    | ImportType(_)
    | ExportVariantType(_)
    | ConstructorBinding(_)
    | ExternalReactClass(_) => exportTypeMap
    };
  };
  codeItems |> List.fold_left(updateExportTypeMap, StringMap.empty);
};

let emitCodeItems =
    (
      ~config,
      ~outputFileRelative,
      ~resolver,
      ~inputCmtToTypeDeclarations,
      codeItems,
    ) => {
  let language = config.language;
  let requireBuffer = Buffer.create(100);
  let importTypeBuffer = Buffer.create(100);
  let exportBuffer = Buffer.create(100);
  let line__ = (buffer, s) => {
    if (Buffer.length(buffer) > 0) {
      Buffer.add_string(buffer, "\n");
    };
    Buffer.add_string(buffer, s);
  };
  let require = line__(requireBuffer);
  let export = line__(exportBuffer);

  let emitImportType = (~language, ~env, importType) =>
    switch (importType) {
    | CodeItem.ImportComment(s) =>
      s |> line__(importTypeBuffer);
      env;
    | ImportTypeAs({typeName, asTypeName, importPath, cmtFile}) =>
      EmitTyp.emitImportTypeAs(~language, ~typeName, ~asTypeName, ~importPath)
      |> line__(importTypeBuffer);

      switch (asTypeName, cmtFile) {
      | (None, _)
      | (_, None) => env
      | (Some(asType), Some(cmtFile)) =>
        let updateTypeMapFromOtherFiles = (~exportTypeMapFromCmt) =>
          switch (exportTypeMapFromCmt |> StringMap.find(typeName)) {
          | x => env.typesFromOtherFiles |> StringMap.add(asType, x)
          | exception Not_found => exportTypeMapFromCmt
          };
        switch (env.cmtExportTypeMapCache |> StringMap.find(cmtFile)) {
        | exportTypeMapFromCmt => {
            ...env,
            typesFromOtherFiles:
              updateTypeMapFromOtherFiles(~exportTypeMapFromCmt),
          }
        | exception Not_found =>
          let exportTypeMapFromCmt =
            Cmt_format.read_cmt(cmtFile)
            |> inputCmtToTypeDeclarations(~language)
            |> createExportTypeMap(~language);
          let cmtExportTypeMapCache =
            env.cmtExportTypeMapCache
            |> StringMap.add(cmtFile, exportTypeMapFromCmt);
          {
            ...env,
            cmtExportTypeMapCache,
            typesFromOtherFiles:
              updateTypeMapFromOtherFiles(~exportTypeMapFromCmt),
          };
        };
      };
    };

  let emitExportType =
      (~language, {CodeItem.opaque, typeVars, typeName, comment, typ}) =>
    typ
    |> EmitTyp.emitExportType(
         ~language,
         ~opaque,
         ~typeName,
         ~typeVars,
         ~comment,
       )
    |> export;

  let emitCheckJsWrapperType = (~env, ~propsTypeName) =>
    switch (env.externalReactClass) {
    | [] => ""

    | [{componentName, _}] =>
      let s =
        "("
        ++ (
          "props" |> EmitTyp.ofType(~language, ~typ=Ident(propsTypeName, []))
        )
        ++ ") {\n      return <"
        ++ componentName
        ++ " {...props}/>;\n    }";
      EmitTyp.emitExportFunction(~name="checkJsWrapperType", ~config, s);

    | [_, ..._] => "// genType warning: found more than one external component annotated with @genType"
    };

  let emitCodeItem = (~exportTypeMap, env, codeItem) => {
    let typToConverter = typ =>
      typ
      |> Converter.typToConverter(
           ~language,
           ~exportTypeMap,
           ~typesFromOtherFiles=env.typesFromOtherFiles,
         );
    if (Debug.codeItems) {
      logItem("Code Item: %s\n", codeItem |> CodeItem.toString(~language));
    };
    switch (codeItem) {
    | CodeItem.ImportType(importType) =>
      emitImportType(~language, ~env, importType)

    | ExportType(exportType) =>
      emitExportType(~language, exportType);
      env;

    | ExportVariantType({CodeItem.typeParams, leafTypes, name}) =>
      EmitTyp.emitExportVariantType(~language, ~name, ~typeParams, ~leafTypes)
      |> export;
      env;

    | ValueBinding({moduleName, id, typ}) =>
      let importPath =
        ModuleResolver.resolveModule(
          ~config,
          ~outputFileRelative,
          ~resolver,
          ~importExtension=".bs",
          moduleName,
        );
      let moduleNameBs = moduleName |> ModuleName.forBsFile;
      let requires =
        moduleNameBs |> requireModule(~requires=env.requires, ~importPath);
      let converter = typ |> typToConverter;

      (
        ModuleName.toString(moduleNameBs)
        ++ "."
        ++ Ident.name(id)
        |> Converter.toJS(~converter)
      )
      ++ ";"
      |> EmitTyp.emitExportConst(~name=id |> Ident.name, ~typ, ~config)
      |> export;
      {...env, requires};

    | ConstructorBinding(
        exportType,
        constructorType,
        argTypes,
        variantName,
        recordValue,
      ) =>
      emitExportType(~language, exportType);
      let recordAsInt = recordValue |> Runtime.emitRecordAsInt(~language);
      if (argTypes == []) {
        recordAsInt
        ++ ";"
        |> EmitTyp.emitExportConst(
             ~name=variantName,
             ~typ=constructorType,
             ~config,
           )
        |> export;
      } else {
        let args =
          argTypes
          |> List.mapi((i, typ) => {
               let converter = typ |> typToConverter;
               let arg = EmitText.argi(i + 1);
               let v = arg |> Converter.toReason(~converter);
               (arg, v);
             });
        let mkReturn = s => "return " ++ s;
        let mkBody = args =>
          recordValue
          |> Runtime.emitRecordAsBlock(~language, ~args)
          |> mkReturn;
        EmitText.funDef(~args, ~mkBody, "")
        |> EmitTyp.emitExportConst(
             ~name=variantName,
             ~typ=constructorType,
             ~config,
           )
        |> export;
      };
      {
        ...env,
        requires:
          env.requires
          |> ModuleNameMap.add(
               ModuleName.createBucklescriptBlock,
               ImportPath.bsBlockPath(~config),
             ),
      };

    | ComponentBinding({
        exportType,
        moduleName,
        propsTypeName,
        componentType,
        typ,
      }) =>
      let converter = typ |> typToConverter;
      let importPath =
        ModuleResolver.resolveModule(
          ~config,
          ~outputFileRelative,
          ~resolver,
          ~importExtension=".bs",
          moduleName,
        );
      let moduleNameBs = moduleName |> ModuleName.forBsFile;

      let name = EmitTyp.componentExportName(~language, ~moduleName);
      let jsProps = "jsProps";
      let jsPropsDot = s => jsProps ++ "." ++ s;

      let args =
        switch (converter) {
        | FunctionC((groupedArgConverters, _retConverter)) =>
          switch (groupedArgConverters) {
          | [
              GroupConverter(propConverters),
              ArgConverter(_, childrenConverter),
              ..._,
            ] =>
            (
              propConverters
              |> List.map(((s, argConverter)) =>
                   jsPropsDot(s)
                   |> Converter.apply(~converter=argConverter, ~toJS=false)
                 )
            )
            @ [
              jsPropsDot("children")
              |> Converter.apply(~converter=childrenConverter, ~toJS=false),
            ]

          | [ArgConverter(_, childrenConverter), ..._] => [
              jsPropsDot("children")
              |> Converter.apply(~converter=childrenConverter, ~toJS=false),
            ]

          | _ => [jsPropsDot("children")]
          }

        | _ => [jsPropsDot("children")]
        };

      let checkJsWrapperType = emitCheckJsWrapperType(~env, ~propsTypeName);

      if (checkJsWrapperType != "") {
        let exportTypeNoChildren =
          switch (exportType.typ) {
          | GroupOfLabeledArgs(fields) =>
            switch (fields |> List.rev) {
            | [_child, ...propFieldsRev] =>
              let typNoChildren =
                GroupOfLabeledArgs(propFieldsRev |> List.rev);
              {...exportType, typ: typNoChildren};
            | [] => exportType
            }
          | _ => exportType
          };
        emitExportType(~language, exportTypeNoChildren);
        checkJsWrapperType |> export;
        env;
      } else {
        emitExportType(~language, exportType);
        EmitTyp.emitExportConstMany(
          ~name,
          ~typ=componentType,
          ~config,
          [
            "ReasonReact.wrapReasonForJs(",
            "  " ++ ModuleName.toString(moduleNameBs) ++ ".component" ++ ",",
            "  (function _("
            ++ EmitTyp.ofType(
                 ~language,
                 ~typ=Ident(propsTypeName, []),
                 jsProps,
               )
            ++ ") {",
            "     return "
            ++ ModuleName.toString(moduleNameBs)
            ++ "."
            ++ "make"
            ++ EmitText.parens(args)
            ++ ";",
            "  }));",
          ],
        )
        |> export;

        EmitTyp.emitExportDefault(~config, name) |> export;

        let requiresWithModule =
          moduleNameBs |> requireModule(~requires=env.requires, ~importPath);
        let requiresWithReasonReact =
          requiresWithModule
          |> ModuleNameMap.add(
               ModuleName.reasonReact,
               ImportPath.reasonReactPath(~config),
             );
        {...env, requires: requiresWithReasonReact};
      };

    | ExternalReactClass({componentName, importPath} as externalReactClass) =>
      let requires =
        env.requires
        |> ModuleNameMap.add(
             ModuleName.fromStringUnsafe(componentName),
             importPath,
           );
      {
        ...env,
        requires,
        externalReactClass: [externalReactClass, ...env.externalReactClass],
      };
    };
  };

  let initialEnv = {
    requires: ModuleNameMap.empty,
    externalReactClass: [],
    cmtExportTypeMapCache: StringMap.empty,
    typesFromOtherFiles: StringMap.empty,
  };
  let exportTypeMap = codeItems |> createExportTypeMap(~language);
  let finalEnv =
    codeItems |> List.fold_left(emitCodeItem(~exportTypeMap), initialEnv);

  if (finalEnv.externalReactClass != []) {
    EmitTyp.requireReact(~language) |> require;
  };
  finalEnv.requires
  |> ModuleNameMap.iter((moduleName, importPath) =>
       EmitTyp.emitRequire(~language, moduleName, importPath) |> require
     );

  let requireString = requireBuffer |> Buffer.to_bytes;
  let importTypeString = importTypeBuffer |> Buffer.to_bytes;
  let exportString = exportBuffer |> Buffer.to_bytes;
  let toList = s => s == "" ? [] : [s];

  (requireString |> toList)
  @ (importTypeString |> toList)
  @ (exportString |> toList)
  |> String.concat("\n\n");
};