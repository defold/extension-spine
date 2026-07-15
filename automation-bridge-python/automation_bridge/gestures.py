"""Deterministic, dependency-free gesture path generation."""

import math
import random
from typing import Any, Dict, Mapping, Optional, Sequence, Tuple, Union


Point = Tuple[float, float]
Bounds = Tuple[float, float, float, float]
Duration = Union[float, Tuple[float, float], Sequence[float]]


class GestureConstraintError(ValueError):
    """Raised when no generated path can satisfy the requested constraints."""


def _point(value: Sequence[float], name: str) -> Point:
    if len(value) != 2:
        raise ValueError(f"{name} must contain exactly two coordinates")
    point = (float(value[0]), float(value[1]))
    if not all(math.isfinite(coordinate) for coordinate in point):
        raise ValueError(f"{name} coordinates must be finite")
    return point


def _inside(point: Point, bounds: Bounds) -> bool:
    left, top, right, bottom = bounds
    return left <= point[0] <= right and top <= point[1] <= bottom


def _duration_range(duration: Duration) -> Tuple[float, float]:
    if isinstance(duration, (int, float)):
        minimum = maximum = float(duration)
    else:
        if len(duration) != 2:
            raise ValueError("duration range must contain exactly two values")
        minimum, maximum = float(duration[0]), float(duration[1])
    if not math.isfinite(minimum) or not math.isfinite(maximum) or minimum <= 0 or maximum < minimum:
        raise ValueError("duration must be positive and ordered")
    return minimum, maximum


def _offset_range(offset: Union[float, Sequence[float]]) -> Tuple[float, float]:
    if isinstance(offset, (int, float)):
        value = abs(float(offset))
        return -value, value
    if len(offset) != 2:
        raise ValueError("lateral_offset must contain exactly two values")
    minimum, maximum = float(offset[0]), float(offset[1])
    if not math.isfinite(minimum) or not math.isfinite(maximum) or maximum < minimum:
        raise ValueError("lateral_offset must be finite and ordered")
    return minimum, maximum


def _kinematics(points: Sequence[Point], durations: Sequence[float]) -> Tuple[float, float]:
    velocities = []
    maximum_velocity = 0.0
    for index, segment_duration in enumerate(durations):
        dx = points[index + 1][0] - points[index][0]
        dy = points[index + 1][1] - points[index][1]
        velocity = (dx / segment_duration, dy / segment_duration)
        velocities.append(velocity)
        maximum_velocity = max(maximum_velocity, math.hypot(*velocity))
    maximum_acceleration = 0.0
    for index in range(1, len(velocities)):
        transition_time = (durations[index - 1] + durations[index]) / 2.0
        acceleration = math.hypot(
            velocities[index][0] - velocities[index - 1][0],
            velocities[index][1] - velocities[index - 1][1],
        ) / transition_time
        maximum_acceleration = max(maximum_acceleration, acceleration)
    return maximum_velocity, maximum_acceleration


