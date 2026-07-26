"""Select LilyGo's alternate GC9A01 initialization for older T-QT panels."""

from pathlib import Path
from shutil import copy2

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
tqt_dir = project_dir / "vendor" / "T-QT"
source_dir = tqt_dir / "extras" / "old_panel"
driver_dir = tqt_dir / "lib" / "TFT_eSPI" / "TFT_Drivers"

for filename in ("GC9A01_Init.h", "GC9A01_Rotation.h"):
    copy2(source_dir / filename, driver_dir / filename)

# This particular old-panel batch exposes the full 128x128 area at address
# (0, 0). LilyGo's generic old-panel profile offsets it by (2, 1), leaving
# exactly two dirty columns and one dirty row visible at the top-left edges.
rotation_file = driver_dir / "GC9A01_Rotation.h"
rotation = rotation_file.read_text(encoding="utf-8")
rotation = rotation.replace("colstart = 2;", "colstart = 0;")
rotation = rotation.replace("colstart = 1;", "colstart = 0;")
rotation = rotation.replace("rowstart = 2;", "rowstart = 0;")
rotation = rotation.replace("rowstart = 1;", "rowstart = 0;")
rotation_file.write_text(rotation, encoding="utf-8")

print("T-QT display profile: old_panel, zero CGRAM offset")

# Expose LVGL's bundled Nayuki QR encoder as a tiny standalone PlatformIO
# library, without compiling the complete LVGL framework.
qr_source = tqt_dir / "lib" / "lvgl" / "src" / "extra" / "libs" / "qrcode"
qr_library = project_dir / "lib" / "qrcodegen" / "src"
qr_library.mkdir(parents=True, exist_ok=True)
copy2(qr_source / "qrcodegen.c", qr_library / "qrcodegen.c")
copy2(qr_source / "qrcodegen.h", qr_library / "qrcodegen.h")
