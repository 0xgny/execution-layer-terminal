"""PyInstaller entry point for the feedhandler.

PyInstaller freezes a script, not a package, so `python -m feedhandler` has no
direct equivalent. This is that module's __main__ under a name PyInstaller can
point at. Only used by scripts/make_dmg.sh; running the feedhandler from a
source checkout still goes through `python -m feedhandler`.
"""

from feedhandler.__main__ import main

if __name__ == "__main__":
    main()
