"""Dependency-free coordinate mapping helpers for the UI runner."""


def desktop_coordinate(x: int | float, y: int | float, scale: float,
                       monitor: dict[str, int]) -> tuple[int, int]:
    """Map model coordinates in the scaled monitor image to desktop pixels."""
    return (monitor["left"] + int(x / scale),
            monitor["top"] + int(y / scale))
