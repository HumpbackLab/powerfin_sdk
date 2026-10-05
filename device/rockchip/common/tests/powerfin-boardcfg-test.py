#!/usr/bin/env python3
"""Check full-update boardcfg reset policy without accessing hardware."""
import os
from pathlib import Path
import subprocess
import tempfile


script = Path(__file__).resolve().parents[1] / "scripts/mk-updateimg.sh"
functions = script.read_text().split("do_build_updateimg()", 1)[0]
cases = [
    ("update", "parameter-powerfin-spinor.txt", True),
    ("update-ab", "parameter-powerfin-spinor.txt", True),
    ("update", "parameter-powerfin-spinor-amp.txt", True),
    ("update-ota", "parameter-powerfin-spinor.txt", False),
    ("update-ab-ota", "parameter-powerfin-spinor.txt", False),
    ("update", "parameter-other-board.txt", False),
]
for image_type, parameter, reset in cases:
    with tempfile.TemporaryDirectory() as directory:
        staging = Path(directory)
        original = "boot boot.img\nboardcfg custom.img\n"
        (staging / "custom-package").write_text(original)
        (staging / "package-file").symlink_to("custom-package")
        (staging / "existing-env").write_bytes(b"preserve source")
        (staging / "boardcfg.img").symlink_to("existing-env")
        subprocess.run(
            ["bash", "-e", "-c", functions +
             '\nnotice() { :; }\nprepare_powerfin_boardcfg "$1"',
             "test", image_type],
            cwd=staging,
            env={**os.environ, "RK_SDK_DIR": str(staging),
                 "RK_PARAMETER": parameter},
            check=True,
        )
        assert (staging / "custom-package").read_text() == original
        assert (staging / "existing-env").read_bytes() == b"preserve source"
        if reset:
            assert not (staging / "boardcfg.img").is_symlink()
            assert (staging / "boardcfg.img").read_bytes() == b"\xff" * 65536
            assert (staging / "package-file").read_text() == (
                "boot boot.img\nboardcfg\tboardcfg.img\n"
            )
        else:
            assert (staging / "boardcfg.img").is_symlink()
            assert (staging / "package-file").is_symlink()
            assert (staging / "package-file").read_text() == original

print("PowerFin boardcfg reset policy and source preservation tests passed")
