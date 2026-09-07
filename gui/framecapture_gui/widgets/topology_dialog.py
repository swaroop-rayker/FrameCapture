"""What `get_gpu_topology` reports, on screen (SPEC.md §5.1).

The command has existed since M2 and no part of the GUI has ever shown it. That matters
more here than the feature's size suggests: SPEC.md §5.1's hybrid-GPU trap -- the display
is wired to the iGPU on a MUX-less laptop, so a capture device created on the dGPU
returns black frames with `S_OK` -- is diagnosed by exactly one fact, which adapter owns
the output, and until now a user could not see it without reading the log file.

So the column that matters is `owns_output`, and it is stated in words rather than left
as a tick a reader has to interpret.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QHeaderView,
    QLabel,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)


class GpuTopologyDialog(QDialog):
    """A table of adapters, or a sentence saying why there isn't one."""

    def __init__(self, topology: Mapping[str, Any] | None, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("GPU topology")
        self.resize(560, 260)

        layout = QVBoxLayout(self)

        adapters = self._rows(topology)
        self._summary = QLabel(self._headline(topology, adapters))
        self._summary.setWordWrap(True)
        layout.addWidget(self._summary)

        self._table = QTableWidget(len(adapters), 3, self)
        self._table.setHorizontalHeaderLabels(["Adapter", "LUID", "Display output"])
        self._table.verticalHeader().setVisible(False)
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._table.setSelectionMode(QTableWidget.SelectionMode.NoSelection)
        header = self._table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)

        for row, adapter in enumerate(adapters):
            self._table.setItem(row, 0, QTableWidgetItem(str(adapter.get("description", "unnamed adapter"))))
            luid = QTableWidgetItem(str(adapter.get("luid", "")))
            luid.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            self._table.setItem(row, 1, luid)
            # In words. "yes"/"no" under a header a reader has already parsed beats a
            # tick they have to work out the polarity of, and this is the field the
            # black-frame defect turns on.
            self._table.setItem(row, 2, QTableWidgetItem("owns it" if adapter.get("owns_output") else "no"))

        self._table.setVisible(bool(adapters))
        layout.addWidget(self._table)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.reject)
        buttons.accepted.connect(self.accept)
        layout.addWidget(buttons)

    @staticmethod
    def _rows(topology: Mapping[str, Any] | None) -> list[Mapping[str, Any]]:
        if topology is None:
            return []
        return [entry for entry in (topology.get("adapters") or []) if isinstance(entry, Mapping)]

    @staticmethod
    def _headline(topology: Mapping[str, Any] | None, adapters: list[Mapping[str, Any]]) -> str:
        if topology is None:
            return "The engine did not answer, so there is no topology to show. Try Tools ▸ Engine ▸ Restart engine."
        if not adapters:
            return "The engine reported no adapters. Recording will not be possible until one is present."
        owners = [str(a.get("description", "")) for a in adapters if a.get("owns_output")]
        if not owners:
            # Possible, and worth saying plainly: it is the shape of SPEC.md §5.1's
            # black-frame case seen from outside.
            return (
                f"{len(adapters)} adapter(s). None reports a display output — on a hybrid-GPU laptop "
                "this is the condition that makes capture return black frames."
            )
        return f"{len(adapters)} adapter(s). Capture and encode happen on the adapter that owns the display output."
