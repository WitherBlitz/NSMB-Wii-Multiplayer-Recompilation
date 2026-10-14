"""Select an input-specific translator profile without weakening the shared SHA pin.

Use only in a PRIVATE workspace containing your own game files:

    python projects/nsmbw/tools/prepare_input_profile.py game

Output: projects/nsmbw/recomp.local.yml (ignored by git).
It can be passed to the Translator.Cli via --project. The original recomp.yml
remains unchanged. This does not guarantee address compatibility for an alternate DOL.
"""
import hashlib
import sys
from pathlib import Path

from unpack_game import MAIN_DOL_SHA256, ALTERNATE_MAIN_DOL_SHA256, check_dol_layout

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GAME = ROOT.parents[2] / "game"
PROFILE = ROOT / "recomp.local.yml"
BASE = ROOT / "recomp.yml"


def prepare(game_dir: Path) -> Path:
    dol_path = game_dir / "main.dol"
    raw = dol_path.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest not in (MAIN_DOL_SHA256, ALTERNATE_MAIN_DOL_SHA256):
        raise ValueError(f"Unrecognized SHA-256 for SMNE01 revision 1: {digest}")
    check_dol_layout(raw)
    source = BASE.read_text(encoding="utf-8-sig")
    expected = "sha256: " + MAIN_DOL_SHA256
    if source.count(expected) != 1:
        raise ValueError("Upstream recomp.yml SHA-256 pin has changed")
    if digest == MAIN_DOL_SHA256:
        print("Standard DOL, use:", BASE)
        return BASE
    PROFILE.write_text(source.replace(expected, "sha256: " + digest), encoding="utf-8")
    print("Alternate verified DOL. Private translator profile:", PROFILE)
    print("WARNING: the translator's existing function map and HLE hooks must be")
    print("validated against this DOL before distributing or running the APK.")
    return PROFILE


if __name__ == "__main__":
    game = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_GAME
    try:
        prepare(game)
    except (OSError, ValueError) as ex:
        sys.exit(f"Cannot prepare native translation profile: {ex}")
