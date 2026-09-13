#!/usr/bin/env python3
"""
Fake device for local testing — see HANDOFF.md Phase 3
"""

import argparse


def main():
    parser = argparse.ArgumentParser(description="Simulated pager device")
    parser.add_argument("--device-id", required=True, help="Device ID")
    parser.add_argument("--reply", help="Auto-reply message text")

    args = parser.parse_args()

    # TODO(orchestrator): MQTT connect logic in Phase 3
    pass


if __name__ == "__main__":
    main()
