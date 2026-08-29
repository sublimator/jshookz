/* Source entry shape is compiler-owned; invocation/normal-return law is provider-owned. */

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

function typeName(type: TS): string {
  const symbol = type.aliasSymbol || type.symbol;
  return symbol && typeof symbol.getName === "function" ? symbol.getName() : "";
}

function hasModifier(ts: TS, node: TS, kind: number): boolean {
  return !!node.modifiers?.some((modifier: TS) => modifier.kind === kind);
}

function directEntryDeclaration(
  ts: TS,
  symbol: TS,
  source: TS,
): { node: TS; error?: string } {
  const declarations = symbol.getDeclarations() || [];
  const node = declarations[0] || source;
  const functions = declarations.filter(
    (declaration: TS) =>
      declaration.getSourceFile() === source &&
      ts.isFunctionDeclaration(declaration) &&
      declaration.name?.text === symbol.getName() &&
      hasModifier(ts, declaration, ts.SyntaxKind.ExportKeyword),
  );
  if (
    (symbol.flags & ts.SymbolFlags.Alias) !== 0 ||
    functions.length !== declarations.length ||
    functions.length !== 1 ||
    !functions[0].body
  ) {
    return {
      node,
      error: "must be one direct exported function declaration; aliases, re-exports, variables, and overloads are forbidden",
    };
  }
  return { node: functions[0] };
}

function returnTypeIsNever(ts: TS, checker: TS, declaration: TS): boolean {
  const signature = checker.getSignatureFromDeclaration(declaration);
  if (!signature) return false;
  return !!(checker.getReturnTypeOfSignature(signature).flags & ts.TypeFlags.Never);
}

function hasOneRequiredParameter(checker: TS, declaration: TS): boolean {
  const parameter = declaration.parameters[0];
  const signature = checker.getSignatureFromDeclaration(declaration);
  return !!(
    parameter &&
    !parameter.questionToken &&
    !parameter.initializer &&
    !parameter.dotDotDotToken &&
    signature &&
    signature.minArgumentCount === 1
  );
}

function exactlyCallbackInfo(
  ts: TS,
  checker: TS,
  declaration: TS,
  callbackInfo: TS,
): boolean {
  const parameter = declaration.parameters[0];
  const type = checker.getTypeAtLocation(parameter);
  if (type.flags & (ts.TypeFlags.Any | ts.TypeFlags.Unknown)) return false;
  if (typeName(type) === "Function") return false;
  return (
    checker.isTypeAssignableTo(type, callbackInfo) &&
    checker.isTypeAssignableTo(callbackInfo, type)
  );
}

export function checkEntrySignatures(ts: TS, program: TS, source: TS): string[] {
  const checker = program.getTypeChecker();
  const diagnostics: string[] = [];
  const exported = moduleExports(checker, source);
  const main = exported.find((symbol) => symbol.getName() === "main");
  if (!main) {
    diagnostics.push(
      formatAt(ts, source, "JSH-ENTRY001: Hook module must directly export main"),
    );
    return diagnostics;
  }

  const mainDeclaration = directEntryDeclaration(ts, main, source);
  if (mainDeclaration.error) {
    diagnostics.push(
      formatAt(
        ts,
        mainDeclaration.node,
        `JSH-ENTRY002: exported main ${mainDeclaration.error}`,
      ),
    );
  } else {
    if (mainDeclaration.node.parameters.length !== 0) {
      diagnostics.push(
        formatAt(
          ts,
          mainDeclaration.node,
          "JSH-ENTRY003: exported main must declare exactly zero parameters",
        ),
      );
    }
    if (!returnTypeIsNever(ts, checker, mainDeclaration.node)) {
      diagnostics.push(
        formatAt(
          ts,
          mainDeclaration.node,
          "JSH-ENTRY004: exported main must have checked return type never",
        ),
      );
    }
  }

  const callback = exported.find((symbol) => symbol.getName() === "callback");
  if (!callback) return diagnostics;

  const callbackInfo = findCallbackInfoType(ts, program, checker);
  const callbackDeclaration = directEntryDeclaration(ts, callback, source);
  const node = callbackDeclaration.node;
  if (callbackDeclaration.error) {
    diagnostics.push(
      formatAt(
        ts,
        node,
        `JSH-ENTRY005: exported callback ${callbackDeclaration.error}`,
      ),
    );
    return diagnostics;
  }
  if (!callbackInfo) {
    diagnostics.push(
      formatAt(
        ts,
        node,
        "JSH-ENTRY007: exported callback requires CallbackInfo in the Hook declarations",
      ),
    );
    return diagnostics;
  }

  const parameters = node.parameters;
  if (parameters.length !== 1 || !hasOneRequiredParameter(checker, node)) {
    diagnostics.push(
      formatAt(
        ts,
        node,
        "JSH-ENTRY006: exported callback must declare exactly one required non-default, non-rest parameter",
      ),
    );
  } else if (!exactlyCallbackInfo(ts, checker, node, callbackInfo)) {
    diagnostics.push(
      formatAt(
        ts,
        node,
        "JSH-ENTRY007: exported callback parameter must have checked type CallbackInfo; received " +
          checker.typeToString(checker.getTypeAtLocation(parameters[0])),
      ),
    );
  }

  if (!returnTypeIsNever(ts, checker, node)) {
    diagnostics.push(
      formatAt(
        ts,
        node,
        "JSH-ENTRY008: exported callback must have checked return type never",
      ),
    );
  }
  return diagnostics;
}

