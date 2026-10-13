"""PlatformIO pre-build hook: uploadfs/buildfs always use current dashboard assets."""
import runpy
from pathlib import Path

Import("env")
runpy.run_path(str(Path(env.subst("$PROJECT_DIR")) / "scripts" / "build_dashboard.py"),
               run_name="__main__")
