#!/usr/bin/env python3
"""Master Automated Test Runner for VBAN Receptor E2E Test Suite.

Executes requirement-driven E2E tests across Tiers 1 to 4:
  - Tier 1: Feature Coverage (Protocol, Dequantizers, SPSC Buffer, Drift, Network, Lifecycle, Bit-Perfect, Telemetry)
  - Tier 2: Boundary & Corner Cases (Packet limits, Numeric Extremes, Sequence Wraparound, Buffer Extremes, Drift Limits)
  - Tier 3: Cross-Feature Combinations (Pairwise multi-subsystem interactions)
  - Tier 4: Real-World Application Scenarios (768 kHz audiophile, 44.1 kHz CD, 192 kHz studio master, Wi-Fi jitter burst, multicast)

Exit Codes:
  0: SUCCESS (100% test cases passed)
  1: FAILURE (One or more tests failed)
  2: SETUP/CONFIG ERROR
"""

import argparse
import os
import sys
import time
import unittest
from typing import List, Tuple

# Ensure project root is on sys.path
PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if PROJECT_ROOT not in sys.path:
    sys.path.insert(0, PROJECT_ROOT)

TIER_DIRECTORIES = {
    1: ("Tier 1: Feature Coverage", os.path.join("e2e_tests", "tier1_feature_coverage")),
    2: ("Tier 2: Boundary & Corner Cases", os.path.join("e2e_tests", "tier2_boundary_corner")),
    3: ("Tier 3: Cross-Feature Combinations", os.path.join("e2e_tests", "tier3_cross_feature")),
    4: ("Tier 4: Real-World Application Scenarios", os.path.join("e2e_tests", "tier4_real_world_scenarios")),
}


def create_suite_for_tier(project_root: str, tier_num: int) -> unittest.TestSuite:
    """Discovers and loads all test cases for a specific tier directory."""
    if tier_num not in TIER_DIRECTORIES:
        raise ValueError(f"Unknown tier: {tier_num}")
    tier_name, rel_path = TIER_DIRECTORIES[tier_num]
    tier_path = os.path.join(project_root, rel_path)
    loader = unittest.defaultTestLoader
    return loader.discover(start_dir=tier_path, pattern="test_*.py", top_level_dir=project_root)


def run_e2e_suite(tiers_to_run: List[int], verbosity: int = 1) -> int:
    """Runs tests for specified tiers and generates an executive summary report."""
    base_dir = os.path.abspath(os.path.dirname(__file__))

    print("=" * 80)
    print("  VBAN RECEPTOR FOR ANDROID - END-TO-END VERIFICATION SUITE")
    print("=" * 80)
    print(f"Executing Tiers: {', '.join(str(t) for t in tiers_to_run)}")
    print(f"Working Directory: {base_dir}")
    print("-" * 80)

    total_tests = 0
    total_failures = 0
    total_errors = 0
    total_skipped = 0
    tier_results = []

    start_all_time = time.perf_counter()

    for tier_num in tiers_to_run:
        tier_title, _ = TIER_DIRECTORIES[tier_num]
        print(f"\n>>> Running {tier_title} ...")

        try:
            suite = create_suite_for_tier(PROJECT_ROOT, tier_num)
        except Exception as ex:
            print(f"ERROR: Failed to discover tests for Tier {tier_num}: {ex}")
            return 2

        suite_count = suite.countTestCases()
        if suite_count == 0:
            print(f"WARNING: No tests discovered for Tier {tier_num}")

        runner = unittest.TextTestRunner(verbosity=verbosity, stream=sys.stdout)
        start_tier_time = time.perf_counter()
        result = runner.run(suite)
        tier_duration = time.perf_counter() - start_tier_time

        num_tests = result.testsRun
        num_fails = len(result.failures)
        num_errs = len(result.errors)
        num_skips = len(result.skipped)

        total_tests += num_tests
        total_failures += num_fails
        total_errors += num_errs
        total_skipped += num_skips

        tier_results.append((tier_num, tier_title, num_tests, num_fails, num_errs, tier_duration))

    total_duration = time.perf_counter() - start_all_time

    print("\n" + "=" * 80)
    print("  EXECUTIVE TEST SUMMARY REPORT")
    print("=" * 80)
    print(f"{'Tier':<40} | {'Tests':<6} | {'Failures':<8} | {'Errors':<6} | {'Time (s)':<8}")
    print("-" * 80)
    for t_num, t_title, n_test, n_fail, n_err, duration in tier_results:
        print(f"{t_title:<40} | {n_test:<6} | {n_fail:<8} | {n_err:<6} | {duration:<8.3f}")
    print("-" * 80)
    print(
        f"{'TOTAL':<40} | {total_tests:<6} | {total_failures:<8} | {total_errors:<6} | {total_duration:<8.3f}"
    )
    print("=" * 80)

    if total_failures == 0 and total_errors == 0:
        print(f"\n[PASS] 100% OF END-TO-END TESTS PASSED ({total_tests}/{total_tests} test cases).")
        return 0
    else:
        print(
            f"\n[FAIL] TEST RUN FAILED: {total_failures} failure(s), {total_errors} error(s) out of {total_tests} tests."
        )
        return 1


def main():
    parser = argparse.ArgumentParser(description="VBAN Receptor E2E Test Runner")
    parser.add_argument(
        "--tier",
        type=int,
        choices=[1, 2, 3, 4],
        help="Run only tests from a specific tier (1, 2, 3, or 4)",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Enable verbose test output",
    )
    args = parser.parse_args()

    verbosity = 2 if args.verbose else 1
    tiers_to_run = [args.tier] if args.tier is not None else [1, 2, 3, 4]

    exit_code = run_e2e_suite(tiers_to_run, verbosity=verbosity)
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
