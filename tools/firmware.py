"""Build SS Forest firmware and flash multiple devices in parallel."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


def run(command: list[str], cwd: Path, environment: dict[str, str] | None = None) -> None:
    print(f"[{cwd}] {' '.join(command)}", flush=True)
    subprocess.run(command, cwd=cwd, env=environment, check=True)


def project_command(idf: list[str], project: Path, action: str, port: str | None = None) -> list[str]:
    command = [*idf, "-C", str(project), action]
    if port is not None:
        command.extend(["-p", port])
    return command


def flash_one(
    idf: list[str], project: Path, port: str, environment: dict[str, str] | None
) -> tuple[str, bool]:
    label = f"{project} -> {port}"
    try:
        run(project_command(idf, project, "flash", port), project, environment)
    except subprocess.CalledProcessError as error:
        print(f"[{label}] failed with exit code {error.returncode}", file=sys.stderr, flush=True)
        return label, False
    return label, True


def existing_project(value: str) -> Path:
    project = Path(value).expanduser().resolve()
    if not (project / "CMakeLists.txt").is_file():
        raise argparse.ArgumentTypeError(f"not an ESP-IDF project: {project}")
    return project


def default_idf_command() -> list[str]:
    configured = os.environ.get("IDF_PY")
    if configured:
        return configured.split()
    if shutil.which("idf.py"):
        return ["idf.py"]

    idf_root = Path(os.environ.get("IDF_PATH", "/opt/esp-idf"))
    python_env = Path.home() / ".espressif/python_env/idf6.0_py3.14_env/bin/python"
    idf_script = idf_root / "tools/idf.py"
    if python_env.is_file() and idf_script.is_file():
        return [str(python_env), str(idf_script)]
    return ["idf.py"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build SS Forest root/node firmware and flash devices in parallel."
    )
    parser.add_argument(
        "--idf",
        default=None,
        help="idf.py command or absolute path (default: IDF_PY or idf.py)",
    )
    parser.add_argument(
        "--root-path",
        "--root-project",
        dest="root_project",
        type=existing_project,
        default=Path("root").resolve(),
        help="root ESP-IDF project path (default: ./root)",
    )
    parser.add_argument(
        "--node-path",
        "--node-project",
        dest="node_projects",
        type=existing_project,
        action="append",
        default=[],
        help="node ESP-IDF project path; repeat for multiple node builds",
    )
    parser.add_argument("--root-port", help="serial port for the root, for example /dev/ttyUSB0")
    parser.add_argument(
        "--node-port",
        dest="node_ports",
        action="append",
        default=[],
        help="serial port for a node; repeat in the same order as --node-path",
    )
    parser.add_argument("--jobs", type=int, default=4, help="maximum parallel flash jobs (default: 4)")
    parser.add_argument("--no-build", action="store_true", help="skip builds and flash existing binaries")
    parser.add_argument("--build-only", action="store_true", help="build firmware but do not flash")
    args = parser.parse_args()

    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    if args.root_port and not args.root_project:
        parser.error("--root-port requires a root project")
    if len(args.node_ports) != len(args.node_projects):
        parser.error("provide exactly one --node-port for each --node-path")
    if not args.root_port and not args.node_ports and not args.build_only:
        parser.error("provide --root-port/--node-port, or use --build-only")
    return args


def main() -> int:
    args = parse_args()
    idf = args.idf.split() if args.idf else default_idf_command()
    environment = os.environ.copy()
    if len(idf) > 1 and idf[-1].endswith("/tools/idf.py"):
        environment.setdefault("ESP_IDF_VERSION", "6.0.0")
        environment.setdefault("IDF_PYTHON_ENV_PATH", str(Path(idf[0]).parent.parent))
    projects = [args.root_project, *args.node_projects]

    if not args.no_build:
        for project in projects:
            run(project_command(idf, project, "build"), project, environment)

    if args.build_only:
        return 0

    flash_jobs: list[tuple[Path, str]] = []
    if args.root_port:
        flash_jobs.append((args.root_project, args.root_port))
    flash_jobs.extend(zip(args.node_projects, args.node_ports))

    failures = 0
    with ThreadPoolExecutor(max_workers=min(args.jobs, len(flash_jobs))) as executor:
        futures = [executor.submit(flash_one, idf, project, port, environment) for project, port in flash_jobs]
        for future in as_completed(futures):
            label, success = future.result()
            print(f"[{label}] {'OK' if success else 'FAILED'}", flush=True)
            failures += not success

    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
