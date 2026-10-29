#!/usr/bin/env python3
# =============================================================================
# GnuChanScreenShoot - build and install (Debian)
# -----------------------------------------------------------------------------
#     python3 makefile.py            build and install the program
#     python3 makefile.py build      compile only
#     python3 makefile.py run        compile and start on the current display
#     python3 makefile.py uninstall  remove the binary and the desktop entry
#
# GnuChanScreenShoot is a C program, built against X11. It grabs two hotkeys
# (Alt+P and Print/SysRq), captures the screen, and writes the image to the
# save directory named in its settings script.
#
# Debian only, on purpose.
#
# License: GPL3
# =============================================================================

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = Path(__file__).resolve().parent
# Build output goes under _temp/, not into the source tree: a build is not a
# change to the project.
BUILD = ROOT.parent.parent / "_temp" / "gcl_SCREENSHOOT-build"

PROGRAM = "GnuChanScreenShoot"
BIN_DIR = Path("/usr/local/bin")
APPS_DIR = Path("/usr/share/applications")
DESKTOP_FILE = APPS_DIR / "gnuchanscreenshot.desktop"

# The settings file, installed into the user's own config directory. It is
# replaced on every install, exactly as the other GnuChan programs' are: the
# installer owns that file, and a machine installing a new build has to get the
# settings that build ships with.
CONFIG_SOURCE = ROOT / "GnuChanScreenShoot_config" / "GnuChanScreenShoot.py"
CONFIG_DIR_NAME = "GnuChanScreenShoot"
CONFIG_FILE_NAME = "GnuChanScreenShoot.py"

SOURCES = (
    "screenshot_config.c",
    "ss_parser.c",
    "GnuChanScreenShoot.c",
)
HEADERS = (
    "screenshot_config.h",
    "ss_parser.h",
)

ELEVATED_VARIABLE = "GNUCHANSCREENSHOOT_ELEVATED"


def step(message: str) -> None:
    print(f"==> {message}", flush=True)


def detail(message: str) -> None:
    print(f"    {message}", flush=True)


def note(message: str) -> None:
    print(message, flush=True)


def run(command: list[str], capture: bool = False,
        environment: dict | None = None, cwd: Path | None = None):
    if capture:
        return subprocess.run(command, check=False, capture_output=True,
                              text=True, env=environment, cwd=cwd)
    return subprocess.run(command, check=False, text=True, env=environment,
                          cwd=cwd)


def apt_environment() -> dict:
    return {**os.environ, "DEBIAN_FRONTEND": "noninteractive"}


def is_root() -> bool:
    geteuid = getattr(os, "geteuid", None)
    return bool(geteuid is not None and geteuid() == 0)


def ensure_root() -> None:
    if is_root():
        return
    if os.environ.get(ELEVATED_VARIABLE) == "1":
        raise SystemExit("error: still not root after sudo")
    sudo = shutil.which("sudo")
    if sudo is None:
        raise SystemExit("error: this installs packages and writes /usr; run as root")
    step("Needs root; re-running through sudo")
    environment = dict(os.environ)
    environment[ELEVATED_VARIABLE] = "1"
    os.execvpe(sudo, [sudo, sys.executable,
                      str(Path(__file__).resolve()), *sys.argv[1:]],
               environment)


def is_debian() -> bool:
    return shutil.which("apt-get") is not None


def header_present(header: str, extra_includes: tuple[str, ...] = ()) -> bool:
    """Whether gcc can preprocess a header, which is how a machine is asked
    whether a development package is installed.

    The check is a real compile of a one-line translation unit rather than a
    test for a file on disk, because where a distribution keeps a header is not
    fixed and what matters is whether the compiler the build will use can find
    it.
    """
    gcc = shutil.which("gcc")
    if gcc is None:
        return False
    command = [gcc, "-E", "-xc", "-"]
    for path in extra_includes:
        command += ["-I", path]
    result = subprocess.run(
        command, input=f"#include <{header}>\n",
        text=True, capture_output=True, check=False,
    )
    return result.returncode == 0


def x11_headers_present() -> bool:
    return header_present("X11/Xlib.h")


def missing_build_dependencies() -> list[str]:
    """Which packages this machine is missing to build the program."""
    needed: list[str] = []
    if shutil.which("gcc") is None:
        needed.append("build-essential")
    if shutil.which("pkg-config") is None:
        needed.append("pkg-config")
    if not x11_headers_present():
        needed.append("libx11-dev")
    if not header_present("png.h"):
        needed.append("libpng-dev")
    if not header_present("jpeglib.h"):
        needed.append("libjpeg-dev")
    return needed


def ensure_build_dependencies() -> None:
    needed = missing_build_dependencies()
    if not needed:
        return
    step("Installing the build dependencies")
    detail("missing: " + ", ".join(needed))
    run(["apt-get", "update", "-o", "Acquire::Retries=3"],
        capture=True, environment=apt_environment())
    command = ["apt-get", "install", "-y", "--no-install-recommends", *needed]
    detail("running: " + " ".join(command))
    if run(command, environment=apt_environment()).returncode != 0:
        raise SystemExit("error: apt-get could not install: " + ", ".join(needed))
    detail("installed: " + ", ".join(needed))


def x11_flags() -> tuple[list[str], list[str]]:
    """The compiler and linker flags for X11."""
    pkg_config = shutil.which("pkg-config")
    if pkg_config is not None:
        cflags = run([pkg_config, "--cflags", "x11"], capture=True)
        libs = run([pkg_config, "--libs", "x11"], capture=True)
        if cflags.returncode == 0 and libs.returncode == 0:
            return cflags.stdout.split(), libs.stdout.split()
    return (["-I/usr/include"], ["-lX11"])


