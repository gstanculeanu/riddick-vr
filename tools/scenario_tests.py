"""Run the automated VR smoke scenario or validate captured manual scenarios."""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "tests" / "scenarios" / "manifest.json"
HOOK_SOURCE = ROOT / "src" / "hook" / "camera_hook.cpp"
HOST_SOURCE = ROOT / "src" / "host" / "openxr_manager.cpp"
MOCK_SCENARIOS = ROOT / "tools" / "scenarios"


def load_manifest():
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if data.get("version") != 1 or not isinstance(data.get("scenarios"), list):
        raise ValueError("unsupported or malformed scenario manifest")
    scenarios = data["scenarios"]
    ids = [item.get("id") for item in scenarios]
    if not all(ids) or len(ids) != len(set(ids)):
        raise ValueError("scenario IDs must be present and unique")
    for scenario in scenarios:
        if scenario.get("mode") not in ("automated", "scripted", "manual"):
            raise ValueError(f"{scenario.get('id')}: mode must be automated, scripted, or manual")
        if not isinstance(scenario.get("steps"), list) or not scenario["steps"]:
            raise ValueError(f"{scenario.get('id')}: at least one step is required")
        if not isinstance(scenario.get("markers", []), list) or not all(
            isinstance(marker, str) and marker for marker in scenario.get("markers", [])
        ):
            raise ValueError(f"{scenario.get('id')}: markers must be non-empty strings")
    source = HOOK_SOURCE.read_text(encoding="utf-8")
    for scenario in scenarios:
        for marker in scenario.get("markers", []):
            if marker not in source:
                raise ValueError(f"{scenario['id']}: marker is not present in hook source: {marker}")
    for path in sorted(MOCK_SCENARIOS.glob("*.json")):
        profile = json.loads(path.read_text(encoding="utf-8"))
        if profile.get("name") != path.stem or not isinstance(profile.get("keys"), list) or not profile["keys"]:
            raise ValueError(f"{path.name}: expected matching name and non-empty keys")
        for field in ("markers", "markers_any", "forbidden_markers"):
            value = profile.get(field, [])
            if not isinstance(value, list) or not all(isinstance(m, str) and m for m in value):
                raise ValueError(f"{path.name}: {field} must be a list of non-empty strings")
        if not profile.get("markers") and not profile.get("markers_any"):
            raise ValueError(f"{path.name}: expected non-empty markers or markers_any")
        # forbidden_markers are matched against the log only, not the hook source
        # (the logged line may be built from a format string).
        for marker in profile.get("markers", []) + profile.get("markers_any", []):
            if marker not in source:
                raise ValueError(f"{path.name}: marker not found in hook source: {marker}")
        previous_frame = -1
        required = {"frame", "lx", "ly", "lz", "lgrip", "ltrig", "lbtn",
                    "rx", "ry", "rz", "rgrip", "rtrig", "rbtn"}
        for index, key in enumerate(profile["keys"]):
            if not isinstance(key, dict) or not required.issubset(key):
                raise ValueError(f"{path.name}: keyframe {index} is missing required fields")
            frame = key["frame"]
            if not isinstance(frame, int) or frame <= previous_frame:
                raise ValueError(f"{path.name}: keyframe frames must be increasing integers")
            previous_frame = frame
    host_source = HOST_SOURCE.read_text(encoding="utf-8")
    if "gripPose.linearVelocity" not in host_source or "90.0f / frameSpan" not in host_source:
        raise ValueError("mock host must derive gesture velocities from scenario keyframe movement")
    profile_ids = {path.stem for path in MOCK_SCENARIOS.glob("*.json")}
    for scenario in scenarios:
        if scenario["mode"] == "scripted":
            profile = scenario.get("profile", scenario["id"])
            if profile not in profile_ids:
                raise ValueError(f"{scenario['id']}: missing scripted profile {profile}.json")
    return {item["id"]: item for item in scenarios}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate the scenario manifest")
    parser.add_argument("--list", action="store_true", help="list available scenarios")
    parser.add_argument("--run", metavar="ID", help="run an automated scenario")
    parser.add_argument("--validate", metavar="ID", help="validate manual scenario evidence")
    parser.add_argument("--log", type=Path, help="hook log containing the scenario run")
    parser.add_argument("--since-byte", type=int, default=0, help="ignore log content before this byte offset")
    parser.add_argument("--artifacts", type=Path, help="directory containing captured BMP/PNG frames")
    parser.add_argument("--frame-since", type=float, default=0.0,
                        help="Unix timestamp; ignore artifacts older than this time")
    parser.add_argument("--frames", type=int, default=360)
    parser.add_argument("--interval", type=int, default=15)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    try:
        scenarios = load_manifest()
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"[scenarios] invalid manifest: {exc}", file=sys.stderr)
        return 1

    if args.check:
        profile_count = len(list(MOCK_SCENARIOS.glob("*.json")))
        print(f"[scenarios] manifest valid ({len(scenarios)} test cases, {profile_count} scripted profiles)")
        if not args.list and not args.run and not args.validate:
            return 0

    if args.list:
        for scenario in scenarios.values():
            print(f"{scenario['id']:16} {scenario['mode']:10} {scenario['title']}")
        return 0

    if args.run:
        scenario = scenarios.get(args.run)
        if not scenario:
            parser.error(f"unknown scenario: {args.run}")
        if scenario["mode"] == "manual":
            parser.error(f"{args.run} is a manual scenario; follow its checklist and use --validate")
        profile = scenario.get("profile", args.run)
        command = [
            sys.executable, str(ROOT / "tools" / "semi_play.py"),
            "--scenario", profile,
            "--frames", str(args.frames),
            "--interval", str(args.interval),
            "--timeout", str(args.timeout),
        ]
        return subprocess.run(command, cwd=ROOT).returncode

    if args.validate:
        scenario = scenarios.get(args.validate)
        if not scenario:
            parser.error(f"unknown scenario: {args.validate}")
        if not args.log or not args.artifacts:
            parser.error("--validate requires both --log and --artifacts")
        try:
            log_data = args.log.read_bytes()[args.since_byte:]
        except OSError as exc:
            print(f"[scenarios] cannot read hook log: {exc}", file=sys.stderr)
            return 1
        log_text = log_data.decode(errors="replace")
        missing = [marker for marker in scenario["markers"] if marker not in log_text]
        frames = list(args.artifacts.glob("*.bmp")) + list(args.artifacts.glob("*.png"))
        frames = [frame for frame in frames if frame.is_file() and frame.stat().st_size > 0
                  and frame.stat().st_mtime >= args.frame_since]
        print(f"[scenarios] {scenario['id']}: {len(frames)} frame artifact(s), {len(scenario['markers']) - len(missing)}/{len(scenario['markers'])} marker(s)")
        if missing:
            print("[scenarios] missing markers:")
            for marker in missing:
                print(f"  - {marker}")
        if not frames:
            print("[scenarios] no non-empty frame artifacts found")
        return 1 if missing or not frames else 0

    if not args.check:
        parser.print_help()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
