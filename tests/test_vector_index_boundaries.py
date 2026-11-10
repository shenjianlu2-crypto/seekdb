import unittest

from minivectordb import VectorIndex


class VectorIndexBoundaryTests(unittest.TestCase):
    def test_filtered_search_never_returns_excluded_ids(self):
        index = VectorIndex(2)
        index.add(1, [0.0, 0.0])
        index.add(2, [1.0, 0.0])
        index.add(3, [2.0, 0.0])

        self.assertEqual([], index.search([0.0, 0.0], 3, candidate_ids=set()))
        self.assertEqual([2], [key for key, _ in index.search(
            [0.0, 0.0], 3, candidate_ids={2}
        )])

    def test_top_k_distance_ties_use_primary_key(self):
        index = VectorIndex(2)
        index.add(8, [1.0, 0.0])
        index.add(3, [-1.0, 0.0])
        index.add(5, [0.0, 1.0])

        self.assertEqual([3], [key for key, _ in index.search([0.0, 0.0], 1)])


if __name__ == "__main__":
    unittest.main()