def check_sources() -> None:
    missing = [name for name in (*SOURCES, *HEADERS)
               if not (ROOT / name).is_file()]
    if missing:
        raise SystemExit("error: missing source files: " + ", ".join(missing))


def build() -> Path:
    check_sources()
    ensure_build_dependencies()
    BUILD.mkdir(parents=True, exist_ok=True)
    output = BUILD / PROGRAM

    cflags, libs = x11_flags()
    command = [
        "gcc", "-std=c99", "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-O2", "-D_DEFAULT_SOURCE",
        "-I", str(ROOT),
        *cflags,
    ]
    command += [str(ROOT / name) for name in SOURCES]
    command += ["-o", str(output)]
    command += [*libs, "-lpng", "-ljpeg"]

    step("Building")
    if run(command, cwd=ROOT).returncode != 0:
        raise SystemExit("error: the build failed")
    detail(f"built {output}")
    return output


def desktop_entry() -> str:
    return "\n".join([
        "[Desktop Entry]",
        "Type=Application",
        "Name=GnuChanScreenShoot",
        "Comment=GnuchanOS screenshot tool",
        f"Exec={BIN_DIR / PROGRAM}",
        f"TryExec={BIN_DIR / PROGRAM}",
        "Terminal=false",
        "Categories=System;Graphics;",
        "Icon=accessories-screenshot",
        "",
    ])


def invoking_user() -> tuple[Path, int, int] | None:
    """The home and ids of the person who ran sudo, not root."""
    name = os.environ.get("SUDO_USER")
    if not name:
        return None
    try:
        import pwd
        info = pwd.getpwnam(name)
        return Path(info.pw_dir), info.pw_uid, info.pw_gid
    except (ImportError, KeyError):
        return None


def install(binary: Path) -> None:
    step("Installing")
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    APPS_DIR.mkdir(parents=True, exist_ok=True)

    target = BIN_DIR / PROGRAM
    staged = BIN_DIR / (PROGRAM + ".new")
    shutil.copyfile(binary, staged)
    staged.chmod(0o755)
    os.replace(str(staged), str(target))
    detail(f"installed {target}")

    temporary = APPS_DIR / "gnuchanscreenshot.desktop.new"
    temporary.write_text(desktop_entry(), encoding="utf-8")
    os.replace(str(temporary), str(DESKTOP_FILE))
    DESKTOP_FILE.chmod(0o644)
    detail(f"installed {DESKTOP_FILE}")

    install_config()


def install_config() -> None:
    """Put the settings file where the program reads it.

    The old file is removed first and the shipped one written in its place,
    the same rule the other GnuChan programs' installers follow: the result is
    what this build ships and nothing of what was there before.
    """
    if not CONFIG_SOURCE.is_file():
        return
    user = invoking_user()
    if user is None:
        detail("skipped the config: no login user to install it for")
        return

    home, uid, gid = user
    directory = home / ".config" / CONFIG_DIR_NAME
    file_target = directory / CONFIG_FILE_NAME

    if CONFIG_SOURCE.stat().st_size == 0:
        raise SystemExit(
            f"error: {CONFIG_SOURCE} is empty, so the screenshot tool would "
            f"have nothing to read"
        )

    directory.mkdir(parents=True, exist_ok=True)
    if file_target.exists():
        file_target.unlink()
        detail(f"removed the old {file_target}")

    shutil.copyfile(CONFIG_SOURCE, file_target)
    try:
        os.chown(directory, uid, gid)
        os.chown(file_target, uid, gid)
    except OSError:
        pass
    detail(f"installed {file_target}")


def uninstall() -> None:
    step("Uninstalling")
    for target in (BIN_DIR / PROGRAM, DESKTOP_FILE):
        if target.exists():
            target.unlink()
            detail(f"removed {target}")
    note(f"The settings under ~/.config/{CONFIG_DIR_NAME}/ were left alone.")


def run_program(binary: Path) -> int:
    if not os.environ.get("DISPLAY"):
        raise SystemExit("error: DISPLAY is not set, so there is no X server to run on")
    step("Starting on the current display")
    return run([str(binary)], cwd=ROOT).returncode


def main() -> int:
    if platform.system().lower() != "linux":
        raise SystemExit(
            f"error: GnuChanScreenShoot is a Debian X11 program; this is "
            f"{platform.system()}"
        )
    if not is_debian():
        raise SystemExit(
            "error: this installs with apt-get and writes Debian's system "
            "directories; it is Debian-only by design"
        )

    action = sys.argv[1] if len(sys.argv) > 1 else "install"
    known = ("install", "build", "run", "uninstall")
    if action not in known:
        print(f"error: unknown action '{action}'", file=sys.stderr)
        print("usage: python3 makefile.py [" + "|".join(known) + "]",
              file=sys.stderr)
        return 2

    if action in ("install", "uninstall"):
        ensure_root()

    if action == "uninstall":
        uninstall()
        return 0

    if action == "build":
        binary = build()
        note(f"Built {binary}.")
        return 0

    if action == "run":
        return run_program(build())

    binary = build()
    install(binary)
    note("")
    note("GnuChanScreenShoot is installed. It is in the applications menu, and")
    note(f"{BIN_DIR / PROGRAM} runs it from a shell.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