export function checkHookImports(
  ts: TS,
  source: TS,
  allowStaticRelativeImports = false,
): string[] {
  const diagnostics: string[] = [];
  const relativeSpecifier = (node: TS): boolean => {
    if (!node || !ts.isStringLiteralLike(node)) return false;
    return node.text.startsWith("./") || node.text.startsWith("../");
  };
  const unwrapTransparentExpression = (original: TS): TS => {
    let node = original;
    while (
      ts.isParenthesizedExpression(node) ||
      ts.isAsExpression(node) ||
      ts.isTypeAssertionExpression(node) ||
      ts.isNonNullExpression(node) ||
      (typeof ts.isSatisfiesExpression === "function" &&
        ts.isSatisfiesExpression(node)) ||
      (typeof ts.isPartiallyEmittedExpression === "function" &&
        ts.isPartiallyEmittedExpression(node))
    ) {
      node = node.expression;
    }
    return node;
  };
  const isCommonJSCallee = (original: TS): boolean => {
    const callee = unwrapTransparentExpression(original);
    if (ts.isIdentifier(callee)) return callee.text === "require";
    return (
      ts.isPropertyAccessExpression(callee) &&
      ts.isIdentifier(unwrapTransparentExpression(callee.expression)) &&
      unwrapTransparentExpression(callee.expression).text === "require"
    );
  };
  const visit = (node: TS) => {
    if (
      ts.isImportDeclaration(node) &&
      node.moduleSpecifier &&
      (!allowStaticRelativeImports || !relativeSpecifier(node.moduleSpecifier))
    ) {
      diagnostics.push(
        formatAt(
          ts,
          node,
          allowStaticRelativeImports
            ? "Hook modules may use only static relative imports; bare module specifiers are forbidden"
            : "JavaScript Hooks must not import modules; use TypeScript for compile-time bundling",
        ),
      );
    } else if (
      ts.isExportDeclaration(node) &&
      node.moduleSpecifier &&
      (!allowStaticRelativeImports || !relativeSpecifier(node.moduleSpecifier))
    ) {
      diagnostics.push(
        formatAt(
          ts,
          node,
          allowStaticRelativeImports
            ? "Hook modules may use only static relative exports; bare module specifiers are forbidden"
            : "JavaScript Hooks must not re-export modules; use TypeScript for compile-time bundling",
        ),
      );
    } else if (
      typeof ts.isImportTypeNode === "function" &&
      ts.isImportTypeNode(node) &&
      node.argument &&
      ts.isLiteralTypeNode(node.argument) &&
      !relativeSpecifier(node.argument.literal)
    ) {
      diagnostics.push(
        formatAt(
          ts,
          node,
          "Hook import types may use only relative module specifiers; bare module specifiers are forbidden",
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
          "Hook modules must use ESM static relative imports; import-equals is forbidden",
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
          "Hook modules must not use dynamic import()",
        ),
      );
    } else if (
      ts.isCallExpression(node) &&
      isCommonJSCallee(node.expression)
    ) {
      diagnostics.push(
        formatAt(
          ts,
          node,
          "Hook modules must not use CommonJS require(); use static relative ESM imports",
        ),
      );
    } else if (
      typeof ts.isMetaProperty === "function" &&
      ts.isMetaProperty(node) &&
      node.keywordToken === ts.SyntaxKind.ImportKeyword
    ) {
      diagnostics.push(
        formatAt(ts, node, "Hook modules must not use import.meta"),
      );
    } else if (
      ts.isModuleDeclaration(node) &&
      (node.flags & ts.NodeFlags.GlobalAugmentation) !== 0
    ) {
      diagnostics.push(
        formatAt(
          ts,
          node,
          "Hook source must not augment ambient globals or Hook API declarations",
        ),
      );
    }
    ts.forEachChild(node, visit);
  };
  visit(source);
  return diagnostics;
}
