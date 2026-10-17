import importlib.util
import pathlib
import unittest

spec = importlib.util.spec_from_file_location(
    "compare", pathlib.Path(__file__).resolve().parents[1] / "benchmarks" / "compare.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class RegressionGateTest(unittest.TestCase):
    def test_repeatable_regression(self):
        rows = module.compare([{"x": 100}] * 7, [{"x": 120}] * 7)
        self.assertEqual(rows[0]["status"], "regression")

    def test_single_outlier_does_not_block(self):
        rows = module.compare([{"x": 100}] * 7, [{"x": 100}] * 6 + [{"x": 500}])
        self.assertEqual(rows[0]["status"], "pass")

    def test_noise_is_not_a_green_result(self):
        runs = [{"x": n} for n in [50, 75, 100, 125, 150, 175, 200]]
        self.assertEqual(module.compare(runs, runs)[0]["status"], "noisy")

    def test_missing_case_fails(self):
        with self.assertRaises(ValueError):
            module.compare([{"x": 100}] * 7, [{"y": 100}] * 7)


if __name__ == "__main__":
    unittest.main()
