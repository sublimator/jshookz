/* Exported callback must accept CallbackInfo. main is called with no args. */

type TS = any;

function formatAt(ts: TS, node: TS, detail: string): string {
  const file = node.getSourceFile ? node.getSourceFile() : undefined;
  if (!file) return detail;
  const { line, character } = file.getLineAndCharacterOfPosition(
    node.getStart(file),
  );
  return `${file.fileName}(${line + 1},${character + 1}): ${detail}`;
}

function findCallbackInfoType(ts: TS, program: TS, checker: TS): TS {
  for (const file of program.getSourceFiles()) {
    const resolved =
      typeof checker.resolveName === "function"
        ? checker.resolveName(
            "CallbackInfo",
            file,
            ts.SymbolFlags.Type,
            false,
          )
        : undefined;
    if (resolved) return checker.getDeclaredTypeOfSymbol(resolved);
    for (const statement of file.statements) {
      if (
        (ts.isInterfaceDeclaration(statement) ||
          ts.isTypeAliasDeclaration(statement)) &&
        statement.name.text === "CallbackInfo"
      ) {
        return checker.getTypeAtLocation(statement);
      }
    }
  }
  return undefined;
}

function moduleExports(checker: TS, source: TS): TS[] {
  const symbol = source.symbol || checker.getSymbolAtLocation(source);
  if (!symbol) return [];
  return checker.getExportsOfModule(symbol);
}

function declarationOf(symbol: TS): TS {
  const declarations = symbol.getDeclarations() || [];
  return declarations[0];
}

function isRestParameter(parameter: TS): boolean {
  const declaration = parameter.valueDeclaration;
  return !!(declaration && declaration.dotDotDotToken);
}

function parameterType(checker: TS, parameter: TS, source: TS): TS {
  if (parameter.valueDeclaration) {
    return checker.getTypeOfSymbolAtLocation(
      parameter,
      parameter.valueDeclaration,
    );
  }
  return checker.getTypeOfSymbolAtLocation(parameter, source);
}

function acceptedType(ts: TS, checker: TS, parameter: TS, source: TS): TS {
  const type = parameterType(checker, parameter, source);
  if (!isRestParameter(parameter)) return type;
  if (checker.isTupleType && checker.isTupleType(type)) {
    const element = checker.getTypeArguments(type)[0];
    if (element) return element;
  }
  if (checker.getIndexTypeOfType) {
    const index = checker.getIndexTypeOfType(type, ts.IndexKind.Number);
    if (index) return index;
  }
  const indexed = checker.getIndexInfoOfType
    ? checker.getIndexInfoOfType(type, ts.IndexKind.Number)
    : undefined;
  if (indexed && indexed.type) return indexed.type;
  return type;
}

function typeName(type: TS): string {
  const symbol = type.aliasSymbol || type.symbol;
  return symbol && typeof symbol.getName === "function" ? symbol.getName() : "";
}

function isUnverifiedCallable(ts: TS, checker: TS, type: TS): boolean {
  if (type.flags & (ts.TypeFlags.Any | ts.TypeFlags.Unknown)) return true;
  if (typeName(type) === "Function") return true;
  const apparent = checker.getApparentType ? checker.getApparentType(type) : type;
  return typeName(apparent) === "Function";
}

export function checkEntrySignatures(ts: TS, program: TS, source: TS): string[] {
  const checker = program.getTypeChecker();
  const diagnostics: string[] = [];
  const exported = moduleExports(checker, source);
  const callback = exported.find((symbol) => symbol.getName() === "callback");
  if (!callback) return diagnostics;

  const callbackInfo = findCallbackInfoType(ts, program, checker);
  const node = declarationOf(callback) || source;
  if (!callbackInfo) {
    diagnostics.push(
      formatAt(
        ts,
        node,
        "exported callback requires CallbackInfo in the Hook declarations",
      ),
    );
    return diagnostics;
  }

  const type = checker.getTypeOfSymbolAtLocation(callback, source);
  const signatures = checker.getSignaturesOfType(type, ts.SignatureKind.Call);
  if (signatures.length === 0) {
    if (isUnverifiedCallable(ts, checker, type)) {
      diagnostics.push(
        formatAt(
          ts,
          node,
          "exported callback must accept CallbackInfo; parameter type is " +
            checker.typeToString(type),
        ),
      );
    }
    return diagnostics;
  }

  for (const signature of signatures) {
    const parameters = signature.getParameters();
    if (parameters.length === 0) continue;
    const first = parameters[0];
    const accepted = acceptedType(ts, checker, first, source);
    if (checker.isTypeAssignableTo(callbackInfo, accepted)) continue;
    diagnostics.push(
      formatAt(
        ts,
        node,
        "exported callback must accept CallbackInfo; parameter type is " +
          checker.typeToString(accepted),
      ),
    );
  }
  return diagnostics;
}

export function checkHookImports(ts: TS, source: TS): string[] {
  const diagnostics: string[] = [];
  const visit = (node: TS) => {
    if (ts.isImportDeclaration(node) && node.moduleSpecifier) {
      diagnostics.push(
        formatAt(
          ts,
          node,
          "Hook source must not import helpers; export main and optional callback from this file",
        ),
      );
    } else if (ts.isExportDeclaration(node) && node.moduleSpecifier) {
      diagnostics.push(
        formatAt(
          ts,
          node,
          "Hook source must not import helpers; export main and optional callback from this file",
        ),
      );
    } else if (
      ts.isImportEqualsDeclaration(node) &&
      node.moduleReference &&
      ts.isExternalModuleReference(node.moduleReference)
    ) {
      diagnostics.push(
        formatAt(
          ts,
          node,
          "Hook source must not import helpers; export main and optional callback from this file",
        ),
      );
    } else if (
      ts.isCallExpression(node) &&
      node.expression.kind === ts.SyntaxKind.ImportKeyword
    ) {
      diagnostics.push(
        formatAt(
          ts,
          node,
          "Hook source must not import helpers; export main and optional callback from this file",
        ),
      );
    }
    ts.forEachChild(node, visit);
  };
  visit(source);
  return diagnostics;
}