class GestureGenerator:
    """Generate replayable drag paths and record them in an active client trace."""

    def __init__(self, client: Optional[Any] = None):
        self._client = client

    def generate_drag(
        self,
        start: Sequence[float],
        target: Sequence[float],
        *,
        seed: int,
        duration: Duration = (0.7, 0.8),
        easing: str = "linear",
        lateral_offset: Union[float, Sequence[float]] = (-35.0, 35.0),
        control_points: int = 2,
        bounds: Optional[Sequence[float]] = None,
        max_velocity: Optional[float] = None,
        max_acceleration: Optional[float] = None,
    ) -> Dict[str, Any]:
        """Return `drag_path` kwargs with deterministic points and timings.

        Velocity is measured per returned segment. Acceleration is the finite
        difference between adjacent segment velocities. Constraints therefore
        describe the emitted samples, independently of a native easing curve.
        """
        start_point = _point(start, "start")
        target_point = _point(target, "target")
        if not isinstance(seed, int):
            raise TypeError("seed must be an int")
        if not isinstance(control_points, int) or control_points < 0:
            raise ValueError("control_points must be a non-negative int")
        if control_points > 128:
            raise ValueError("control_points must not exceed 128")
        minimum_duration, maximum_duration = _duration_range(duration)
        minimum_offset, maximum_offset = _offset_range(lateral_offset)
        parsed_bounds: Optional[Bounds] = None
        if bounds is not None:
            if len(bounds) != 4:
                raise ValueError("bounds must be (left, top, right, bottom)")
            parsed_bounds = tuple(float(value) for value in bounds)  # type: ignore[assignment]
            if not all(math.isfinite(value) for value in parsed_bounds) or parsed_bounds[2] < parsed_bounds[0] or parsed_bounds[3] < parsed_bounds[1]:
                raise ValueError("bounds must be finite and ordered")
            if not _inside(start_point, parsed_bounds) or not _inside(target_point, parsed_bounds):
                raise GestureConstraintError("start and target must lie inside bounds")
        for name, value in (("max_velocity", max_velocity), ("max_acceleration", max_acceleration)):
            if value is not None and (not math.isfinite(value) or value <= 0):
                raise ValueError(f"{name} must be positive and finite")

        rng = random.Random(seed)
        total_duration = rng.uniform(minimum_duration, maximum_duration)
        dx = target_point[0] - start_point[0]
        dy = target_point[1] - start_point[1]
        distance = math.hypot(dx, dy)
        normal = (0.0, 0.0) if distance == 0 else (-dy / distance, dx / distance)
        sampled_offsets = [rng.uniform(minimum_offset, maximum_offset) for _ in range(control_points)]

        # Reduce lateral variation deterministically until every requested
        # geometric and kinematic constraint holds. The straight path is the
        # final candidate and makes impossible duration constraints explicit.
        scale = 1.0
        candidate = None
        for _ in range(25):
            points = [start_point]
            for index, sampled_offset in enumerate(sampled_offsets, start=1):
                progress = index / (control_points + 1.0)
                envelope = math.sin(math.pi * progress)
                offset = sampled_offset * envelope * scale
                points.append((start_point[0] + dx * progress + normal[0] * offset, start_point[1] + dy * progress + normal[1] * offset))
            points.append(target_point)
            lengths = [math.dist(points[index], points[index + 1]) for index in range(len(points) - 1)]
            total_length = sum(lengths)
            durations = ([total_duration / len(lengths)] * len(lengths) if total_length == 0 else [total_duration * length / total_length for length in lengths])
            velocity, acceleration = _kinematics(points, durations)
            in_bounds = parsed_bounds is None or all(_inside(point, parsed_bounds) for point in points)
            if in_bounds and (max_velocity is None or velocity <= max_velocity + 1e-9) and (max_acceleration is None or acceleration <= max_acceleration + 1e-9):
                candidate = (points, durations, velocity, acceleration)
                break
            scale *= 0.5
        if candidate is None:
            raise GestureConstraintError(
                "duration, endpoints, and bounds cannot satisfy the requested velocity/acceleration constraints"
            )

        points, durations, velocity, acceleration = candidate
        # Keep the returned mapping directly splattable into
        # `bridge.drag_path(**gesture)`; generation metadata goes to the trace.
        result: Dict[str, Any] = {
            "points": [[point[0], point[1]] for point in points],
            "durations": durations,
            "easing": [easing] * len(durations),
        }
        trace_value = dict(result)
        trace_value["generation"] = {
                "duration": sum(durations),
                "max_velocity": velocity,
                "max_acceleration": acceleration,
                "bounds": list(parsed_bounds) if parsed_bounds else None,
                "control_points": control_points,
                "seed": seed,
        }
        if self._client is not None and hasattr(self._client, "_trace_record"):
            self._client._trace_record("generated_path", trace_value)
        return result
