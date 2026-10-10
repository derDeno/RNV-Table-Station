Import("env")

import json
from pathlib import Path


toolchain = Path(env.PioPlatform().get_package_dir("toolchain-riscv32-esp") or "")
if not (toolchain / "bin").is_dir():
    from platformio.project.config import ProjectConfig

    core_dir = ProjectConfig.get_instance().get("platformio", "core_dir")
    toolchain = Path(core_dir) / "tools" / "toolchain-riscv32-esp"
if (toolchain / "bin").is_dir():
    env.PrependENVPath("PATH", str(toolchain / "bin"))


project_dir = Path(env.subst("$PROJECT_DIR"))
build_dir = Path(env.subst("$BUILD_DIR"))
if not build_dir.is_absolute():
    build_dir = project_dir / build_dir

values = {}
env_file = project_dir / ".env"
if env_file.exists():
    for line in env_file.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator and key.strip() and not key.lstrip().startswith("#"):
            values[key.strip()] = value.strip().strip("\"'")

macros = {
    "RNV_CLIENT_ID": "clientID",
    "RNV_TENANT_ID": "tenantID",
    "RNV_CLIENT_SECRET": "clientSecret",
    "RNV_RESOURCE": "resource",
    "RNV_API_URL": "hostname",
}
header = "#pragma once\n" + "".join(
    f"#define {macro} {json.dumps(values.get(key, ''))}\n"
    for macro, key in macros.items()
)
build_dir.mkdir(parents=True, exist_ok=True)
(build_dir / "rnv_credentials.h").write_text(header, encoding="utf-8")
env.Append(CPPPATH=[str(build_dir)])
