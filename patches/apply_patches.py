Import("env")
import shutil, os

# Patch ST7789_Rotation.h to add CGRAM_OFFSET support for 76×284 panel (ST7789P3).
# TFT_eSPI's built-in offsets only cover widths 135, 172, 170 and height 280.
# Without this patch, a 76-wide display falls into the else branch (offset=0,0)
# which writes to wrong GRAM addresses, producing horizontal line artifacts.
src = os.path.join(env.subst("$PROJECT_DIR"), "patches", "ST7789_Rotation.h")
dst = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"),
                   "TFT_eSPI", "TFT_Drivers", "ST7789_Rotation.h")
if os.path.isfile(dst):
    shutil.copy2(src, dst)
    print("patches/apply_patches.py: patched ST7789_Rotation.h")
