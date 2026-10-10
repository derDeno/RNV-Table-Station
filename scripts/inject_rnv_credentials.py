Import("env")

import json
import os
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

client_secret = os.environ.get("RNV_CLIENT_SECRET")
env_file = project_dir / ".env"
if client_secret is None and env_file.exists():
    for line in env_file.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator and key.strip() == "clientSecret":
            client_secret = value.strip().strip("\"'")
            break

values = {
    "RNV_CLIENT_ID": "96dd88de-1601-44cc-830a-c5c24d064563",
    "RNV_TENANT_ID": "87cd3c4f-1e0a-4350-889e-3969cd4616c9",
    "RNV_CLIENT_SECRET": client_secret or "",
    "RNV_RESOURCE": "1484212f-edce-452a-aae9-46141bae91af",
    "RNV_API_URL": "https://graphql-sandbox-dds.rnv-online.de",
}
header = "#pragma once\n" + "".join(
    f"#define {macro} {json.dumps(value)}\n"
    for macro, value in values.items()
)
build_dir.mkdir(parents=True, exist_ok=True)
(build_dir / "rnv_credentials.h").write_text(header, encoding="utf-8")
env.Append(CPPPATH=[str(build_dir)])
