from pathlib import Path

from hostem import HookRunner


def test_hostem_executes_nested_typescript_graph_as_one_hook(tmp_path: Path):
    helpers = tmp_path / "helpers"
    helpers.mkdir()
    (helpers / "constants.ts").write_text(
        "export const CODE = 83 as const;\n"
        'export const MESSAGE = "bundled hostem" as const;\n'
    )
    (helpers / "policy.ts").write_text(
        'import { CODE, MESSAGE } from "./constants";\n'
        "let initialized = 0;\n"
        "initialized += 1;\n"
        "export function finish(): never {\n"
        "  accept(`${MESSAGE}:${initialized}`, CODE);\n"
        "}\n"
    )
    hook = tmp_path / "bundled.hook.ts"
    hook.write_text(
        'import { finish } from "./helpers/policy";\n'
        "export function main(): never { finish(); }\n"
    )

    runner = HookRunner()
    result = runner.run_file(hook)

    assert result.accepted, result.error
    assert result.return_code == 83
    assert result.return_msg == b"bundled hostem:1"
    assert [call.name for call in result.call_log] == ["accept"]
