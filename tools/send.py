#!/usr/bin/env python3
"""
CLI to send a message via the relay API — see HANDOFF.md Phase 2
"""

import argparse


def main():
    parser = argparse.ArgumentParser(description="Send a message via the relay")
    parser.add_argument("--device-id", required=True, help="Target device ID")
    parser.add_argument("--body", required=True, help="Message body")

    args = parser.parse_args()

    # TODO(orchestrator): Relay API call logic in Phase 2
    pass


if __name__ == "__main__":
    main()
