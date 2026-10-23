"""Shared input/output locations for the boundary scripts.

Every value can be overridden with an environment variable:
  SPIKE_ROOT  managed assembly set (plain IL PEs). Either a flat directory of *.dll/*.exe
              (as written by tools/sprx_pe.py) or a directory with common/ and Application/.
  SPIKE_TAG   label used in output/cache file names (fw403, fw700, ...).
  SPIKE_OUT   output directory for json/txt/pkl results.
  SPIKE_KEK   root of the decrypted PS5 filesystem dump (system/, system_ex/).
"""
import os

_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

ROOT = os.environ.get("SPIKE_ROOT", os.path.join(_REPO, "work", "il403"))
TAG = os.environ.get("SPIKE_TAG", "fw403")
OUT = os.environ.get("SPIKE_OUT", os.path.join(_REPO, "work", "boundary"))
KEK = os.environ.get("SPIKE_KEK", "")

# PS5 native images of the ShellUI process (FW 4.03 dump layout)
COMMON_EX_LIB = os.path.join(KEK, "system_ex", "common_ex", "lib")
SHELLUI_APP = os.path.join(KEK, "system_ex", "app", "NPXS40087")
EBOOT = os.path.join(SHELLUI_APP, "eboot.bin")
PSM_APPLICATION = os.path.join(SHELLUI_APP, "psm", "Application")
SYSTEM_LIB_DIRS = [os.path.join(KEK, "system", "common", "lib"),
                   os.path.join(KEK, "system", "priv", "lib"),
                   COMMON_EX_LIB]


def out(name):
    """path of an output file; creates the output directory on first use"""
    os.makedirs(OUT, exist_ok=True)
    return os.path.join(OUT, name)
