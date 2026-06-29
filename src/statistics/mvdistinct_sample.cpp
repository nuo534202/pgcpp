// mvdistinct_sample.cpp — single-column ndistinct estimator and sample helper.
//
// Implements the Haas-Stefanski D1 estimator used by PostgreSQL's
// mvndistinct.c (estimate_ndistinct). The estimator extrapolates the number
// of distinct values in a population from a uniform random sample using the
// number of singletons (values appearing exactly once) in the sample.

#include "statistics/mvdistinct_sample.hpp"

namespace pgcpp::statistics {

// EstimateNDistinct — the D1 estimator from Haas & Stokes (1998).
//   D1 = d + f1 * n / (n - f1)
// where d = distinct values in sample, f1 = singletons, n = sample size.
// The result is then scaled by totalrows/samplerows when the relation is
// larger than the sample. When f1 == 0 the additive term vanishes (the
// sample is "saturated": every observed value recurs, so there is little
// evidence of additional unseen values). Guard against n == f1 (a degenerate
// sample where every value is a singleton) which would divide by zero.
double EstimateNDistinct(double totalrows, double samplerows, double distinct, double f1) {
    double n = samplerows;
    double f1_ndistinct;
    if (distinct == 0.0 || f1 == 0.0 || n <= f1) {
        f1_ndistinct = 0.0;
    } else {
        f1_ndistinct = f1 * n / (n - f1);
    }
    double ndistinct = distinct + f1_ndistinct;
    if (totalrows > 0.0 && samplerows > 0.0) {
        ndistinct *= totalrows / samplerows;
    }
    return ndistinct;
}

// SampleRows — copy the flat value/null arrays into a StatsBuildData.
// The caller retains ownership of the source vectors; the struct holds its
// own copies so it can outlive the caller's buffers.
StatsBuildData SampleRows(const std::vector<AttrNumber>& attnums,
                          const std::vector<types::Datum>& values, const std::vector<bool>& nulls,
                          int nrows) {
    StatsBuildData data;
    data.attnums = attnums;
    data.values = values;
    data.nulls = nulls;
    data.nrows = nrows;
    return data;
}

}  // namespace pgcpp::statistics
