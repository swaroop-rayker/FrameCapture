"""On-screen overlay surfaces (M9.6).

The pill and the toast notifications float over whatever is being recorded, which makes
one property non-negotiable for everything in this package: **nothing here may appear in
the output file**, and the way it must not appear is not "hidden behind a black
rectangle" either. `exclusion` is how that is achieved, and it is the only thing in this
package that touches Win32.
"""

from .exclusion import ExclusionSupport, exclude_from_capture, guard, install_popup_guard, probe

__all__ = ["ExclusionSupport", "exclude_from_capture", "guard", "install_popup_guard", "probe"]
