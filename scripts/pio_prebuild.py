Import("env")

from pathlib import Path
import subprocess
import sys


project_dir = Path(env["PROJECT_DIR"])
generator = project_dir / "scripts" / "generate_web_ui_header.py"

subprocess.check_call([sys.executable, str(generator)], cwd=str(project_dir))
