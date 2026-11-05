"""项目根目录启动脚本：uv run python run_server.py"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "src"))

from main.server import main  # noqa: E402

if __name__ == "__main__":
    main()
